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

typedef struct VoxelBodyBounds
{
    double minimum[3];
    double maximum[3];
} VoxelBodyBounds;

enum
{
    VOXEL_DYNAMIC_COLLIDER_CAPACITY = 32u,
};

// Точный внешний коллайдер, не привязанный к целочисленной сетке мира.
// stableId обязан быть ненулевым и уникальным внутри одной полной выборки;
// эквивалентные полные выборки обязаны сохранять те же ID.
typedef struct VoxelDynamicCollider
{
    VoxelBodyBounds bounds;
    double velocity[3];
    float friction;
    uint64_t stableId;
} VoxelDynamicCollider;

// Callback обязан при каждом вызове полностью записать flags и friction и
// быть read-only для одного simulation snapshot: collision API может вызвать
// его несколько раз. Все функции требуют ненулевые source, callback и
// выходные указатели; context может быть NULL, если source он не нужен.
typedef void (*VoxelBlockPhysicsQuery)(
    void* context, int64_t x, int64_t y, int64_t z,
    VoxelBlockPhysics* outBlock);

// Не больше одного bounded broadphase-запроса на одну collision-операцию.
// Controller может выполнить несколько таких операций за fixed step, поэтому
// callback обязан быть read-only для одного simulation snapshot. Он записывает
// не больше colliderCapacity элементов и их точное число. true означает, что
// выборка полна; false означает truncation/error и обрабатывается fail-closed.
typedef bool (*VoxelDynamicColliderQuery)(
    void* context, const VoxelBodyBounds* queryBounds,
    VoxelDynamicCollider* outColliders, uint32_t colliderCapacity,
    uint32_t* outColliderCount);

typedef struct VoxelCollisionSource
{
    void* context;
    VoxelBlockPhysicsQuery queryBlockPhysics;
    VoxelDynamicColliderQuery queryDynamicColliders;
} VoxelCollisionSource;

typedef struct VoxelGroundContact
{
    float friction;
    bool supported;
    // Нулевая скорость и stableId == 0 обозначают статическую опору.
    double surfaceVelocity[3];
    uint64_t surfaceStableId;
} VoxelGroundContact;

typedef struct VoxelBodyShape
{
    // Все значения конечны; radius > collisionEpsilon,
    // height > 2 * collisionEpsilon, eyeHeight лежит в [0, height].
    double radius;
    double height;
    double eyeHeight;
    double collisionEpsilon;
} VoxelBodyShape;

LAIUE_PHYSICS_API void VoxelBodyCalculateBounds(
    const double position[3], const VoxelBodyShape* shape,
    VoxelBodyBounds* outBounds);

LAIUE_PHYSICS_API bool VoxelBodyCollides(
    const VoxelCollisionSource* collision,
    const double position[3], const VoxelBodyShape* shape);

// Двигает тело по одной оси и возвращает true при столкновении. Неконечные,
// выходящие за simulation range значения и неверная ось fail-closed без
// изменения position и без вызова broadphase callback.
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
