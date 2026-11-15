#include "BlockPhysics.h"
#include "World.h"
#include "InfiniteWorld.h"
#include "Constants.h"
#include "Funcs.h"
#include "Event.h"
#include "ExtMath.h"
#include "Block.h"
#include "Lighting.h"
#include "Options.h"
#include "Generator.h"
#include "Platform.h"
#include "Game.h"
#include "Logger.h"
#include "Vectors.h"
#include "Chat.h"
#include "Audio.h"
#include "Entity.h"

/* Data for a resizable queue, used for liquid physic tick entries. */
struct TickQueue {
	cc_uint32* entries; /* Buffer holding the items in the tick queue */
	int capacity; /* Max number of elements in the buffer */
	int mask;     /* capacity - 1, as capacity is always a power of two */
	int count;    /* Number of used elements */
	int head;     /* Head index into the buffer */
	int tail;     /* Tail index into the buffer */
};

static void TickQueue_Init(struct TickQueue* queue) {
	queue->entries  = NULL;
	queue->capacity = 0;
	queue->mask  = 0;
	queue->count = 0;
	queue->head  = 0;
	queue->tail  = 0;
}

static void TickQueue_Clear(struct TickQueue* queue) {
	if (!queue->entries) return;
	Mem_Free(queue->entries);
	TickQueue_Init(queue);
}

static void TickQueue_Resize(struct TickQueue* queue) {
	cc_uint32* entries;
	int i, idx, capacity;

	if (queue->capacity >= (Int32_MaxValue / 4)) {
		Chat_AddRaw("&cToo many physics entries, clearing");
		TickQueue_Clear(queue);
	}

	capacity = queue->capacity * 2;
	if (capacity < 32) capacity = 32;
	entries = (cc_uint32*)Mem_Alloc(capacity, 4, "physics tick queue");

	/* Elements must be readjusted to avoid index wrapping issues */
	/* https://stackoverflow.com/questions/55343683/resizing-of-the-circular-queue-using-dynamic-array */
	for (i = 0; i < queue->count; i++) {
		idx = (queue->head + i) & queue->mask;
		entries[i] = queue->entries[idx];
	}
	Mem_Free(queue->entries);

	queue->entries  = entries;
	queue->capacity = capacity;
	queue->mask     = capacity - 1; /* capacity is power of two */
	queue->head = 0;
	queue->tail = queue->count;
}

/* Appends an entry to the end of the queue, resizing if necessary. */
static void TickQueue_Enqueue(struct TickQueue* queue, cc_uint32 item) {
	if (queue->count == queue->capacity)
		TickQueue_Resize(queue);

	queue->entries[queue->tail] = item;
	queue->tail = (queue->tail + 1) & queue->mask;
	queue->count++;
}

/* Retrieves the entry from the front of the queue. */
static cc_uint32 TickQueue_Dequeue(struct TickQueue* queue) {
	cc_uint32 result = queue->entries[queue->head];
	queue->head = (queue->head + 1) & queue->mask;
	queue->count--;
	return result;
}


struct Physics_ Physics;
static RNGState physics_rnd;
static int physics_tickCount;
static int physics_maxWaterX, physics_maxWaterY, physics_maxWaterZ;
static struct TickQueue lavaQ, waterQ;

#define INFINITE_PHYSICS_QUEUE_SIZE 4096
#define INFINITE_PHYSICS_TICK_BUDGET 32
struct InfinitePhysicsEntry { int x, y, z; cc_uint8 delay, flow; };
static struct InfinitePhysicsEntry infinitePhysicsQueue[INFINITE_PHYSICS_QUEUE_SIZE];
static int infinitePhysicsHead, infinitePhysicsCount;

#define PHYSICS_DELAY_MASK 0xF8000000UL
#define PHYSICS_POS_MASK   0x07FFFFFFUL
#define PHYSICS_DELAY_SHIFT 27
#define PHYSICS_ONE_DELAY   (1U << PHYSICS_DELAY_SHIFT)
#define PHYSICS_LAVA_DELAY (30U << PHYSICS_DELAY_SHIFT)
#define PHYSICS_WATER_DELAY (5U << PHYSICS_DELAY_SHIFT)

static void Physics_OnNewMapLoaded(void* obj) {
	TickQueue_Clear(&lavaQ);
	TickQueue_Clear(&waterQ);
	infinitePhysicsHead = infinitePhysicsCount = 0;

	physics_maxWaterX = World.MaxX - 2;
	physics_maxWaterY = World.MaxY - 2;
	physics_maxWaterZ = World.MaxZ - 2;

	Tree_Blocks = World.Blocks;
	Random_SeedFromCurrentTime(&physics_rnd);
	Tree_Rnd = &physics_rnd;
}

void Physics_SetEnabled(cc_bool enabled) {
	Physics.Enabled = enabled;
	Physics_OnNewMapLoaded(NULL);
}

static void Physics_Activate(int index) {
	BlockID block = World.Blocks[index];
	PhysicsHandler activate = Physics.OnActivate[block];
	if (activate) activate(index, block);
}

static void Physics_ActivateNeighbours(int x, int y, int z, int index) {
	if (x > 0)          Physics_Activate(index - 1);
	if (x < World.MaxX) Physics_Activate(index + 1);
	if (z > 0)          Physics_Activate(index - World.Width);
	if (z < World.MaxZ) Physics_Activate(index + World.Width);
	if (y > 0)          Physics_Activate(index - World.OneY);
	if (y < World.MaxY) Physics_Activate(index + World.OneY);
}

static cc_bool Physics_IsEdgeWater(int x, int y, int z) {
	return
		(Env.EdgeBlock == BLOCK_WATER || Env.EdgeBlock == BLOCK_STILL_WATER)
		&& (y >= Env_SidesHeight && y < Env.EdgeHeight)
		&& (x == 0 || z == 0 || x == World.MaxX || z == World.MaxZ);
}


/* Infinite worlds cannot use packed finite-map indices for physics. Keep
   falling-block updates coordinate based so negative X/Z positions work. */
static cc_bool InfinitePhysics_IsFalling(BlockID block) {
	return block == BLOCK_SAND || block == BLOCK_GRAVEL;
}

static cc_bool InfinitePhysics_IsReplaceable(BlockID block) {
	return block == BLOCK_AIR || (block >= BLOCK_WATER && block <= BLOCK_STILL_LAVA);
}

static void InfinitePhysics_Fall(int x, int y, int z) {
	int currentY = y;

	/* Process stacked falling blocks iteratively to avoid recursive calls. */
	while (currentY < World.Height) {
		BlockID block = World_GetBlock(x, currentY, z);
		int destY = currentY;

		if (!InfinitePhysics_IsFalling(block)) return;

		/* Search down through air and liquids, stopping at the first solid block. */
		while (destY > 0 && InfinitePhysics_IsReplaceable(World_GetBlock(x, destY - 1, z))) {
			destY--;
		}
		if (destY == currentY) return;

		Game_UpdateBlock(x, currentY, z, BLOCK_AIR);
		Game_UpdateBlock(x, destY, z, block);

		/* The move may expose another falling block directly above it. */
		currentY++;
	}
}

static void InfinitePhysics_Queue(int x, int y, int z, BlockID block, int flow) {
	int i, tail;
	/* A liquid can be reached from several neighbours in the same tick.
	   Keep only one pending update per coordinate to avoid queue saturation. */
	for (i = 0; i < infinitePhysicsCount; i++) {
		int index = (infinitePhysicsHead + i) % INFINITE_PHYSICS_QUEUE_SIZE;
		if (infinitePhysicsQueue[index].x == x &&
			infinitePhysicsQueue[index].y == y &&
			infinitePhysicsQueue[index].z == z) {
			if (flow < infinitePhysicsQueue[index].flow) infinitePhysicsQueue[index].flow = (cc_uint8)flow;
			return;
		}
	}
	if (infinitePhysicsCount == INFINITE_PHYSICS_QUEUE_SIZE) return;
	tail = (infinitePhysicsHead + infinitePhysicsCount) % INFINITE_PHYSICS_QUEUE_SIZE;
	infinitePhysicsQueue[tail].x = x; infinitePhysicsQueue[tail].y = y; infinitePhysicsQueue[tail].z = z;
	infinitePhysicsQueue[tail].delay = (block == BLOCK_LAVA || block == BLOCK_STILL_LAVA) ? 30 : 5;
	infinitePhysicsQueue[tail].flow = (cc_uint8)flow;
	infinitePhysicsCount++;
}
static cc_bool InfinitePhysics_IsLiquid(BlockID block) {
	return block == BLOCK_WATER || block == BLOCK_STILL_WATER || block == BLOCK_LAVA || block == BLOCK_STILL_LAVA;
}

/* Sponge interactions use world coordinates rather than finite-map indices. */
static void InfinitePhysics_PlaceSponge(int x, int y, int z) {
	int xx, yy, zz;
	for (yy = y - 2; yy <= y + 2; yy++)
	 for (zz = z - 2; zz <= z + 2; zz++)
	  for (xx = x - 2; xx <= x + 2; xx++) {
		BlockID block;
		if (!World_Contains(xx, yy, zz)) continue;
		block = World_GetBlock(xx, yy, zz);
		if (block == BLOCK_WATER || block == BLOCK_STILL_WATER)
			Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
	}
}

static void InfinitePhysics_DeleteSponge(int x, int y, int z) {
	int xx, yy, zz;
	for (yy = y - 3; yy <= y + 3; yy++)
	 for (zz = z - 3; zz <= z + 3; zz++)
	  for (xx = x - 3; xx <= x + 3; xx++) {
		BlockID block;
		if (Math_AbsI(xx - x) != 3 && Math_AbsI(yy - y) != 3 && Math_AbsI(zz - z) != 3) continue;
		if (!World_Contains(xx, yy, zz)) continue;
		block = World_GetBlock(xx, yy, zz);
		if (block == BLOCK_WATER || block == BLOCK_STILL_WATER)
			InfinitePhysics_Queue(xx, yy, zz, block, 0);
	}
}
static cc_bool InfinitePhysics_NearSponge(int x, int y, int z) {
	int xx, yy, zz;
	BlockID block;
	for (yy = max(0, y - 2); yy <= min(World.Height - 1, y + 2); yy++)
	 for (zz = z - 2; zz <= z + 2; zz++)
	  for (xx = x - 2; xx <= x + 2; xx++)
		if (InfiniteWorld_TryGetBlock(xx, yy, zz, &block) && block == BLOCK_SPONGE) return true;
	return false;
}
static void InfinitePhysics_SpreadTo(int x, int y, int z, BlockID liquid, int flow) {
	BlockID other;
	if (!World_Contains(x, y, z)) return;
	other = World_GetBlock(x, y, z);
	if (liquid == BLOCK_WATER || liquid == BLOCK_STILL_WATER) {
		if (other == BLOCK_LAVA || other == BLOCK_STILL_LAVA) Game_UpdateBlock(x, y, z, BLOCK_STONE);
		else if (Blocks.Draw[other] == DRAW_GAS && !InfinitePhysics_NearSponge(x, y, z)) {
			Game_UpdateBlock(x, y, z, BLOCK_WATER); InfinitePhysics_Queue(x, y, z, BLOCK_WATER, flow);
		}
	} else {
		if (other == BLOCK_WATER || other == BLOCK_STILL_WATER) Game_UpdateBlock(x, y, z, BLOCK_STONE);
		else if (Blocks.Draw[other] == DRAW_GAS) {
			Game_UpdateBlock(x, y, z, BLOCK_LAVA); InfinitePhysics_Queue(x, y, z, BLOCK_LAVA, flow);
		}
	}
}
static void InfinitePhysics_ActivateLiquid(int x, int y, int z, BlockID liquid, int flow) {
	int maxFlow = (liquid == BLOCK_LAVA || liquid == BLOCK_STILL_LAVA) ? 3 : 7;
	InfinitePhysics_SpreadTo(x, y - 1, z, liquid, flow);
	if (flow >= maxFlow) return;
	InfinitePhysics_SpreadTo(x - 1, y, z, liquid, flow + 1);
	InfinitePhysics_SpreadTo(x + 1, y, z, liquid, flow + 1);
	InfinitePhysics_SpreadTo(x, y, z - 1, liquid, flow + 1);
	InfinitePhysics_SpreadTo(x, y, z + 1, liquid, flow + 1);
}
/* Coordinate-based random ticks for the active area of an infinite world.
   Sampling is bounded per frame and centred on the local player. */
#define INFINITE_RANDOM_TICK_BUDGET 64
#define INFINITE_RANDOM_TICK_CHUNK_RADIUS 2

static void InfinitePhysics_RandomTick(int x, int y, int z, BlockID block) {
	BlockID below;
	int i, count, height;
	IVec3 coords[TREE_MAX_COUNT];
	BlockRaw blocks[TREE_MAX_COUNT];

	if (InfinitePhysics_IsFalling(block)) {
		InfinitePhysics_Fall(x, y, z);
		return;
	}
	if (InfinitePhysics_IsLiquid(block)) {
		InfinitePhysics_Queue(x, y, z, block, 0);
		return;
	}
	if (block == BLOCK_DIRT) {
		if (Lighting.IsLit(x, y, z)) Game_UpdateBlock(x, y, z, BLOCK_GRASS);
		return;
	}
	if (block == BLOCK_GRASS) {
		if (!Lighting.IsLit(x, y, z)) Game_UpdateBlock(x, y, z, BLOCK_DIRT);
		return;
	}
	if (block == BLOCK_DANDELION || block == BLOCK_ROSE) {
		if (!Lighting.IsLit(x, y, z)) {
			Game_UpdateBlock(x, y, z, BLOCK_AIR);
		} else {
			below = y > 0 ? World_GetBlock(x, y - 1, z) : BLOCK_DIRT;
			if (below != BLOCK_DIRT && below != BLOCK_GRASS) Game_UpdateBlock(x, y, z, BLOCK_AIR);
		}
		return;
	}
	if (block == BLOCK_RED_SHROOM || block == BLOCK_BROWN_SHROOM) {
		if (Lighting.IsLit(x, y, z)) {
			Game_UpdateBlock(x, y, z, BLOCK_AIR);
			return;
		}
		below = y > 0 ? World_GetBlock(x, y - 1, z) : BLOCK_STONE;
		if (below != BLOCK_STONE && below != BLOCK_COBBLE) Game_UpdateBlock(x, y, z, BLOCK_AIR);
		return;
	}
	if (block != BLOCK_SAPLING) return;

	below = y > 0 ? World_GetBlock(x, y - 1, z) : BLOCK_AIR;
	/* Saplings stay alive on dirt. */
	if (below == BLOCK_DIRT) return;
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
	if (below != BLOCK_GRASS || !Lighting.IsLit(x, y, z)) return;

	height = 5 + Random_Next(&physics_rnd, 3);
	if (!TreeGen_CanGrow(x, y, z, height)) return;
	count = TreeGen_Grow(x, y, z, height, coords, blocks);
	for (i = 0; i < count; i++)
		Game_UpdateBlock(coords[i].x, coords[i].y, coords[i].z, blocks[i]);
}

static void InfinitePhysics_TickRandomBlocks(void) {
	Vec3 position;
	int i, x, y, z, chunkX, chunkZ;
	if (!Entities.CurPlayer) return;
	position = Entities.CurPlayer->Base.Position;
	chunkX = InfiniteWorld_ChunkCoord((int)position.x);
	chunkZ = InfiniteWorld_ChunkCoord((int)position.z);

	for (i = 0; i < INFINITE_RANDOM_TICK_BUDGET; i++) {
		cc_int64 blockX, blockZ;
		int cx = chunkX + Random_Next(&physics_rnd, INFINITE_RANDOM_TICK_CHUNK_RADIUS * 2 + 1) - INFINITE_RANDOM_TICK_CHUNK_RADIUS;
		int cz = chunkZ + Random_Next(&physics_rnd, INFINITE_RANDOM_TICK_CHUNK_RADIUS * 2 + 1) - INFINITE_RANDOM_TICK_CHUNK_RADIUS;
		x = Random_Next(&physics_rnd, INFINITE_WORLD_CHUNK_SIZE);
		z = Random_Next(&physics_rnd, INFINITE_WORLD_CHUNK_SIZE);
		blockX = (cc_int64)cx * INFINITE_WORLD_CHUNK_SIZE + x;
		blockZ = (cc_int64)cz * INFINITE_WORLD_CHUNK_SIZE + z;
		if (blockX < Int32_MinValue || blockX > Int32_MaxValue ||
			blockZ < Int32_MinValue || blockZ > Int32_MaxValue) continue;
		x = (int)blockX;
		z = (int)blockZ;
		y = Random_Next(&physics_rnd, World.Height);
		InfinitePhysics_RandomTick(x, y, z, World_GetBlock(x, y, z));
	}
}

static void InfinitePhysics_Tick(void) {
	int i, count = min(infinitePhysicsCount, INFINITE_PHYSICS_TICK_BUDGET);
	for (i = 0; i < count; i++) {
		struct InfinitePhysicsEntry entry = infinitePhysicsQueue[infinitePhysicsHead];
		BlockID block;
		infinitePhysicsHead = (infinitePhysicsHead + 1) % INFINITE_PHYSICS_QUEUE_SIZE; infinitePhysicsCount--;
		block = World_GetBlock(entry.x, entry.y, entry.z);
		if (!InfinitePhysics_IsLiquid(block)) continue;
		if (entry.delay) {
			entry.delay--;
			infinitePhysicsQueue[(infinitePhysicsHead + infinitePhysicsCount) % INFINITE_PHYSICS_QUEUE_SIZE] = entry;
			infinitePhysicsCount++;
		} else InfinitePhysics_ActivateLiquid(entry.x, entry.y, entry.z, block, entry.flow);
	}
}

static void InfinitePhysics_ActivateAbove(int x, int y, int z) {
	BlockID above;
	if (y + 1 >= World.Height) return;

	above = World_GetBlock(x, y + 1, z);
	if (InfinitePhysics_IsFalling(above)) InfinitePhysics_Fall(x, y + 1, z);
}

void Physics_OnBlockChanged(int x, int y, int z, BlockID old, BlockID now) {
	PhysicsHandler handler;
	int index;
	if (!Physics.Enabled) return;
	if (World.Infinite) {
		if (InfinitePhysics_IsFalling(now)) InfinitePhysics_Fall(x, y, z);
		if (InfinitePhysics_IsLiquid(now)) InfinitePhysics_Queue(x, y, z, now, 0);
		if (now == BLOCK_SPONGE) InfinitePhysics_PlaceSponge(x, y, z);
		if (old == BLOCK_SPONGE && now == BLOCK_AIR) InfinitePhysics_DeleteSponge(x, y, z);
		if (now == BLOCK_AIR) {
			BlockID adjacent;
			adjacent = World_GetBlock(x - 1, y, z); if (InfinitePhysics_IsLiquid(adjacent)) InfinitePhysics_Queue(x - 1, y, z, adjacent, 0);
			adjacent = World_GetBlock(x + 1, y, z); if (InfinitePhysics_IsLiquid(adjacent)) InfinitePhysics_Queue(x + 1, y, z, adjacent, 0);
			adjacent = World_GetBlock(x, y, z - 1); if (InfinitePhysics_IsLiquid(adjacent)) InfinitePhysics_Queue(x, y, z - 1, adjacent, 0);
			adjacent = World_GetBlock(x, y, z + 1); if (InfinitePhysics_IsLiquid(adjacent)) InfinitePhysics_Queue(x, y, z + 1, adjacent, 0);
			adjacent = World_GetBlock(x, y - 1, z); if (InfinitePhysics_IsLiquid(adjacent)) InfinitePhysics_Queue(x, y - 1, z, adjacent, 0);
		}
		InfinitePhysics_ActivateAbove(x, y, z);
		return;
	}

	if (now == BLOCK_AIR && Physics_IsEdgeWater(x, y, z)) {
		now = BLOCK_STILL_WATER;
		Game_UpdateBlock(x, y, z, BLOCK_STILL_WATER);
	}
	index = World_Pack(x, y, z);

	/* User can place/delete blocks over ID 256 */
	if (now == BLOCK_AIR) {
		handler = Physics.OnDelete[(BlockRaw)old];
		if (handler) handler(index, old);
	} else {
		handler = Physics.OnPlace[(BlockRaw)now];
		if (handler) handler(index, now);
	}
	Physics_ActivateNeighbours(x, y, z, index);
}

static void Physics_TickRandomBlocks(void) {
	int lo, hi, index;
	BlockID block;
	PhysicsHandler tick;
	int x, y, z, x2, y2, z2;

	for (y = 0; y < World.Height; y += CHUNK_SIZE) {
		y2 = min(y + CHUNK_MAX, World.MaxY);
		for (z = 0; z < World.Length; z += CHUNK_SIZE) {
			z2 = min(z + CHUNK_MAX, World.MaxZ);
			for (x = 0; x < World.Width; x += CHUNK_SIZE) {
				x2 = min(x + CHUNK_MAX, World.MaxX);

				/* Inlined 3 random ticks for this chunk */
				lo = World_Pack( x,  y,  z);
				hi = World_Pack(x2, y2, z2);
				
				index = Random_Range(&physics_rnd, lo, hi);
				block = World.Blocks[index];
				tick = Physics.OnRandomTick[block];
				if (tick) tick(index, block);

				index = Random_Range(&physics_rnd, lo, hi);
				block = World.Blocks[index];
				tick = Physics.OnRandomTick[block];
				if (tick) tick(index, block);

				index = Random_Range(&physics_rnd, lo, hi);
				block = World.Blocks[index];
				tick = Physics.OnRandomTick[block];
				if (tick) tick(index, block);
			}
		}
	}
}


static void Physics_DoFalling(int index, BlockID block) {
	int found = -1, start = index;
	BlockID other;
	int x, y, z;

	/* Find lowest block can fall into */
	while (index >= World.OneY) {
		index -= World.OneY;
		other  = World.Blocks[index];

		if (other == BLOCK_AIR || (other >= BLOCK_WATER && other <= BLOCK_STILL_LAVA))
			found = index;
		else
			break;
	}

	if (found == -1) return;
	World_Unpack(found, x, y, z);
	Game_UpdateBlock(x, y, z, block);

	World_Unpack(start, x, y, z);
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
	Physics_ActivateNeighbours(x, y, z, start);
}

static cc_bool Physics_CheckItem(struct TickQueue* queue, int* posIndex) {
	cc_uint32 item = TickQueue_Dequeue(queue);
	*posIndex     = (int)(item & PHYSICS_POS_MASK);

	if (item >= PHYSICS_ONE_DELAY) {
		item -= PHYSICS_ONE_DELAY;
		TickQueue_Enqueue(queue, item);
		return false;
	}
	return true;
}


static void Physics_HandleSapling(int index, BlockID block) {
	IVec3 coords[TREE_MAX_COUNT];
	BlockRaw blocks[TREE_MAX_COUNT];
	int i, count, height;

	BlockID below;
	int x, y, z;
	World_Unpack(index, x, y, z);

	below = BLOCK_AIR;
	if (y > 0) below = World.Blocks[index - World.OneY];
	/* Saplings stay alive on dirt */
	if (below == BLOCK_DIRT) return;

	/* Saplings grow if on grass in light, otherwise turn to air */
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
	if (below != BLOCK_GRASS || !Lighting.IsLit(x, y, z)) return;

	height = 5 + Random_Next(&physics_rnd, 3);

	if (TreeGen_CanGrow(x, y, z, height)) {
		count = TreeGen_Grow(x, y, z, height, coords, blocks);

		for (i = 0; i < count; i++) 
		{
			Game_UpdateBlock(coords[i].x, coords[i].y, coords[i].z, blocks[i]);
		}
	}
}

static void Physics_HandleDirt(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_GRASS);
	}
}

static void Physics_HandleGrass(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (!Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_DIRT);
	}
}

static void Physics_HandleFlower(int index, BlockID block) {
	BlockID below;
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (!Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_AIR);
		Physics_ActivateNeighbours(x, y, z, index);
		return;
	}

	below = BLOCK_DIRT;
	if (y > 0) below = World.Blocks[index - World.OneY];
	if (!(below == BLOCK_DIRT || below == BLOCK_GRASS)) {
		Game_UpdateBlock(x, y, z, BLOCK_AIR);
		Physics_ActivateNeighbours(x, y, z, index);
	}
}

static void Physics_HandleMushroom(int index, BlockID block) {
	BlockID below;
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_AIR);
		Physics_ActivateNeighbours(x, y, z, index);
		return;
	}

	below = BLOCK_STONE;
	if (y > 0) below = World.Blocks[index - World.OneY];
	if (!(below == BLOCK_STONE || below == BLOCK_COBBLE)) {
		Game_UpdateBlock(x, y, z, BLOCK_AIR);
		Physics_ActivateNeighbours(x, y, z, index);
	}
}


static void Physics_PlaceLava(int index, BlockID block) {
	TickQueue_Enqueue(&lavaQ, PHYSICS_LAVA_DELAY | index);
}

static void Physics_PropagateLava(int posIndex, int x, int y, int z) {
	BlockID block = World.Blocks[posIndex];

	if (block >= BLOCK_WATER && block <= BLOCK_STILL_LAVA) {
		/* Lava spreading into water turns the water solid */
		if (block == BLOCK_WATER || block == BLOCK_STILL_WATER) {
			Game_UpdateBlock(x, y, z, BLOCK_STONE);
		}
	} else if (Blocks.Draw[block] == DRAW_GAS) {
		TickQueue_Enqueue(&lavaQ, PHYSICS_LAVA_DELAY | posIndex);
		Game_UpdateBlock(x, y, z, BLOCK_LAVA);
	}
}

static void Physics_ActivateLava(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (x > 0)          Physics_PropagateLava(index - 1, x - 1, y, z);
	if (x < World.MaxX) Physics_PropagateLava(index + 1, x + 1, y, z);
	if (z > 0)          Physics_PropagateLava(index - World.Width, x, y, z - 1);
	if (z < World.MaxZ) Physics_PropagateLava(index + World.Width, x, y, z + 1);
	if (y > 0)          Physics_PropagateLava(index - World.OneY, x, y - 1, z);
}

static void Physics_TickLava(void) {
	int i, count = lavaQ.count;
	for (i = 0; i < count; i++) {
		int index;
		if (Physics_CheckItem(&lavaQ, &index)) {
			BlockID block = World.Blocks[index];
			if (!(block == BLOCK_LAVA || block == BLOCK_STILL_LAVA)) continue;
			Physics_ActivateLava(index, block);
		}
	}
}


static void Physics_PlaceWater(int index, BlockID block) {
	TickQueue_Enqueue(&waterQ, PHYSICS_WATER_DELAY | index);
}

static void Physics_PropagateWater(int posIndex, int x, int y, int z) {
	BlockID block = World.Blocks[posIndex];
	int xx, yy, zz;

	if (block >= BLOCK_WATER && block <= BLOCK_STILL_LAVA) {
		/* Water spreading into lava turns the lava solid */
		if (block == BLOCK_LAVA || block == BLOCK_STILL_LAVA) {
			Game_UpdateBlock(x, y, z, BLOCK_STONE);
		}
	} else if (Blocks.Draw[block] == DRAW_GAS) {
		/* Sponge check */		
		for (yy = (y < 2 ? 0 : y - 2); yy <= (y > physics_maxWaterY ? World.MaxY : y + 2); yy++) {
			for (zz = (z < 2 ? 0 : z - 2); zz <= (z > physics_maxWaterZ ? World.MaxZ : z + 2); zz++) {
				for (xx = (x < 2 ? 0 : x - 2); xx <= (x > physics_maxWaterX ? World.MaxX : x + 2); xx++) {
					block = World_GetBlock(xx, yy, zz);
					if (block == BLOCK_SPONGE) return;
				}
			}
		}

		TickQueue_Enqueue(&waterQ, PHYSICS_WATER_DELAY | posIndex);
		Game_UpdateBlock(x, y, z, BLOCK_WATER);
	}
}

static void Physics_ActivateWater(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (x > 0)          Physics_PropagateWater(index - 1,           x - 1, y,     z);
	if (x < World.MaxX) Physics_PropagateWater(index + 1,           x + 1, y,     z);
	if (z > 0)          Physics_PropagateWater(index - World.Width, x,     y,     z - 1);
	if (z < World.MaxZ) Physics_PropagateWater(index + World.Width, x,     y,     z + 1);
	if (y > 0)          Physics_PropagateWater(index - World.OneY,  x,     y - 1, z);
}

static void Physics_TickWater(void) {
	int i, count = waterQ.count;
	for (i = 0; i < count; i++) {
		int index;
		if (Physics_CheckItem(&waterQ, &index)) {
			BlockID block = World.Blocks[index];
			if (!(block == BLOCK_WATER || block == BLOCK_STILL_WATER)) continue;
			Physics_ActivateWater(index, block);
		}
	}
}


static void Physics_PlaceSponge(int index, BlockID block) {
	int x, y, z, xx, yy, zz;
	World_Unpack(index, x, y, z);

	for (yy = y - 2; yy <= y + 2; yy++) {
		for (zz = z - 2; zz <= z + 2; zz++) {
			for (xx = x - 2; xx <= x + 2; xx++) {
				if (!World_Contains(xx, yy, zz)) continue;

				block = World_GetBlock(xx, yy, zz);
				if (block == BLOCK_WATER || block == BLOCK_STILL_WATER) {
					Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
				}
			}
		}
	}
}

static void Physics_DeleteSponge(int index, BlockID block) {
	int x, y, z, xx, yy, zz;
	World_Unpack(index, x, y, z);

	for (yy = y - 3; yy <= y + 3; yy++) {
		for (zz = z - 3; zz <= z + 3; zz++) {
			for (xx = x - 3; xx <= x + 3; xx++) {
				if (Math_AbsI(yy - y) == 3 || Math_AbsI(zz - z) == 3 || Math_AbsI(xx - x) == 3) {
					if (!World_Contains(xx, yy, zz)) continue;

					index = World_Pack(xx, yy, zz);
					block = World.Blocks[index];
					if (block == BLOCK_WATER || block == BLOCK_STILL_WATER) {
						TickQueue_Enqueue(&waterQ, index | PHYSICS_ONE_DELAY);
					}
				}
			}
		}
	}
}


static void Physics_HandleSlab(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);
	if (index < World.OneY) return;

	if (World.Blocks[index - World.OneY] != BLOCK_SLAB) return;
	Game_UpdateBlock(x, y,     z, BLOCK_AIR);
	Game_UpdateBlock(x, y - 1, z, BLOCK_DOUBLE_SLAB);
}

static void Physics_HandleCobblestoneSlab(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);
	if (index < World.OneY) return;

	if (World.Blocks[index - World.OneY] != BLOCK_COBBLE_SLAB) return;
	Game_UpdateBlock(x, y,     z, BLOCK_AIR);
	Game_UpdateBlock(x, y - 1, z, BLOCK_COBBLE);
}


/* TODO: should this be moved into a precomputed lookup table, instead of calculating every time? */
/*  performance difference probably isn't enough to really matter */
static cc_bool BlocksTNT(BlockID b) {
	/* NOTE: A bit hacky, but works well enough */
	return (b >= BLOCK_WATER && b <= BLOCK_STILL_LAVA) || 
		(Blocks.ExtendedCollide[b] == COLLIDE_SOLID && (Blocks.DigSounds[b] == SOUND_METAL || Blocks.DigSounds[b] == SOUND_STONE));
}

#define TNT_POWER 4
#define TNT_POWER_SQUARED (TNT_POWER * TNT_POWER)
static void Physics_HandleTnt(int index, BlockID block) {
	int x, y, z;
	int dx, dy, dz, xx, yy, zz;

	World_Unpack(index, x, y, z);
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
	Physics_ActivateNeighbours(x, y, z, index);
	
	for (dy = -TNT_POWER; dy <= TNT_POWER; dy++) {
		for (dz = -TNT_POWER; dz <= TNT_POWER; dz++) {
			for (dx = -TNT_POWER; dx <= TNT_POWER; dx++) {
				if (dx * dx + dy * dy + dz * dz > TNT_POWER_SQUARED) continue;

				xx = x + dx; yy = y + dy; zz = z + dz;
				if (!World_Contains(xx, yy, zz)) continue;
				index = World_Pack(xx, yy, zz);

				block = World.Blocks[index];
				if (BlocksTNT(block)) continue;

				Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
				Physics_ActivateNeighbours(xx, yy, zz, index);
			}
		}
	}
}

void Physics_Init(void) {
	Event_Register_(&WorldEvents.MapLoaded,    NULL, Physics_OnNewMapLoaded);
	Physics.Enabled = Options_GetBool(OPT_BLOCK_PHYSICS, true);
	TickQueue_Init(&lavaQ);
	TickQueue_Init(&waterQ);

	Physics.OnPlace[BLOCK_SAND]        = Physics_DoFalling;
	Physics.OnPlace[BLOCK_GRAVEL]      = Physics_DoFalling;
	Physics.OnActivate[BLOCK_SAND]     = Physics_DoFalling;
	Physics.OnActivate[BLOCK_GRAVEL]   = Physics_DoFalling;
	Physics.OnRandomTick[BLOCK_SAND]   = Physics_DoFalling;
	Physics.OnRandomTick[BLOCK_GRAVEL] = Physics_DoFalling;

	Physics.OnRandomTick[BLOCK_SAPLING] = Physics_HandleSapling;
	Physics.OnRandomTick[BLOCK_DIRT]    = Physics_HandleDirt;
	Physics.OnRandomTick[BLOCK_GRASS]   = Physics_HandleGrass;

	Physics.OnRandomTick[BLOCK_DANDELION]    = Physics_HandleFlower;
	Physics.OnRandomTick[BLOCK_ROSE]         = Physics_HandleFlower;
	Physics.OnRandomTick[BLOCK_RED_SHROOM]   = Physics_HandleMushroom;
	Physics.OnRandomTick[BLOCK_BROWN_SHROOM] = Physics_HandleMushroom;

	Physics.OnPlace[BLOCK_LAVA]    = Physics_PlaceLava;
	Physics.OnPlace[BLOCK_WATER]   = Physics_PlaceWater;
	Physics.OnPlace[BLOCK_SPONGE]  = Physics_PlaceSponge;
	Physics.OnDelete[BLOCK_SPONGE] = Physics_DeleteSponge;

	Physics.OnActivate[BLOCK_WATER]       = Physics.OnPlace[BLOCK_WATER];
	Physics.OnActivate[BLOCK_STILL_WATER] = Physics.OnPlace[BLOCK_WATER];
	Physics.OnActivate[BLOCK_LAVA]        = Physics.OnPlace[BLOCK_LAVA];
	Physics.OnActivate[BLOCK_STILL_LAVA]  = Physics.OnPlace[BLOCK_LAVA];

	Physics.OnRandomTick[BLOCK_WATER]       = Physics_ActivateWater;
	Physics.OnRandomTick[BLOCK_STILL_WATER] = Physics_ActivateWater;
	Physics.OnRandomTick[BLOCK_LAVA]        = Physics_ActivateLava;
	Physics.OnRandomTick[BLOCK_STILL_LAVA]  = Physics_ActivateLava;

	Physics.OnPlace[BLOCK_SLAB]        = Physics_HandleSlab;
	if (Game_ClassicMode) return;
	Physics.OnPlace[BLOCK_COBBLE_SLAB] = Physics_HandleCobblestoneSlab;
	Physics.OnPlace[BLOCK_TNT]         = Physics_HandleTnt;
}

void Physics_Free(void) {
	Event_Unregister_(&WorldEvents.MapLoaded,    NULL, Physics_OnNewMapLoaded);
}

void Physics_Tick(void) {
	if (!Physics.Enabled) return;
	if (World.Infinite) { InfinitePhysics_Tick(); InfinitePhysics_TickRandomBlocks(); physics_tickCount++; return; }
	if (!World.Blocks) return;

	/*if ((tickCount % 5) == 0) {*/
	Physics_TickLava();
	Physics_TickWater();
	/*}*/
	physics_tickCount++;
	Physics_TickRandomBlocks();
}
