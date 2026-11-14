#ifndef CC_INFINITEWORLD_H
#define CC_INFINITEWORLD_H
#include "Core.h"
CC_BEGIN_HEADER

/* First-stage deterministic terrain source for streamed infinite worlds.
   Chunks are 16x64x16 and use signed chunk coordinates.
   Copyright 2026 BenFreedomWalker | Licensed under BSD-3
*/
#define INFINITE_WORLD_CHUNK_SIZE 16
#define INFINITE_WORLD_HEIGHT 64
#define INFINITE_WORLD_CHUNK_VOLUME (INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_HEIGHT)

/* Generates one chunk into a y-major, then z, then x block array.
   Returns false when the requested chunk would overflow block coordinates.
   Generation depends only on seed and absolute chunk coordinates.
*/
cc_bool InfiniteWorld_GenerateChunk(int seed, int chunkX, int chunkZ, BlockRaw* blocks);

CC_END_HEADER
#endif
