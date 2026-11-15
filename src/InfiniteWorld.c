#include "InfiniteWorld.h"
#include "BlockID.h"
#include "Platform.h"
#include "World.h"
#include "Stream.h"
#include "Errors.h"

/* Integer-only value noise keeps terrain identical across platforms and visits.
   Coordinates are absolute block coordinates, not relative to the loaded area.
*/
static cc_uint32 InfiniteWorld_Hash(int seed, int x, int z) {
	cc_uint32 h = (cc_uint32)seed;
	h ^= (cc_uint32)x * 0x9E3779B1u;
	h ^= (cc_uint32)z * 0x85EBCA77u;
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

static int InfiniteWorld_FloorDiv(int value, int divisor) {
	int q = value / divisor;
	if (value < 0 && (value % divisor)) q--;
	return q;
}

static int InfiniteWorld_Smooth(int t) {
	cc_uint64 value = (cc_uint64)t * (cc_uint64)t;
	return (int)((value * (cc_uint64)(3 * 65536 - 2 * t)) >> 32);
}

static int InfiniteWorld_Lerp(int a, int b, int t) {
	return a + (((b - a) * t) >> 16);
}

static int InfiniteWorld_Noise(int seed, int x, int z, int scale) {
	int gx = InfiniteWorld_FloorDiv(x, scale);
	int gz = InfiniteWorld_FloorDiv(z, scale);
	cc_int64 tx = (cc_int64)x - (cc_int64)gx * scale;
	cc_int64 tz = (cc_int64)z - (cc_int64)gz * scale;
	int fx = (int)((tx << 16) / scale);
	int fz = (int)((tz << 16) / scale);
	int a, b, c, d, top, bottom;

	fx = InfiniteWorld_Smooth(fx);
	fz = InfiniteWorld_Smooth(fz);
	a = (int)(InfiniteWorld_Hash(seed, gx,     gz)     & 255);
	b = (int)(InfiniteWorld_Hash(seed, gx + 1, gz)     & 255);
	c = (int)(InfiniteWorld_Hash(seed, gx,     gz + 1) & 255);
	d = (int)(InfiniteWorld_Hash(seed, gx + 1, gz + 1) & 255);

	top    = InfiniteWorld_Lerp(a, b, fx);
	bottom = InfiniteWorld_Lerp(c, d, fx);
	return InfiniteWorld_Lerp(top, bottom, fz) - 128;
}

static cc_bool InfiniteWorld_IsDesert(int seed, int x, int z) {
	return InfiniteWorld_Noise(seed ^ 0x3C6EF372u, x, z, 256) > 70;
}

static cc_bool InfiniteWorld_IsSnowy(int seed, int x, int z, int surface) {
	int temperature = InfiniteWorld_Noise(seed ^ 0xA54FF53Au, x, z, 192);
	return surface >= 38 && temperature < -24;
}

/* Broad forest regions add a distinct vegetation density without changing
   the underlying terrain height or the placement of neighbouring chunks. */
static cc_bool InfiniteWorld_IsForest(int seed, int x, int z) {
	return InfiniteWorld_Noise(seed ^ 0x1B873593u, x, z, 160) > 20;
}

/* Low terrain near the waterline forms sandy beaches and shallow seabeds. */
static cc_bool InfiniteWorld_IsBeach(int surface) {
	return surface >= INFINITE_WORLD_WATER_LEVEL - 3 &&
		surface <= INFINITE_WORLD_WATER_LEVEL + 2;
}

static int InfiniteWorld_SurfaceHeight(int seed, int x, int z) {
	int broad = InfiniteWorld_Noise(seed, x, z, 96);
	int detail = InfiniteWorld_Noise(seed ^ 0x51ED270Bu, x, z, 24);
	int height = 30 + (broad * 12) / 128 + (detail * 4) / 128;

	if (height < 8) height = 8;
	if (height > 55) height = 55;
	return height;
}


/* C integer division truncates toward zero, so adjust negative nonmultiples
   to obtain mathematical floor division. */
int InfiniteWorld_ChunkCoord(int blockCoord) {
	int chunk = blockCoord / INFINITE_WORLD_CHUNK_SIZE;
	if (blockCoord < 0 && (blockCoord % INFINITE_WORLD_CHUNK_SIZE)) chunk--;
	return chunk;
}

int InfiniteWorld_LocalCoord(int blockCoord) {
	int local = blockCoord % INFINITE_WORLD_CHUNK_SIZE;
	return local < 0 ? local + INFINITE_WORLD_CHUNK_SIZE : local;
}

/* Trees are derived from absolute X/Z coordinates, including a two-block halo,
   so canopies crossing chunk boundaries are generated consistently. */
static void InfiniteWorld_GenerateTrees(int seed, int chunkX, int chunkZ, BlockRaw* blocks) {
	cc_int64 baseX = (cc_int64)chunkX * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 baseZ = (cc_int64)chunkZ * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 xEnd = baseX + INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 zEnd = baseZ + INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 anchorX, anchorZ, leafX, leafZ;
	for (anchorZ = baseZ - 2; anchorZ <= zEnd + 1; anchorZ++) {
		for (anchorX = baseX - 2; anchorX <= xEnd + 1; anchorX++) {
			cc_uint32 hash;
			int x, z, surface, height, top, y, radius, index;
			cc_bool forest;
			if (anchorX < Int32_MinValue || anchorX > Int32_MaxValue ||
				anchorZ < Int32_MinValue || anchorZ > Int32_MaxValue) continue;
			x = (int)anchorX; z = (int)anchorZ;
			hash = InfiniteWorld_Hash(seed ^ 0x2C9277B5u, x, z);
			surface = InfiniteWorld_SurfaceHeight(seed, x, z);
			if (surface <= INFINITE_WORLD_WATER_LEVEL || InfiniteWorld_IsDesert(seed, x, z) ||
				InfiniteWorld_IsBeach(surface)) continue;
			forest = InfiniteWorld_IsForest(seed, x, z);
			if ((hash & 255u) >= (forest ? 10u : 2u)) continue;
			height = forest ? 6 + (int)((hash >> 8) % 4u) : 5 + (int)((hash >> 8) % 3u);
			top = surface + height;
			if (top + 1 >= INFINITE_WORLD_HEIGHT) continue;
			/* Trunks may replace leaves from an overlapping tree. */
			if (anchorX >= baseX && anchorX < xEnd && anchorZ >= baseZ && anchorZ < zEnd) {
				for (y = surface + 1; y < top; y++) {
					index = y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE
						+ (z - (int)baseZ) * INFINITE_WORLD_CHUNK_SIZE + (x - (int)baseX);
					if (blocks[index] == BLOCK_AIR || blocks[index] == BLOCK_LEAVES) blocks[index] = BLOCK_LOG;
				}
			}
			for (y = top - 2; y <= top + 1; y++) {
				radius = y >= top ? 1 : 2;
				for (leafZ = anchorZ - radius; leafZ <= anchorZ + radius; leafZ++) {
					for (leafX = anchorX - radius; leafX <= anchorX + radius; leafX++) {
						if (leafX < baseX || leafX >= xEnd || leafZ < baseZ || leafZ >= zEnd ||
							y < 0 || y >= INFINITE_WORLD_HEIGHT) continue;
						x = (int)leafX; z = (int)leafZ;
						index = y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE
							+ (z - (int)baseZ) * INFINITE_WORLD_CHUNK_SIZE + (x - (int)baseX);
						if (blocks[index] == BLOCK_AIR) blocks[index] = BLOCK_LEAVES;
					}
				}
			}
		}
	}
}
static cc_int64 InfiniteWorld_FloorDiv64(cc_int64 value, int divisor);

/* Flower patches are anchored to an absolute 16x16 world grid. Each chunk
   checks a small halo of patch origins, so a cluster crossing a chunk edge
   appears identically regardless of which chunk is generated first. */
static void InfiniteWorld_GenerateFlowers(int seed, int chunkX, int chunkZ, BlockRaw* blocks) {
	cc_int64 baseX = (cc_int64)chunkX * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 baseZ = (cc_int64)chunkZ * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 endX = baseX + INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 endZ = baseZ + INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 gx, gz;
	cc_int64 minGX = InfiniteWorld_FloorDiv64(baseX - 4, 16);
	cc_int64 maxGX = InfiniteWorld_FloorDiv64(endX + 2, 16);
	cc_int64 minGZ = InfiniteWorld_FloorDiv64(baseZ - 4, 16);
	cc_int64 maxGZ = InfiniteWorld_FloorDiv64(endZ + 2, 16);
	int attempt;
	for (gz = minGZ; gz <= maxGZ; gz++) {
		for (gx = minGX; gx <= maxGX; gx++) {
			cc_uint32 anchorHash;
			cc_int64 anchorX, anchorZ;
			if (gx < -134217728LL || gx > 134217727LL ||
				gz < -134217728LL || gz > 134217727LL) continue;
			anchorHash = InfiniteWorld_Hash(seed ^ 0x6A09E667u, (int)gx, (int)gz);
			anchorX = gx * 16 + (anchorHash & 15u);
			anchorZ = gz * 16 + ((anchorHash >> 4) & 15u);
			for (attempt = 0; attempt < 8; attempt++) {
				cc_uint32 offsetHash = anchorHash ^ (cc_uint32)(attempt * 0x85EBCA77u);
				cc_int64 candidateX = anchorX + (int)(offsetHash & 7u) - 3;
				cc_int64 candidateZ = anchorZ + (int)((offsetHash >> 8) & 7u) - 3;
				cc_uint32 hash;
				int worldX, worldZ, surface, y, x, z, index;
				if (candidateX < Int32_MinValue || candidateX > Int32_MaxValue ||
					candidateZ < Int32_MinValue || candidateZ > Int32_MaxValue) continue;
				if (candidateX < baseX || candidateX >= endX || candidateZ < baseZ || candidateZ >= endZ) continue;
				worldX = (int)candidateX; worldZ = (int)candidateZ;
				if (attempt >= (InfiniteWorld_IsForest(seed, worldX, worldZ) ? 4 : 2)) continue;
				hash = InfiniteWorld_Hash(seed ^ (int)(0xBB67AE85u + attempt * 0x9E3779B9u), worldX, worldZ);
				surface = InfiniteWorld_SurfaceHeight(seed, worldX, worldZ);
				if (surface <= INFINITE_WORLD_WATER_LEVEL ||
					InfiniteWorld_IsDesert(seed, worldX, worldZ) ||
					InfiniteWorld_IsBeach(surface) ||
					InfiniteWorld_IsSnowy(seed, worldX, worldZ, surface)) continue;
				y = surface + 1;
				if (y >= INFINITE_WORLD_HEIGHT) continue;
				x = worldX - (int)baseX; z = worldZ - (int)baseZ;
				index = y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE
					+ z * INFINITE_WORLD_CHUNK_SIZE + x;
				if (blocks[index] != BLOCK_AIR ||
					blocks[index - INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE] != BLOCK_GRASS) continue;
				blocks[index] = (hash & 0x100u) ? BLOCK_ROSE : BLOCK_DANDELION;
			}
		}
	}
}
/* Rare mushrooms grow on stone floors inside dry cave sections. Their
   placement is derived from world coordinates, not chunk load order. */
static void InfiniteWorld_GenerateMushrooms(int seed, int chunkX, int chunkZ, BlockRaw* blocks) {
	cc_int64 baseX = (cc_int64)chunkX * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 baseZ = (cc_int64)chunkZ * INFINITE_WORLD_CHUNK_SIZE;
	int x, y, z, worldX, worldZ, surface, index, plane = INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE;
	cc_uint32 hash;
	for (z = 0; z < INFINITE_WORLD_CHUNK_SIZE; z++) {
		worldZ = (int)(baseZ + z);
		for (x = 0; x < INFINITE_WORLD_CHUNK_SIZE; x++) {
			worldX = (int)(baseX + x);
			surface = InfiniteWorld_SurfaceHeight(seed, worldX, worldZ);
			for (y = 2; y < surface - 2 && y < INFINITE_WORLD_HEIGHT - 1; y++) {
				index = y * plane + z * INFINITE_WORLD_CHUNK_SIZE + x;
				if (blocks[index] != BLOCK_AIR || blocks[index - plane] != BLOCK_STONE ||
					blocks[index + plane] != BLOCK_AIR) continue;
				hash = InfiniteWorld_Hash(seed ^ (int)(0x9E3779B9u * (cc_uint32)y), worldX, worldZ);
				if ((hash & 1023u) != 0u) continue;
				blocks[index] = (hash & 2048u) ? BLOCK_RED_SHROOM : BLOCK_BROWN_SHROOM;
			}
		}
	}
}
/* Ore clusters use a fixed 8-block 3D grid. Each chunk evaluates the
   neighbouring cluster centres too, keeping veins continuous at edges. */
static cc_int64 InfiniteWorld_FloorDiv64(cc_int64 value, int divisor) {
	cc_int64 q = value / divisor;
	if (value < 0 && value % divisor) q--;
	return q;
}

static void InfiniteWorld_GenerateOres(int seed, int chunkX, int chunkZ, BlockRaw* blocks) {
	cc_int64 baseX = (cc_int64)chunkX * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 baseZ = (cc_int64)chunkZ * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 endX = baseX + INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 endZ = baseZ + INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 gx, gz;
	cc_int64 minGX = InfiniteWorld_FloorDiv64(baseX - 3, 8);
	cc_int64 maxGX = InfiniteWorld_FloorDiv64(endX + 2, 8);
	cc_int64 minGZ = InfiniteWorld_FloorDiv64(baseZ - 3, 8);
	cc_int64 maxGZ = InfiniteWorld_FloorDiv64(endZ + 2, 8);
	int gy;
	for (gy = 0; gy < 8; gy++) {
		int centerY = gy * 8 + 4;
		for (gz = minGZ; gz <= maxGZ; gz++) {
			for (gx = minGX; gx <= maxGX; gx++) {
				cc_uint32 hash = InfiniteWorld_Hash(seed ^ (int)((cc_uint32)gy * 0x9E3779B9u), (int)gx, (int)gz);
				cc_int64 centerX = gx * 8 + 4;
				cc_int64 centerZ = gz * 8 + 4;
				BlockRaw ore;
				int radius, dx, dy, dz;
				cc_uint32 roll = hash % 100u;
				if (roll < 18u) {
					ore = BLOCK_COAL_ORE; radius = 1 + (int)((hash >> 8) % 2u);
				} else if (roll < 25u) {
					ore = BLOCK_IRON_ORE; radius = 1 + (int)((hash >> 8) % 2u);
				} else if (roll < 28u) {
					ore = BLOCK_GOLD_ORE; radius = 1;
				} else {
					continue;
				}
				for (dy = -radius; dy <= radius; dy++) {
					int y = centerY + dy;
					if (y <= 0 || y >= INFINITE_WORLD_HEIGHT) continue;
					for (dz = -radius; dz <= radius; dz++) {
						cc_int64 worldZ = centerZ + dz;
						if (worldZ < baseZ || worldZ >= endZ) continue;
						for (dx = -radius; dx <= radius; dx++) {
							cc_int64 worldX = centerX + dx;
							int index;
							if (worldX < baseX || worldX >= endX) continue;
							if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
							index = y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE
								+ (int)(worldZ - baseZ) * INFINITE_WORLD_CHUNK_SIZE + (int)(worldX - baseX);
							if (blocks[index] == BLOCK_STONE) blocks[index] = ore;
						}
					}
				}
			}
		}
	}
}
/* Cave worms are seeded by a fixed 32x32 world grid. Every chunk evaluates
   nearby worms from their origins, so tunnels continue across chunk edges. */
static void InfiniteWorld_GenerateCaves(int seed, int chunkX, int chunkZ, BlockRaw* blocks) {
	cc_int64 baseX = (cc_int64)chunkX * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 baseZ = (cc_int64)chunkZ * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 endX = baseX + INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 endZ = baseZ + INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 minGX = InfiniteWorld_FloorDiv64(baseX - 114, 32);
	cc_int64 maxGX = InfiniteWorld_FloorDiv64(endX + 82, 32);
	cc_int64 minGZ = InfiniteWorld_FloorDiv64(baseZ - 114, 32);
	cc_int64 maxGZ = InfiniteWorld_FloorDiv64(endZ + 82, 32);
	cc_int64 gx, gz;

	for (gz = minGZ; gz <= maxGZ; gz++) {
		for (gx = minGX; gx <= maxGX; gx++) {
			cc_uint32 origin = InfiniteWorld_Hash(seed ^ 0x6D2B79F5u, (int)gx, (int)gz);
			cc_int64 caveX = gx * 32 + (origin & 31u);
			cc_int64 caveZ = gz * 32 + ((origin >> 5) & 31u);
			int caveY = 7 + (int)((origin >> 10) % 42u);
			int length = 40 + (int)((origin >> 18) % 41u);
			int vx = 0, vy = 0, vz = 0;
			int step;

			for (step = 0; step < length; step++) {
				cc_uint32 h = InfiniteWorld_Hash(seed ^ (int)(0x9E3779B9u * (cc_uint32)(step + 1)),
					(int)gx, (int)gz);
				int dx = (int)(h % 3u) - 1;
				int dy = (int)((h >> 2) % 3u) - 1;
				int dz = (int)((h >> 4) % 3u) - 1;
				int radius = (step > 0 && (step % 16) == 0 && ((h >> 12) & 3u) == 0u)
					? 3 : 1 + (int)(((h >> 8) % 3u) == 0u);
				int ox, oy, oz;

				vx += dx; if (vx < -3) vx = -3; if (vx > 3) vx = 3;
				vy += dy; if (vy < -2) vy = -2; if (vy > 2) vy = 2;
				vz += dz; if (vz < -3) vz = -3; if (vz > 3) vz = 3;
				caveX += vx / 2;
				caveZ += vz / 2;
				if (vy > 0) caveY++; else if (vy < 0) caveY--;
				if (caveY < 5) caveY = 5;
				if (caveY > 53) caveY = 53;

				if ((h & 7u) == 0u) continue;
				/* Most nearby cave worms never enter this chunk. Reject their
				   bounding spheres before visiting individual carve voxels. */
				if (caveX + radius < baseX || caveX - radius >= endX ||
					caveZ + radius < baseZ || caveZ - radius >= endZ) continue;
				for (oy = -radius; oy <= radius; oy++) {
					int y = caveY + oy;
					if (y <= 0 || y >= INFINITE_WORLD_HEIGHT) continue;
					for (oz = -radius; oz <= radius; oz++) {
						cc_int64 worldZ = caveZ + oz;
						if (worldZ < baseZ || worldZ >= endZ) continue;
						for (ox = -radius; ox <= radius; ox++) {
							cc_int64 worldX = caveX + ox;
							int index;
							if (worldX < baseX || worldX >= endX) continue;
							if (ox * ox + oy * oy + oz * oz > radius * radius) continue;
							index = y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE
								+ (int)(worldZ - baseZ) * INFINITE_WORLD_CHUNK_SIZE + (int)(worldX - baseX);
							if (blocks[index] == BLOCK_STONE || blocks[index] == BLOCK_DIRT || blocks[index] == BLOCK_SANDSTONE) {
								/* Low cave sections intersect the deterministic aquifer table. */
								blocks[index] = y <= 5 ? BLOCK_STILL_LAVA : (y <= 12 ? BLOCK_STILL_WATER : BLOCK_AIR);
							}
						}
					}
				}
			}
		}
	}
}

cc_bool InfiniteWorld_GenerateChunk(int seed, int chunkX, int chunkZ, BlockRaw* blocks) {
	int x, y, z, worldX, worldZ, surface, index;
	cc_bool desert, beach, snowy;
	cc_uint32 surfaceHash;
	cc_int64 baseX = (cc_int64)chunkX * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 baseZ = (cc_int64)chunkZ * INFINITE_WORLD_CHUNK_SIZE;

	if (!blocks) return false;
	if (baseX < Int32_MinValue || baseX + (INFINITE_WORLD_CHUNK_SIZE - 1) > Int32_MaxValue) return false;
	if (baseZ < Int32_MinValue || baseZ + (INFINITE_WORLD_CHUNK_SIZE - 1) > Int32_MaxValue) return false;

	for (z = 0; z < INFINITE_WORLD_CHUNK_SIZE; z++) {
		worldZ = (int)(baseZ + z);
		for (x = 0; x < INFINITE_WORLD_CHUNK_SIZE; x++) {
			worldX = (int)(baseX + x);
			surface = InfiniteWorld_SurfaceHeight(seed, worldX, worldZ);
			desert = InfiniteWorld_IsDesert(seed, worldX, worldZ);
			beach = !desert && InfiniteWorld_IsBeach(surface);
			surfaceHash = InfiniteWorld_Hash(seed ^ 0xBB67AE85u, worldX, worldZ);
			snowy = !desert && !beach && InfiniteWorld_IsSnowy(seed, worldX, worldZ, surface);

			for (y = 0; y < INFINITE_WORLD_HEIGHT; y++) {
				index = (y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE) + (z * INFINITE_WORLD_CHUNK_SIZE) + x;
				if (y == 0) {
					blocks[index] = BLOCK_BEDROCK;
				} else if (y < surface - 3) {
					blocks[index] = BLOCK_STONE;
				} else if (y < surface) {
					blocks[index] = (desert || beach) ? BLOCK_SANDSTONE : BLOCK_DIRT;
				} else if (y == surface) {
					blocks[index] = desert ? BLOCK_SAND :
						(beach && (surfaceHash & 7u) == 0u) ? BLOCK_GRAVEL :
						beach ? BLOCK_SAND : BLOCK_GRASS;
				} else if (y == surface + 1 && snowy) {
					blocks[index] = BLOCK_SNOW;
				} else if (y <= INFINITE_WORLD_WATER_LEVEL) {
					blocks[index] = BLOCK_STILL_WATER;
				} else {
					blocks[index] = BLOCK_AIR;
				}
			}
		}
	}
	InfiniteWorld_GenerateCaves(seed, chunkX, chunkZ, blocks);
	InfiniteWorld_GenerateOres(seed, chunkX, chunkZ, blocks);
	InfiniteWorld_GenerateTrees(seed, chunkX, chunkZ, blocks);
	InfiniteWorld_GenerateFlowers(seed, chunkX, chunkZ, blocks);
	InfiniteWorld_GenerateMushrooms(seed, chunkX, chunkZ, blocks);
	return true;
}

/* Modified chunks stay resident until saved. Generated-only chunk block arrays
   use a bounded cache; their terrain can always be regenerated from the seed. */
struct InfiniteWorld_ChunkEdit {
	cc_uint16 index, block;
};

struct InfiniteWorld_Chunk {
	int x, z;
	cc_bool modified, compressionBlocked;
	cc_uint64 lastUsed;
	BlockRaw* blocks;
#ifdef EXTENDED_BLOCKS
	BlockRaw* blocks2;
#endif
	struct InfiniteWorld_ChunkEdit* edits;
	cc_uint32 editCount;
};

static struct InfiniteWorld_Chunk* InfiniteWorld_Chunks;
static int InfiniteWorld_ChunksCount, InfiniteWorld_ChunksCapacity;
static cc_uint32* InfiniteWorld_ChunkSlots;
static cc_uint32 InfiniteWorld_ChunkSlotsCapacity;
static int InfiniteWorld_Seed;
static cc_bool InfiniteWorld_Active;
static cc_uint64 InfiniteWorld_AccessCounter;
static int InfiniteWorld_ResidentChunks;
static int InfiniteWorld_NextCompactAt = 4096;
/* Keep the full 21x21x4 renderer window resident, with room for border reads. */
#define INFINITE_WORLD_MAX_RESIDENT_CHUNKS 2048

/* Rebuilds a dense chunk from its deterministic base terrain and sparse edits. */
static cc_bool InfiniteWorld_CreateChunkData(int chunkX, int chunkZ,
		const struct InfiniteWorld_ChunkEdit* edits, cc_uint32 editCount,
		BlockRaw** blocksOut, BlockRaw** blocks2Out) {
	BlockRaw* blocks;
	BlockRaw* blocks2 = NULL;
	cc_uint32 i;
	*blocksOut = NULL;
	*blocks2Out = NULL;
	blocks = (BlockRaw*)Mem_TryAlloc(INFINITE_WORLD_CHUNK_VOLUME, sizeof(BlockRaw));
	if (!blocks) return false;
	if (!InfiniteWorld_GenerateChunk(InfiniteWorld_Seed, chunkX, chunkZ, blocks)) {
		Mem_Free(blocks);
		return false;
	}
#ifdef EXTENDED_BLOCKS
	for (i = 0; i < editCount; i++) {
		if (edits[i].block > 255) {
			blocks2 = (BlockRaw*)Mem_TryAllocCleared(INFINITE_WORLD_CHUNK_VOLUME, sizeof(BlockRaw));
			if (!blocks2) {
				Mem_Free(blocks);
				return false;
			}
			break;
		}
	}
#endif
	for (i = 0; i < editCount; i++) {
		cc_uint32 index = edits[i].index;
		if (index >= INFINITE_WORLD_CHUNK_VOLUME) {
			Mem_Free(blocks);
			Mem_Free(blocks2);
			return false;
		}
		blocks[index] = (BlockRaw)edits[i].block;
#ifdef EXTENDED_BLOCKS
		if (blocks2) blocks2[index] = (BlockRaw)(edits[i].block >> 8);
#endif
	}
	*blocksOut = blocks;
	*blocks2Out = blocks2;
	return true;
}

static cc_bool InfiniteWorld_MaterializeChunk(struct InfiniteWorld_Chunk* chunk) {
	BlockRaw* blocks, *blocks2;
	if (!InfiniteWorld_CreateChunkData(chunk->x, chunk->z, chunk->edits, chunk->editCount, &blocks, &blocks2)) return false;
	chunk->blocks = blocks;
#ifdef EXTENDED_BLOCKS
	chunk->blocks2 = blocks2;
#endif
	Mem_Free(chunk->edits);
	chunk->edits = NULL;
	chunk->editCount = 0;
	chunk->compressionBlocked = false;
	InfiniteWorld_ResidentChunks++;
	return true;
}

/* Converts a modified chunk to a compact list of differences from its seed terrain.
   Dense edits remain resident; sparse edits can be reconstructed on demand. */
static cc_bool InfiniteWorld_CompressModifiedChunk(struct InfiniteWorld_Chunk* chunk) {
	BlockRaw* base;
	struct InfiniteWorld_ChunkEdit* edits;
	cc_uint32 i, count = 0, out = 0;
	cc_uint64 denseBytes;
#ifdef EXTENDED_BLOCKS
	cc_bool hasUpper = false;
#endif
	base = (BlockRaw*)Mem_TryAlloc(INFINITE_WORLD_CHUNK_VOLUME, sizeof(BlockRaw));
	if (!base) return false;
	if (!InfiniteWorld_GenerateChunk(InfiniteWorld_Seed, chunk->x, chunk->z, base)) {
		Mem_Free(base);
		return false;
	}
	for (i = 0; i < INFINITE_WORLD_CHUNK_VOLUME; i++) {
		cc_uint16 block = chunk->blocks[i];
#ifdef EXTENDED_BLOCKS
		if (chunk->blocks2) block |= (cc_uint16)(chunk->blocks2[i] << 8);
		if (block > 255) hasUpper = true;
#endif
		if (block != (cc_uint16)base[i]) count++;
	}
	if (!count) {
		Mem_Free(base);
		Mem_Free(chunk->blocks);
#ifdef EXTENDED_BLOCKS
		Mem_Free(chunk->blocks2);
		chunk->blocks2 = NULL;
#endif
		Mem_Free(chunk->edits);
		chunk->edits = NULL;
		chunk->editCount = 0;
		chunk->blocks = NULL;
		chunk->modified = false;
		chunk->compressionBlocked = false;
		InfiniteWorld_ResidentChunks--;
		return true;
	}
	denseBytes = (cc_uint64)INFINITE_WORLD_CHUNK_VOLUME * sizeof(BlockRaw);
#ifdef EXTENDED_BLOCKS
	if (hasUpper) denseBytes += (cc_uint64)INFINITE_WORLD_CHUNK_VOLUME * sizeof(BlockRaw);
#endif
	if ((cc_uint64)count * sizeof(struct InfiniteWorld_ChunkEdit) >= (denseBytes * 3) / 4) {
		Mem_Free(base);
		chunk->compressionBlocked = true;
		return false;
	}
	edits = (struct InfiniteWorld_ChunkEdit*)Mem_TryAlloc(count, sizeof(struct InfiniteWorld_ChunkEdit));
	if (!edits) {
		Mem_Free(base);
		return false;
	}
	for (i = 0; i < INFINITE_WORLD_CHUNK_VOLUME; i++) {
		cc_uint16 block = chunk->blocks[i];
#ifdef EXTENDED_BLOCKS
		if (chunk->blocks2) block |= (cc_uint16)(chunk->blocks2[i] << 8);
#endif
		if (block == (cc_uint16)base[i]) continue;
		edits[out].index = (cc_uint16)i;
		edits[out].block = block;
		out++;
	}
	Mem_Free(base);
	Mem_Free(chunk->blocks);
#ifdef EXTENDED_BLOCKS
	Mem_Free(chunk->blocks2);
	chunk->blocks2 = NULL;
#endif
	chunk->blocks = NULL;
	Mem_Free(chunk->edits);
	chunk->edits = edits;
	chunk->editCount = count;
	chunk->compressionBlocked = false;
	InfiniteWorld_ResidentChunks--;
	return true;
}

void InfiniteWorld_Reset(void) {
	int i;
	for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
		Mem_Free(InfiniteWorld_Chunks[i].blocks);
#ifdef EXTENDED_BLOCKS
		Mem_Free(InfiniteWorld_Chunks[i].blocks2);
#endif
		Mem_Free(InfiniteWorld_Chunks[i].edits);
	}
	Mem_Free(InfiniteWorld_Chunks);
	Mem_Free(InfiniteWorld_ChunkSlots);
	InfiniteWorld_ChunkSlots = NULL;
	InfiniteWorld_ChunkSlotsCapacity = 0;
	InfiniteWorld_Chunks = NULL;
	InfiniteWorld_ChunksCount = 0;
	InfiniteWorld_ChunksCapacity = 0;
	InfiniteWorld_Seed = 0;
	InfiniteWorld_Active = false;
	InfiniteWorld_AccessCounter = 0;
	InfiniteWorld_ResidentChunks = 0;
	InfiniteWorld_NextCompactAt = 4096;
}

void InfiniteWorld_Init(int seed) {
	InfiniteWorld_Reset();
	InfiniteWorld_Seed = seed;
	InfiniteWorld_Active = true;
}

int InfiniteWorld_ChunkCount(void) {
	return InfiniteWorld_ChunksCount;
}

static cc_uint32 InfiniteWorld_HashChunk(int x, int z) {
	cc_uint32 hash = (cc_uint32)x * 0x9E3779B1u ^ (cc_uint32)z * 0x85EBCA77u;
	hash ^= hash >> 16;
	hash *= 0x7FEB352Du;
	hash ^= hash >> 15;
	return hash;
}

static cc_bool InfiniteWorld_RebuildChunkSlots(cc_uint32 capacity) {
	cc_uint32* slots;
	cc_uint32 i, index;
	if (capacity < 32 || (capacity & (capacity - 1))) return false;
	slots = (cc_uint32*)Mem_TryAllocCleared(capacity, sizeof(cc_uint32));
	if (!slots) return false;

	for (i = 0; i < (cc_uint32)InfiniteWorld_ChunksCount; i++) {
		struct InfiniteWorld_Chunk* chunk = &InfiniteWorld_Chunks[i];
		index = InfiniteWorld_HashChunk(chunk->x, chunk->z) & (capacity - 1);
		while (slots[index]) index = (index + 1) & (capacity - 1);
		slots[index] = i + 1;
	}
	Mem_Free(InfiniteWorld_ChunkSlots);
	InfiniteWorld_ChunkSlots = slots;
	InfiniteWorld_ChunkSlotsCapacity = capacity;
	return true;
}

static struct InfiniteWorld_Chunk* InfiniteWorld_FindChunk(int chunkX, int chunkZ) {
	cc_uint32 index, slot;
	if (!InfiniteWorld_ChunkSlotsCapacity) return NULL;
	index = InfiniteWorld_HashChunk(chunkX, chunkZ) & (InfiniteWorld_ChunkSlotsCapacity - 1);
	while ((slot = InfiniteWorld_ChunkSlots[index]) != 0) {
		struct InfiniteWorld_Chunk* chunk = &InfiniteWorld_Chunks[slot - 1];
		if (chunk->x == chunkX && chunk->z == chunkZ) return chunk;
		index = (index + 1) & (InfiniteWorld_ChunkSlotsCapacity - 1);
	}
	return NULL;
}

static cc_bool InfiniteWorld_InsertChunkSlot(int chunkX, int chunkZ, int chunkIndex) {
	cc_uint32 capacity = InfiniteWorld_ChunkSlotsCapacity;
	cc_uint32 index;
	if (!capacity || ((cc_uint32)InfiniteWorld_ChunksCount * 2 >= capacity)) {
		cc_uint32 next = capacity ? capacity * 2 : 32;
		if (next < capacity || !InfiniteWorld_RebuildChunkSlots(next)) return false;
		capacity = InfiniteWorld_ChunkSlotsCapacity;
	}
	index = InfiniteWorld_HashChunk(chunkX, chunkZ) & (capacity - 1);
	while (InfiniteWorld_ChunkSlots[index]) index = (index + 1) & (capacity - 1);
	InfiniteWorld_ChunkSlots[index] = (cc_uint32)chunkIndex + 1;
	return true;
}

static void InfiniteWorld_EvictGeneratedChunks(struct InfiniteWorld_Chunk* keep) {
	while (InfiniteWorld_ResidentChunks > INFINITE_WORLD_MAX_RESIDENT_CHUNKS) {
		int attempts = InfiniteWorld_ResidentChunks;
		cc_bool evicted = false;
		while (attempts-- > 0) {
			struct InfiniteWorld_Chunk* oldest = NULL;
			int i;
			for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
				struct InfiniteWorld_Chunk* candidate = &InfiniteWorld_Chunks[i];
				if (!candidate->blocks || candidate == keep ||
					(candidate->modified && candidate->compressionBlocked)) continue;
				if (!oldest || candidate->lastUsed < oldest->lastUsed) oldest = candidate;
			}
			if (!oldest) break;
			if (oldest->modified) {
				if (!InfiniteWorld_CompressModifiedChunk(oldest)) {
					oldest->lastUsed = ++InfiniteWorld_AccessCounter;
					continue;
				}
			} else {
				Mem_Free(oldest->blocks);
				oldest->blocks = NULL;
#ifdef EXTENDED_BLOCKS
				Mem_Free(oldest->blocks2);
				oldest->blocks2 = NULL;
#endif
				InfiniteWorld_ResidentChunks--;
			}
			evicted = true;
			break;
		}
		if (!evicted) break;
	}
}

/* Evicted generated chunks need no persistent metadata: their contents are
   reproducible from the seed. Periodically discard those records so travelling
   through a large world does not grow the coordinate index forever. */
static void InfiniteWorld_CompactChunks(void) {
	struct InfiniteWorld_Chunk* oldChunks, *newChunks;
	int oldCount, oldCapacity, i, live = 0, out = 0;
	cc_uint32 capacity;

	if (InfiniteWorld_ChunksCount < InfiniteWorld_NextCompactAt) return;
	InfiniteWorld_NextCompactAt = InfiniteWorld_ChunksCount + 1024;
	for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
		if (InfiniteWorld_Chunks[i].blocks || InfiniteWorld_Chunks[i].modified) live++;
	}
	if (live * 2 >= InfiniteWorld_ChunksCount) return;

	capacity = 32;
	while (capacity < (cc_uint32)live * 2) {
		if (capacity > 0x7FFFFFFFu) return;
		capacity *= 2;
	}
	newChunks = (struct InfiniteWorld_Chunk*)Mem_TryAlloc((cc_uint32)live, sizeof(struct InfiniteWorld_Chunk));
	if (!newChunks && live) return;
	for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
		struct InfiniteWorld_Chunk* chunk = &InfiniteWorld_Chunks[i];
		if (!chunk->blocks && !chunk->modified) continue;
		newChunks[out++] = *chunk;
	}

	oldChunks   = InfiniteWorld_Chunks;
	oldCount    = InfiniteWorld_ChunksCount;
	oldCapacity = InfiniteWorld_ChunksCapacity;
	InfiniteWorld_Chunks = newChunks;
	InfiniteWorld_ChunksCount = live;
	InfiniteWorld_ChunksCapacity = live;
	if (!InfiniteWorld_RebuildChunkSlots(capacity)) {
		InfiniteWorld_Chunks = oldChunks;
		InfiniteWorld_ChunksCount = oldCount;
		InfiniteWorld_ChunksCapacity = oldCapacity;
		Mem_Free(newChunks);
		return;
	}
	Mem_Free(oldChunks);
}


static struct InfiniteWorld_Chunk* InfiniteWorld_GetChunk(int chunkX, int chunkZ) {
	struct InfiniteWorld_Chunk* chunk;
	BlockRaw* blocks, *blocks2;
	if (!InfiniteWorld_Active) return NULL;
	InfiniteWorld_CompactChunks();
	chunk = InfiniteWorld_FindChunk(chunkX, chunkZ);
	if (chunk) {
		if (!chunk->blocks && !InfiniteWorld_MaterializeChunk(chunk)) return NULL;
		chunk->lastUsed = ++InfiniteWorld_AccessCounter;
		InfiniteWorld_EvictGeneratedChunks(chunk);
		return chunk;
	}

	if (InfiniteWorld_ChunksCount == InfiniteWorld_ChunksCapacity) {
		int capacity = InfiniteWorld_ChunksCapacity ? InfiniteWorld_ChunksCapacity * 2 : 16;
		struct InfiniteWorld_Chunk* grown;
		if (capacity < InfiniteWorld_ChunksCapacity) return NULL;
		grown = (struct InfiniteWorld_Chunk*)Mem_TryRealloc(
			InfiniteWorld_Chunks, (cc_uint32)capacity, sizeof(struct InfiniteWorld_Chunk));
		if (!grown) return NULL;
		InfiniteWorld_Chunks = grown;
		InfiniteWorld_ChunksCapacity = capacity;
	}

	if (!InfiniteWorld_CreateChunkData(chunkX, chunkZ, NULL, 0, &blocks, &blocks2)) return NULL;
	chunk = &InfiniteWorld_Chunks[InfiniteWorld_ChunksCount];
	chunk->x = chunkX;
	chunk->z = chunkZ;
	chunk->modified = false;
	chunk->compressionBlocked = false;
	chunk->lastUsed = ++InfiniteWorld_AccessCounter;
	chunk->blocks = blocks;
#ifdef EXTENDED_BLOCKS
	chunk->blocks2 = blocks2;
#endif
	chunk->edits = NULL;
	chunk->editCount = 0;
	if (!InfiniteWorld_InsertChunkSlot(chunkX, chunkZ, InfiniteWorld_ChunksCount)) {
		Mem_Free(blocks);
		Mem_Free(blocks2);
		return NULL;
	}
	InfiniteWorld_ChunksCount++;
	InfiniteWorld_ResidentChunks++;
	InfiniteWorld_EvictGeneratedChunks(chunk);
	return chunk;
}

cc_bool InfiniteWorld_TryGetBlock(int x, int y, int z, BlockID* block) {
	struct InfiniteWorld_Chunk* chunk;
	int index;
	if (!block || y < 0 || y >= INFINITE_WORLD_HEIGHT || !InfiniteWorld_Active) return false;
	chunk = InfiniteWorld_FindChunk(InfiniteWorld_ChunkCoord(x), InfiniteWorld_ChunkCoord(z));
	if (!chunk) return false;
	index = (y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE)
		+ (InfiniteWorld_LocalCoord(z) * INFINITE_WORLD_CHUNK_SIZE)
		+ InfiniteWorld_LocalCoord(x);
	if (!chunk->blocks) {
		cc_uint32 low = 0, high = chunk->editCount;
		while (low < high) {
			cc_uint32 mid = low + (high - low) / 2;
			if (chunk->edits[mid].index < index) low = mid + 1;
			else high = mid;
		}
		if (low >= chunk->editCount || chunk->edits[low].index != index) return false;
		*block = (BlockID)chunk->edits[low].block;
		chunk->lastUsed = ++InfiniteWorld_AccessCounter;
		return true;
	}
	/* Read-only physics probes still count as recent use for cache eviction. */
	chunk->lastUsed = ++InfiniteWorld_AccessCounter;
#ifdef EXTENDED_BLOCKS
	if (chunk->blocks2) *block = (BlockID)(chunk->blocks[index] | (chunk->blocks2[index] << 8));
	else
#endif
	*block = (BlockID)chunk->blocks[index];
	return true;
}

BlockID InfiniteWorld_GetBlock(int x, int y, int z) {
	int cx, cz, index;
	struct InfiniteWorld_Chunk* chunk;
	if (y < 0) return BLOCK_BEDROCK;
	if (y >= INFINITE_WORLD_HEIGHT) return BLOCK_AIR;
	if (!InfiniteWorld_Active) return BLOCK_AIR;

	cx = InfiniteWorld_ChunkCoord(x);
	cz = InfiniteWorld_ChunkCoord(z);
	chunk = InfiniteWorld_GetChunk(cx, cz);
	if (!chunk) return BLOCK_AIR;
	index = (y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE)
		+ (InfiniteWorld_LocalCoord(z) * INFINITE_WORLD_CHUNK_SIZE)
		+ InfiniteWorld_LocalCoord(x);
#ifdef EXTENDED_BLOCKS
	if (chunk->blocks2) return (BlockID)(chunk->blocks[index] | (chunk->blocks2[index] << 8));
#endif
	return (BlockID)chunk->blocks[index];
}

cc_bool InfiniteWorld_SetBlock(int x, int y, int z, BlockID block) {
	int cx, cz, index;
	struct InfiniteWorld_Chunk* chunk;
	if (y < 0 || y >= INFINITE_WORLD_HEIGHT || !InfiniteWorld_Active) return false;

	cx = InfiniteWorld_ChunkCoord(x);
	cz = InfiniteWorld_ChunkCoord(z);
	chunk = InfiniteWorld_GetChunk(cx, cz);
	if (!chunk) return false;
	index = (y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE)
		+ (InfiniteWorld_LocalCoord(z) * INFINITE_WORLD_CHUNK_SIZE)
		+ InfiniteWorld_LocalCoord(x);
#ifndef EXTENDED_BLOCKS
	/* No-op writes should not turn reproducible terrain into a pinned edit. */
	if (chunk->blocks[index] == (BlockRaw)block) return true;
#endif
#ifdef EXTENDED_BLOCKS
	/* Avoid pinning a generated chunk when a caller writes its existing block. */
	if ((BlockID)(chunk->blocks[index] | (chunk->blocks2 ? (chunk->blocks2[index] << 8) : 0)) == block) return true;
	if (block > 255 && !chunk->blocks2) {
		chunk->blocks2 = (BlockRaw*)Mem_TryAllocCleared(INFINITE_WORLD_CHUNK_VOLUME, sizeof(BlockRaw));
		if (!chunk->blocks2) return false;
	}
	if (chunk->blocks2) chunk->blocks2[index] = (BlockRaw)(block >> 8);
#endif
	chunk->blocks[index] = (BlockRaw)block;
	chunk->modified = true;
	chunk->compressionBlocked = false;
	return true;
}


static void InfiniteWorld_WriteU32(cc_uint8* dst, cc_uint32 value) {
	dst[0] = (cc_uint8)value; dst[1] = (cc_uint8)(value >> 8);
	dst[2] = (cc_uint8)(value >> 16); dst[3] = (cc_uint8)(value >> 24);
}
static cc_uint32 InfiniteWorld_ReadU32(const cc_uint8* src) {
	return (cc_uint32)src[0] | ((cc_uint32)src[1] << 8) | ((cc_uint32)src[2] << 16) | ((cc_uint32)src[3] << 24);
}
static cc_result InfiniteWorld_WriteChunkData(struct Stream* stream, struct InfiniteWorld_Chunk* chunk) {
	BlockRaw* tempBlocks = NULL, *tempBlocks2 = NULL;
	BlockRaw* blocks = chunk->blocks;
	cc_result res;
#ifdef EXTENDED_BLOCKS
	BlockRaw zeroUpper[INFINITE_WORLD_CHUNK_VOLUME] = { 0 };
	BlockRaw* blocks2 = chunk->blocks2;
#endif
	if (!blocks) {
		if (!InfiniteWorld_CreateChunkData(chunk->x, chunk->z, chunk->edits, chunk->editCount, &tempBlocks, &tempBlocks2)) return ERR_OUT_OF_MEMORY;
		blocks = tempBlocks;
#ifdef EXTENDED_BLOCKS
		blocks2 = tempBlocks2;
#endif
	}
	res = Stream_Write(stream, blocks, INFINITE_WORLD_CHUNK_VOLUME);
#ifdef EXTENDED_BLOCKS
	if (!res) res = Stream_Write(stream, blocks2 ? blocks2 : zeroUpper, INFINITE_WORLD_CHUNK_VOLUME);
#endif
	Mem_Free(tempBlocks);
	Mem_Free(tempBlocks2);
	return res;
}

cc_result InfiniteWorld_Save(struct Stream* stream) {
	cc_uint8 header[16], coords[8];
	cc_uint32 count = 0;
	int i;
	cc_result res;
	if (!InfiniteWorld_Active) return ERR_NOT_SUPPORTED;
	for (i = 0; i < InfiniteWorld_ChunksCount; i++) if (InfiniteWorld_Chunks[i].modified) count++;
	header[0] = 'I'; header[1] = 'C'; header[2] = 'W'; header[3] = '1';
	InfiniteWorld_WriteU32(&header[4], (cc_uint32)InfiniteWorld_Seed);
	InfiniteWorld_WriteU32(&header[8], INFINITE_WORLD_HEIGHT);
	InfiniteWorld_WriteU32(&header[12], count);
	if ((res = Stream_Write(stream, header, sizeof(header)))) return res;
	for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
		struct InfiniteWorld_Chunk* chunk = &InfiniteWorld_Chunks[i];
		if (!chunk->modified) continue;
		InfiniteWorld_WriteU32(&coords[0], (cc_uint32)chunk->x);
		InfiniteWorld_WriteU32(&coords[4], (cc_uint32)chunk->z);
		if ((res = Stream_Write(stream, coords, sizeof(coords)))) return res;
		if ((res = InfiniteWorld_WriteChunkData(stream, chunk))) return res;
	}
	return 0;
}
struct InfiniteWorld_ChunkCoordPair { int x, z; };

static cc_bool InfiniteWorld_ChunkCoordLess(const struct InfiniteWorld_ChunkCoordPair* a,
		const struct InfiniteWorld_ChunkCoordPair* b) {
	return a->x < b->x || (a->x == b->x && a->z < b->z);
}

/* In-place heapsort keeps duplicate detection bounded to one coordinate pair
   per saved chunk, even for the maximum supported save size. */
static void InfiniteWorld_SortChunkCoords(struct InfiniteWorld_ChunkCoordPair* coords, cc_uint32 count) {
	cc_uint32 root, end;
	for (root = count / 2; root > 0; ) {
		cc_uint32 parent = --root;
		for (;;) {
			cc_uint32 child = parent * 2 + 1;
			struct InfiniteWorld_ChunkCoordPair tmp;
			if (child >= count) break;
			if (child + 1 < count && InfiniteWorld_ChunkCoordLess(&coords[child], &coords[child + 1])) child++;
			if (!InfiniteWorld_ChunkCoordLess(&coords[parent], &coords[child])) break;
			tmp = coords[parent]; coords[parent] = coords[child]; coords[child] = tmp;
			parent = child;
		}
	}
	for (end = count; end > 1; ) {
		cc_uint32 limit = --end, parent = 0;
		struct InfiniteWorld_ChunkCoordPair tmp = coords[0];
		coords[0] = coords[limit]; coords[limit] = tmp;
		for (;;) {
			cc_uint32 child = parent * 2 + 1;
			if (child >= limit) break;
			if (child + 1 < limit && InfiniteWorld_ChunkCoordLess(&coords[child], &coords[child + 1])) child++;
			if (!InfiniteWorld_ChunkCoordLess(&coords[parent], &coords[child])) break;
			tmp = coords[parent]; coords[parent] = coords[child]; coords[child] = tmp;
			parent = child;
		}
	}
}

/* Validate all coordinates before replacing the active world. Streams that
   cannot seek retain the normal loader path; seekable streams get the full
   duplicate/bounds check without changing the ICW1 record layout. */
static cc_result InfiniteWorld_PreflightRecords(struct Stream* stream, cc_uint32 count,
		cc_uint32 payloadSize) {
	struct InfiniteWorld_ChunkCoordPair* coords;
	cc_uint8 bytes[8];
	cc_uint32 position, n;
	cc_result res = 0, seekRes;
	if (stream->Position(stream, &position)) return 0;
	if (stream->Seek(stream, position)) return 0;
	if (!count) return 0;
	coords = (struct InfiniteWorld_ChunkCoordPair*)Mem_TryAlloc(count, sizeof(struct InfiniteWorld_ChunkCoordPair));
	if (!coords) return ERR_OUT_OF_MEMORY;

	for (n = 0; n < count; n++) {
		int x, z;
		if ((res = Stream_Read(stream, bytes, sizeof(bytes)))) break;
		x = (int)(cc_int32)InfiniteWorld_ReadU32(&bytes[0]);
		z = (int)(cc_int32)InfiniteWorld_ReadU32(&bytes[4]);
		if (x < -134217728 || x > 134217727 || z < -134217728 || z > 134217727) {
			res = ERR_INVALID_ARGUMENT;
			break;
		}
		coords[n].x = x;
		coords[n].z = z;
		if ((res = stream->Skip(stream, payloadSize))) break;
	}
	if (!res) {
		InfiniteWorld_SortChunkCoords(coords, count);
		for (n = 1; n < count; n++) {
			if (coords[n - 1].x == coords[n].x && coords[n - 1].z == coords[n].z) {
				res = ERR_INVALID_ARGUMENT;
				break;
			}
		}
	}
	seekRes = stream->Seek(stream, position);
	Mem_Free(coords);
	return seekRes ? seekRes : res;
}

cc_result InfiniteWorld_Load(struct Stream* stream) {
	cc_uint8 header[16], coords[8];
#ifdef EXTENDED_BLOCKS
	cc_uint8 upper[INFINITE_WORLD_CHUNK_VOLUME];
	int i;
#endif
	cc_uint32 count, n, position, length;
	cc_uint64 recordSize, required;
	int seed, chunkX, chunkZ;
	struct InfiniteWorld_Chunk* chunk;
	cc_result res;
	if ((res = Stream_Read(stream, header, sizeof(header)))) return res;
	if (header[0] != 'I' || header[1] != 'C' || header[2] != 'W' || header[3] != '1') return ERR_INVALID_ARGUMENT;
	seed = (int)(cc_int32)InfiniteWorld_ReadU32(&header[4]);
	if (InfiniteWorld_ReadU32(&header[8]) != INFINITE_WORLD_HEIGHT) return ERR_INVALID_ARGUMENT;
	count = InfiniteWorld_ReadU32(&header[12]);
	if (count > 1000000u) return ERR_INVALID_ARGUMENT;
	recordSize = 8 + INFINITE_WORLD_CHUNK_VOLUME;
#ifdef EXTENDED_BLOCKS
	recordSize += INFINITE_WORLD_CHUNK_VOLUME;
#endif
	/* Reject truncated files before replacing the currently loaded world. */
	if (!stream->Position(stream, &position) && !stream->Length(stream, &length)) {
		required = (cc_uint64)count * recordSize;
		if (position > length || required > (cc_uint64)(length - position)) return ERR_END_OF_STREAM;
	}
	/* Bounds and duplicate checks must also happen before the active world is
	   replaced. The preflight rewinds on completion, ready for the real load. */
	if ((res = InfiniteWorld_PreflightRecords(stream, count, (cc_uint32)(recordSize - 8)))) return res;
	World_SetInfiniteMap(seed, INFINITE_WORLD_HEIGHT);
	for (n = 0; n < count; n++) {
		if ((res = Stream_Read(stream, coords, sizeof(coords)))) return res;
		chunkX = (int)(cc_int32)InfiniteWorld_ReadU32(&coords[0]);
		chunkZ = (int)(cc_int32)InfiniteWorld_ReadU32(&coords[4]);
		/* Keep the checks for non-seekable streams, which cannot be preflighted. */
		if (chunkX < -134217728 || chunkX > 134217727 ||
			chunkZ < -134217728 || chunkZ > 134217727) return ERR_INVALID_ARGUMENT;
		if (InfiniteWorld_FindChunk(chunkX, chunkZ)) return ERR_INVALID_ARGUMENT;
		chunk = InfiniteWorld_GetChunk(chunkX, chunkZ);
		if (!chunk) return ERR_OUT_OF_MEMORY;
		if ((res = Stream_Read(stream, chunk->blocks, INFINITE_WORLD_CHUNK_VOLUME))) return res;
#ifdef EXTENDED_BLOCKS
		if ((res = Stream_Read(stream, upper, INFINITE_WORLD_CHUNK_VOLUME))) return res;
		for (i = 0; i < INFINITE_WORLD_CHUNK_VOLUME; i++) {
			if (upper[i]) {
				if (!chunk->blocks2) {
					chunk->blocks2 = (BlockRaw*)Mem_TryAllocCleared(INFINITE_WORLD_CHUNK_VOLUME, sizeof(BlockRaw));
					if (!chunk->blocks2) return ERR_OUT_OF_MEMORY;
				}
				break;
			}
		}
		if (chunk->blocks2) Mem_Copy(chunk->blocks2, upper, INFINITE_WORLD_CHUNK_VOLUME);
#endif
		chunk->modified = true;
	}
	return 0;
}
