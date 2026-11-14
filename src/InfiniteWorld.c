#include "InfiniteWorld.h"
#include "BlockID.h"
#include "Platform.h"

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
	int tx = x - gx * scale;
	int tz = z - gz * scale;
	int fx = (tx << 16) / scale;
	int fz = (tz << 16) / scale;
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
				} else if (y <= 30) {
					blocks[index] = BLOCK_STILL_WATER;
				} else {
					blocks[index] = BLOCK_AIR;
				}
			}
		}
	}
	return true;
}

/* The first storage implementation retains generated chunks for the lifetime
   of a world. This deliberately avoids discarding player edits before the
   persistent chunk file format is added. */
struct InfiniteWorld_Chunk {
	int x, z;
	BlockRaw* blocks;
#ifdef EXTENDED_BLOCKS
	BlockRaw* blocks2;
#endif
};

static struct InfiniteWorld_Chunk* InfiniteWorld_Chunks;
static int InfiniteWorld_ChunksCount, InfiniteWorld_ChunksCapacity;
static int InfiniteWorld_Seed;
static cc_bool InfiniteWorld_Active;

void InfiniteWorld_Reset(void) {
	int i;
	for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
		Mem_Free(InfiniteWorld_Chunks[i].blocks);
#ifdef EXTENDED_BLOCKS
		Mem_Free(InfiniteWorld_Chunks[i].blocks2);
#endif
	}
	Mem_Free(InfiniteWorld_Chunks);
	InfiniteWorld_Chunks = NULL;
	InfiniteWorld_ChunksCount = 0;
	InfiniteWorld_ChunksCapacity = 0;
	InfiniteWorld_Seed = 0;
	InfiniteWorld_Active = false;
}

void InfiniteWorld_Init(int seed) {
	InfiniteWorld_Reset();
	InfiniteWorld_Seed = seed;
	InfiniteWorld_Active = true;
}

int InfiniteWorld_ChunkCount(void) {
	return InfiniteWorld_ChunksCount;
}

static struct InfiniteWorld_Chunk* InfiniteWorld_FindChunk(int chunkX, int chunkZ) {
	int i;
	for (i = 0; i < InfiniteWorld_ChunksCount; i++) {
		struct InfiniteWorld_Chunk* chunk = &InfiniteWorld_Chunks[i];
		if (chunk->x == chunkX && chunk->z == chunkZ) return chunk;
	}
	return NULL;
}

static struct InfiniteWorld_Chunk* InfiniteWorld_GetChunk(int chunkX, int chunkZ) {
	struct InfiniteWorld_Chunk* chunk = InfiniteWorld_FindChunk(chunkX, chunkZ);
	BlockRaw* blocks;
	if (chunk) return chunk;
	if (!InfiniteWorld_Active) return NULL;

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

	chunk = &InfiniteWorld_Chunks[InfiniteWorld_ChunksCount++];
	chunk->x = chunkX;
	chunk->z = chunkZ;
	chunk->blocks = blocks;
#ifdef EXTENDED_BLOCKS
	chunk->blocks2 = NULL;
#endif
	return chunk;
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
	return true;
}
