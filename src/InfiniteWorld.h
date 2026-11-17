#ifndef CC_INFINITEWORLD_H
#define CC_INFINITEWORLD_H
#include "Core.h"
struct Stream;
CC_BEGIN_HEADER

/* First-stage deterministic terrain source for streamed infinite worlds.
   Chunks are 16x64x16 and use signed chunk coordinates.
   Copyright 2026 BenFreedomWalker | Licensed under BSD-3
*/
#define INFINITE_WORLD_CHUNK_SIZE 16
#define INFINITE_WORLD_HEIGHT 64
#define INFINITE_WORLD_WATER_LEVEL 30
#define INFINITE_WORLD_CHUNK_VOLUME (INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_CHUNK_SIZE * INFINITE_WORLD_HEIGHT)

/* Generates one chunk into a y-major, then z, then x block array.
   Returns false when the requested chunk would overflow block coordinates.
   Generation depends only on seed and absolute chunk coordinates.
*/

/* Converts signed block coordinates to chunk and in-chunk coordinates.
   Local coordinates are always in the range [0, 15], including for negatives.
*/
int InfiniteWorld_ChunkCoord(int blockCoord);
int InfiniteWorld_LocalCoord(int blockCoord);

cc_bool InfiniteWorld_GenerateChunk(int seed, int chunkX, int chunkZ, BlockRaw* blocks);

/* Starts a new in-memory infinite world. Existing finite World state is untouched. */
void InfiniteWorld_Init(int seed);
/* Frees all cached chunks and disables the infinite-world block store. */
void InfiniteWorld_Reset(void);
/* Reads a block from the generated chunk store. Y below zero is bedrock;
   Y at or above the world height is air. */
BlockID InfiniteWorld_GetBlock(int x, int y, int z);
/* Generates/materializes the target chunk and reports allocation failure separately from air. */
cc_bool InfiniteWorld_TryGetBlockGenerate(int x, int y, int z, BlockID* block);
/* Copies an 18x18x18 mesh volume, resolving each intersecting data chunk once. */
cc_bool InfiniteWorld_CopyMeshData(int x1, int y1, int z1, BlockID* blocks);
/* Reads dense resident data or explicit sparse edits; never generates base terrain. */
cc_bool InfiniteWorld_TryGetBlock(int x, int y, int z, BlockID* block);
/* Changes a block in the chunk store. Returns false if coordinates are invalid
   or memory allocation fails. */
cc_bool InfiniteWorld_SetBlock(int x, int y, int z, BlockID block);
/* Number of chunk coordinates tracked by the world. */
int InfiniteWorld_ChunkCount(void);
cc_result InfiniteWorld_Save(struct Stream* stream);
cc_result InfiniteWorld_Load(struct Stream* stream);


CC_END_HEADER
#endif
