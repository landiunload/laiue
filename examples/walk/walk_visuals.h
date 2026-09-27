#pragma once

#include "graphics/graphics_device_v2.h"
#include "physics/ragdoll.h"

#include <stdbool.h>
#include <stdint.h>

#define WALK_VISUAL_TEXTURE_COUNT 3u
#define WALK_TERRAIN_SKIRT_VERTEX_COUNT 24u
#define WALK_RAGDOLL_VISUAL_VERTEX_COUNT 9792u

typedef struct WalkRagdollVisualScratch
{
    LaiueGraphicsVertexV2 vertices[WALK_RAGDOLL_VISUAL_VERTEX_COUNT];
} WalkRagdollVisualScratch;

/* Platform adapters provide encoded asset bytes allocated with PlatformAllocate.
 * The shared visual layer owns and releases the returned buffer. */
typedef bool (*WalkReadAssetFn)(void *context, const char *relativePath,
                                uint8_t **outBytes, uint32_t *outSize);

bool WalkVisualsCreateTerrain(LaiueGraphicsDeviceV2 *device,
                              WalkReadAssetFn readAsset, void *assetContext,
                              LaiueGraphicsHandle outBuffers[WALK_VISUAL_TEXTURE_COUNT],
                              LaiueGraphicsHandle outTextures[WALK_VISUAL_TEXTURE_COUNT],
                              LaiueGraphicsHandle *outSampler);
void WalkVisualsDestroyTerrain(LaiueGraphicsDeviceV2 *device,
                               LaiueGraphicsHandle buffers[WALK_VISUAL_TEXTURE_COUNT],
                               LaiueGraphicsHandle textures[WALK_VISUAL_TEXTURE_COUNT],
                               LaiueGraphicsHandle *sampler);

bool WalkVisualsCreateRagdollBuffer(LaiueGraphicsDeviceV2 *device,
                                    LaiueGraphicsHandle *outBuffer);
bool WalkVisualsUpdateRagdollBuffer(LaiueGraphicsDeviceV2 *device,
                                    LaiueGraphicsHandle buffer,
                                    const VoxelRagdoll *ragdoll,
                                    const double renderOrigin[3],
                                    WalkRagdollVisualScratch *scratch);
