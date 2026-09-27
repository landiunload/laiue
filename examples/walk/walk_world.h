#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WALK_WORLD_INTERACTION_DISTANCE 32.0f

typedef uint8_t (*WalkWorldGetBlockFn)(void *context, int64_t x, int64_t y,
                                       int64_t z);
typedef bool (*WalkWorldSetBlockFn)(void *context, int64_t x, int64_t y,
                                    int64_t z, uint8_t material);

typedef struct WalkWorldBlockHit
{
    int64_t block[3];
    int64_t previousBlock[3];
} WalkWorldBlockHit;

/* origin is local to offset, so billion-kilometre world coordinates never
 * lose the sub-block precision needed by the DDA traversal. */
bool WalkWorldRaycast(WalkWorldGetBlockFn getBlock, void *context,
                      const double origin[3], const int64_t offset[3],
                      const float direction[3], float maximumDistance,
                      WalkWorldBlockHit *outHit);
bool WalkWorldEditHit(WalkWorldSetBlockFn setBlock, void *context,
                      const WalkWorldBlockHit *hit, bool place,
                      uint8_t material);
