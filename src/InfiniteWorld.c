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
	cc_int64 anchorX, anchorZ, leafX, leafZ;
	int x0 = (int)baseX, z0 = (int)baseZ;
	for (anchorZ = baseZ - 2; anchorZ <= baseZ + INFINITE_WORLD_CHUNK_SIZE + 1; anchorZ++) {
		for (anchorX = baseX - 2; anchorX <= baseX + INFINITE_WORLD_CHUNK_SIZE + 1; anchorX++) {
			cc_uint32 hash;
			int x, z, surface, height, top, y, radius, index;
			if (anchorX < Int32_MinValue || anchorX > Int32_MaxValue ||
				anchorZ < Int32_MinValue || anchorZ > Int32_MaxValue) continue;
			x = (int)anchorX; z = (int)anchorZ;
			hash = InfiniteWorld_Hash(seed ^ 0x2C9277B5u, x, z);
			if ((hash & 255u) >= 2u) continue;
			surface = InfiniteWorld_SurfaceHeight(seed, x, z);
			if (surface <= INFINITE_WORLD_WATER_LEVEL) continue;
			height = 5 + (int)((hash >> 8) % 3u);
			top = surface + height;
			if (top + 1 >= INFINITE_WORLD_HEIGHT) continue;
			/* Trunks may replace leaves from an overlapping tree. */
			if (x >= x0 && x < x0 + INFINITE_WORLD_CHUNK_SIZE &&
				z >= z0 && z < z0 + INFINITE_WORLD_CHUNK_SIZE) {
				for (y = surface + 1; y < top; y++) {
					index = y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE
						+ (z - z0) * INFINITE_WORLD_CHUNK_SIZE + (x - x0);
					if (blocks[index] == BLOCK_AIR || blocks[index] == BLOCK_LEAVES) blocks[index] = BLOCK_LOG;
				}
			}
			for (y = top - 2; y <= top + 1; y++) {
				radius = y >= top ? 1 : 2;
				for (leafZ = anchorZ - radius; leafZ <= anchorZ + radius; leafZ++) {
					for (leafX = anchorX - radius; leafX <= anchorX + radius; leafX++) {
						if (leafX < x0 || leafX >= x0 + INFINITE_WORLD_CHUNK_SIZE ||
							leafZ < z0 || leafZ >= z0 + INFINITE_WORLD_CHUNK_SIZE || y < 0 || y >= INFINITE_WORLD_HEIGHT) continue;
						x = (int)leafX; z = (int)leafZ;
						index = y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE
							+ (z - z0) * INFINITE_WORLD_CHUNK_SIZE + (x - x0);
						if (blocks[index] == BLOCK_AIR) blocks[index] = BLOCK_LEAVES;
					}
				}
			}
		}
	}
}
cc_bool InfiniteWorld_GenerateChunk(int seed, int chunkX, int chunkZ, BlockRaw* blocks) {
	int x, y, z, worldX, worldZ, surface, index;
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

			for (y = 0; y < INFINITE_WORLD_HEIGHT; y++) {
				index = (y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE) + (z * INFINITE_WORLD_CHUNK_SIZE) + x;
				if (y == 0) {
					blocks[index] = BLOCK_BEDROCK;
				} else if (y < surface - 3) {
					blocks[index] = BLOCK_STONE;
				} else if (y < surface) {
					blocks[index] = BLOCK_DIRT;
				} else if (y == surface) {
					blocks[index] = BLOCK_GRASS;
				} else if (y <= INFINITE_WORLD_WATER_LEVEL) {
					blocks[index] = BLOCK_STILL_WATER;
				} else {
					blocks[index] = BLOCK_AIR;
				}
			}
		}
	}
	InfiniteWorld_GenerateTrees(seed, chunkX, chunkZ, blocks);
	return true;
}

/* Modified chunks stay resident until saved. Generated-only chunk block arrays
   use a bounded cache; their terrain can always be regenerated from the seed. */
struct InfiniteWorld_Chunk {
	int x, z;
	cc_bool modified;
	cc_uint64 lastUsed;
	BlockRaw* blocks;
#ifdef EXTENDED_BLOCKS
	BlockRaw* blocks2;
#endif
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

void InfiniteWorld_Reset(void) {
	int i;
	for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
		Mem_Free(InfiniteWorld_Chunks[i].blocks);
#ifdef EXTENDED_BLOCKS
		Mem_Free(InfiniteWorld_Chunks[i].blocks2);
#endif
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
		struct InfiniteWorld_Chunk* oldest = NULL;
		int i;
		for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
			struct InfiniteWorld_Chunk* candidate = &InfiniteWorld_Chunks[i];
			if (!candidate->blocks || candidate->modified || candidate == keep) continue;
			if (!oldest || candidate->lastUsed < oldest->lastUsed) oldest = candidate;
		}
		if (!oldest) break;
		Mem_Free(oldest->blocks);
		oldest->blocks = NULL;
		InfiniteWorld_ResidentChunks--;
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
	BlockRaw* blocks;
	if (!InfiniteWorld_Active) return NULL;
	InfiniteWorld_CompactChunks();
	chunk = InfiniteWorld_FindChunk(chunkX, chunkZ);
	if (chunk) {
		if (!chunk->blocks) {
			blocks = (BlockRaw*)Mem_TryAlloc(INFINITE_WORLD_CHUNK_VOLUME, sizeof(BlockRaw));
			if (!blocks) return NULL;
			if (!InfiniteWorld_GenerateChunk(InfiniteWorld_Seed, chunkX, chunkZ, blocks)) {
				Mem_Free(blocks);
				return NULL;
			}
			chunk->blocks = blocks;
			InfiniteWorld_ResidentChunks++;
		}
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

	blocks = (BlockRaw*)Mem_TryAlloc(INFINITE_WORLD_CHUNK_VOLUME, sizeof(BlockRaw));
	if (!blocks) return NULL;
	if (!InfiniteWorld_GenerateChunk(InfiniteWorld_Seed, chunkX, chunkZ, blocks)) {
		Mem_Free(blocks);
		return NULL;
	}

	chunk = &InfiniteWorld_Chunks[InfiniteWorld_ChunksCount];
	chunk->x = chunkX;
	chunk->z = chunkZ;
	chunk->modified = false;
	chunk->lastUsed = ++InfiniteWorld_AccessCounter;
	chunk->blocks = blocks;
#ifdef EXTENDED_BLOCKS
	chunk->blocks2 = NULL;
#endif
	if (!InfiniteWorld_InsertChunkSlot(chunkX, chunkZ, InfiniteWorld_ChunksCount)) {
		Mem_Free(blocks);
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
	/* A chunk without resident blocks is generated-only and cannot contain edits. */
	if (!chunk || !chunk->blocks) return false;
	index = (y * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE)
		+ (InfiniteWorld_LocalCoord(z) * INFINITE_WORLD_CHUNK_SIZE)
		+ InfiniteWorld_LocalCoord(x);
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
#ifdef EXTENDED_BLOCKS
	if (block > 255 && !chunk->blocks2) {
		chunk->blocks2 = (BlockRaw*)Mem_TryAllocCleared(INFINITE_WORLD_CHUNK_VOLUME, sizeof(BlockRaw));
		if (!chunk->blocks2) return false;
	}
	if (chunk->blocks2) chunk->blocks2[index] = (BlockRaw)(block >> 8);
#endif
	chunk->blocks[index] = (BlockRaw)block;
	chunk->modified = true;
	return true;
}


static void InfiniteWorld_WriteU32(cc_uint8* dst, cc_uint32 value) {
	dst[0] = (cc_uint8)value; dst[1] = (cc_uint8)(value >> 8);
	dst[2] = (cc_uint8)(value >> 16); dst[3] = (cc_uint8)(value >> 24);
}
static cc_uint32 InfiniteWorld_ReadU32(const cc_uint8* src) {
	return (cc_uint32)src[0] | ((cc_uint32)src[1] << 8) | ((cc_uint32)src[2] << 16) | ((cc_uint32)src[3] << 24);
}
cc_result InfiniteWorld_Save(struct Stream* stream) {
	cc_uint8 header[16], coords[8];
#ifdef EXTENDED_BLOCKS
	cc_uint8 upper[INFINITE_WORLD_CHUNK_VOLUME] = { 0 };
#endif
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
		if ((res = Stream_Write(stream, chunk->blocks, INFINITE_WORLD_CHUNK_VOLUME))) return res;
#ifdef EXTENDED_BLOCKS
		if ((res = Stream_Write(stream, chunk->blocks2 ? chunk->blocks2 : upper, INFINITE_WORLD_CHUNK_VOLUME))) return res;
#endif
	}
	return 0;
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
	World_SetInfiniteMap(seed, INFINITE_WORLD_HEIGHT);
	for (n = 0; n < count; n++) {
		if ((res = Stream_Read(stream, coords, sizeof(coords)))) return res;
		chunkX = (int)(cc_int32)InfiniteWorld_ReadU32(&coords[0]);
		chunkZ = (int)(cc_int32)InfiniteWorld_ReadU32(&coords[4]);
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
