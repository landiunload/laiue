#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "api.h"

typedef bool (*VoxelSolidBlockQuery)(
    void* context, int64_t x, int64_t y, int64_t z);

typedef struct VoxelCollisionSource
{
    void* context;
    VoxelSolidBlockQuery isSolidBlock;
} VoxelCollisionSource;

typedef struct VoxelBodyShape
{
    double radius;
    double height;
    double eyeHeight;
    double collisionEpsilon;
} VoxelBodyShape;

typedef struct VoxelBodyBounds
{
    double minimum[3];
    double maximum[3];
} VoxelBodyBounds;

LAIUE_PHYSICS_API void VoxelBodyCalculateBounds(
    const double position[3], const VoxelBodyShape* shape,
    VoxelBodyBounds* outBounds);

LAIUE_PHYSICS_API bool VoxelBodyCollides(
    const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape);

// Двигает тело по одной оси и возвращает true при столкновении.
LAIUE_PHYSICS_API bool VoxelBodyMoveAxis(
    const VoxelCollisionSource* collision,
    double position[3], const VoxelBodyShape* shape,
    int32_t axis, double distance);

// Любая опора под AABB: используется для обычного контакта с землёй.
LAIUE_PHYSICS_API bool VoxelBodyHasGroundContact(
    const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth);

// Опора под внутренней областью стоп. Нужна для защиты края при приседании:
// тело может свисать, но его центр не уходит в пустоту.
LAIUE_PHYSICS_API bool VoxelBodyHasStableGround(
    const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth, double supportRadius);

LAIUE_PHYSICS_API bool VoxelBodyOverlapsBlock(
    const double position[3], const VoxelBodyShape* shape,
    const int64_t block[3]);
