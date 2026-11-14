#include "InfiniteWorld.h"
#include "BlockID.h"

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

cc_bool InfiniteWorld_GenerateChunk(int seed, int chunkX, int chunkZ, BlockRaw* blocks) {
	int x, y, z, worldX, worldZ, surface, index;
	cc_int64 baseX = (cc_int64)chunkX * INFINITE_WORLD_CHUNK_SIZE;
	cc_int64 baseZ = (cc_int64)chunkZ * INFINITE_WORLD_CHUNK_SIZE;

	if (!blocks) return false;
	if (baseX < Int32_MinValue || baseX + 15 > Int32_MaxValue) return false;
	if (baseZ < Int32_MinValue || baseZ + 15 > Int32_MaxValue) return false;

	for (z = 0; z < INFINITE_WORLD_CHUNK_SIZE; z++) {
		worldZ = (int)(baseZ + z);
		for (x = 0; x < INFINITE_WORLD_CHUNK_SIZE; x++) {
			worldX = (int)(baseX + x);
			surface = InfiniteWorld_SurfaceHeight(seed, worldX, worldZ);

			for (y = 0; y < INFINITE_WORLD_HEIGHT; y++) {
				if (y == 0) {
					blocks[(y * 16 * 16) + (z * 16) + x] = BLOCK_BEDROCK;
				} else if (y < surface - 3) {
					blocks[(y * 16 * 16) + (z * 16) + x] = BLOCK_STONE;
				} else if (y < surface) {
					blocks[(y * 16 * 16) + (z * 16) + x] = BLOCK_DIRT;
				} else if (y == surface) {
					blocks[(y * 16 * 16) + (z * 16) + x] = BLOCK_GRASS;
				} else if (y <= 30) {
					blocks[(y * 16 * 16) + (z * 16) + x] = BLOCK_STILL_WATER;
				} else {
					blocks[(y * 16 * 16) + (z * 16) + x] = BLOCK_AIR;
				}
			}
		}
	}
	return true;
}
