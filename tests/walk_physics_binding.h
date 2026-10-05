#pragma once

// Тесты и бенчмарки компонуют physics напрямую, а код walk обращается к
// телам только через таблицу laiue.physics. Здесь таблица собирается из
// тех же функций, что публикует модуль, и привязывается к walk.

#include "../examples/walk/walk_physics.h"
#include "physics/physics_service.h"

static WalkPhysicsContext linkedWalkPhysicsContext;
static LaiuePhysicsServiceV1 linkedWalkPhysicsService;

static inline bool BindLinkedWalkPhysics(void)
{
    // MSVC не принимает адрес dllimport-функции в статическом инициализаторе
    // (C4232), поэтому таблица, переживающая вызов, заполняется здесь.
    linkedWalkPhysicsService = (LaiuePhysicsServiceV1){
        .structSize = sizeof(LaiuePhysicsServiceV1),
        .abiVersion = LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
        .configureThread = VoxelPhysicsConfigureThread,
        .stepScratchBytes = VoxelRigidBodyStepScratchBytes,
        .ragdollInitialize = VoxelRagdollInitialize,
        .ragdollRelease = VoxelRagdollRelease,
        .ragdollSettingsDefault = VoxelRagdollSettingsDefault,
        .ragdollStep = VoxelRagdollStep,
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
    return WalkPhysicsBind(&linkedWalkPhysicsContext, &linkedWalkPhysicsService,
                           (uint32_t)sizeof(linkedWalkPhysicsService));
}
