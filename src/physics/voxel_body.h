#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "api.h"

enum
{
    VOXEL_BLOCK_PHYSICS_SOLID = 1u << 0,
};

// Минимальное физическое представление блока. Physics не зависит от
// BlockType и от реализации мира, поэтому новые миры могут предоставлять
// те же свойства через собственный callback.
typedef struct VoxelBlockPhysics
{
    uint32_t flags;
    // Нормализованный конечный коэффициент в диапазоне [0, 1].
    float friction;
} VoxelBlockPhysics;

// Callback обязан при каждом вызове полностью записать flags и friction.
// Все функции collision API требуют ненулевые source, callback и выходные
// указатели; context может быть NULL, если конкретному source он не нужен.
typedef void (*VoxelBlockPhysicsQuery)(
    void* context, int64_t x, int64_t y, int64_t z,
    VoxelBlockPhysics* outBlock);

typedef struct VoxelCollisionSource
{
    void* context;
    VoxelBlockPhysicsQuery queryBlockPhysics;
} VoxelCollisionSource;

typedef struct VoxelGroundContact
{
    float friction;
    bool supported;
} VoxelGroundContact;

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
// Трение нескольких блоков усредняется по площади опоры под стопами.
LAIUE_PHYSICS_API void VoxelBodyQueryGroundContact(
    const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth, VoxelGroundContact* outContact);

// Совместимый сокращённый запрос, если свойства поверхности не нужны.
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

// Minecraft-подобная sneak-защита: уменьшает только добровольное
// горизонтальное перемещение так, чтобы AABB сохранял пересечение с опорой.
// Внешние импульсы должны вызывать VoxelBodyMoveAxis напрямую.
LAIUE_PHYSICS_API void VoxelBodyClipSneakingMovement(
    const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape,
    double probeDepth, double* xDistance, double* yDistance);
