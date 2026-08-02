#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "construct/physical_construct.h"
#include "physics/voxel_body.h"
#include "world/world.h"

#define VOXEL_INTERACTION_PHYSICS_LEVER_ITEM 3U

typedef enum VoxelEditType
{
    VOXEL_EDIT_NONE,
    VOXEL_EDIT_BREAK,
    VOXEL_EDIT_PLACE,
    VOXEL_EDIT_ACTIVATE_LEVER,
} VoxelEditType;

typedef enum VoxelEditSpace
{
    VOXEL_EDIT_SPACE_WORLD,
    VOXEL_EDIT_SPACE_CONSTRUCT,
} VoxelEditSpace;

typedef enum VoxelSceneHitKind
{
    VOXEL_SCENE_HIT_WORLD,
    VOXEL_SCENE_HIT_CONSTRUCT_VOXEL,
    VOXEL_SCENE_HIT_LEVER_BASE,
    VOXEL_SCENE_HIT_LEVER_HANDLE,
} VoxelSceneHitKind;

typedef struct VoxelSceneHit
{
    VoxelSceneHitKind kind;
    double distance;
    int8_t normal[3];
    int64_t worldBlock[3];
    uint64_t bodyId;
    int32_t localBlock[3];
    BlockType material;
} VoxelSceneHit;

typedef struct VoxelEdit
{
    VoxelEditType type;
    VoxelEditSpace space;
    int64_t block[3];
    uint64_t bodyId;
    int32_t localBlock[3];
    int8_t mountNormal[3];
    BlockType original;
    BlockType replacement;
} VoxelEdit;

LAIUE_INTERACTION_API bool VoxelInteractionRaycastScene(
    World* world, const PhysicalConstructSystem* constructs,
    const double origin[3], const float direction[3],
    float maximumDistance, VoxelSceneHit* outHit);

LAIUE_INTERACTION_API bool VoxelInteractionTryCreateEdit(
    World* world, PhysicalConstructSystem* constructs,
    const double origin[3], const float direction[3],
    const PhysicalConstructAabb* blockingBodies,
    uint32_t blockingBodyCount,
    bool breakPressed, bool placePressed, uint16_t placementItem,
    float maximumDistance,
    VoxelEdit* outEdit);
