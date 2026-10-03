#pragma once

// Тесты и бенчмарки компонуют physics напрямую, а код walk обращается к
// телам только через таблицу laiue.physics. Здесь таблица собирается из
// тех же функций, что публикует модуль, и привязывается к walk.

#include "../examples/walk/walk_physics.h"
#include "physics/physics_service.h"

static inline bool BindLinkedWalkPhysics(void)
{
    static const LaiuePhysicsServiceV1 table = {
        .structSize = sizeof(LaiuePhysicsServiceV1),
        .abiVersion = LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
        .bodyWake = VoxelRigidBodyWake,
        .ragdollJump = VoxelRagdollJump,
        .bodyLocalPosition = VoxelRigidBodyLocalPosition,
        .bodyOrientationMatrix = VoxelRigidBodyOrientationMatrix,
        .bodyLinearVelocity = VoxelRigidBodyLinearVelocity,
        .bodyAngularVelocity = VoxelRigidBodyAngularVelocity,
        .bodyAddLinearVelocity = VoxelRigidBodyAddLinearVelocity,
        .bodyAddAngularVelocity = VoxelRigidBodyAddAngularVelocity,
        .bodyTranslateBlocks = VoxelRigidBodyTranslateBlocks,
    };
    return WalkPhysicsBind(&table, (uint32_t)sizeof(table));
}
