#pragma once

#include "physics/compound_shape.h"
#include "physics/ragdoll.h"
#include "physics/rigid_body.h"
#include "physics/voxel_body.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_PHYSICS_SERVICE_NAME "laiue.physics"
#define LAIUE_PHYSICS_SERVICE_ABI_VERSION_1 1u

/* The service table intentionally mirrors the allocation and step contracts
 * of physics without exposing implementation ownership.  Callers provide
 * all body, cache and scratch storage; the module never hides an allocator in
 * a callback, which keeps deterministic replay and optional loading intact. */
typedef struct LaiuePhysicsServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    void (*configureThread)(void);
    uint32_t (*threadIsConfigured)(void);
    bool (*bodyInitialize)(VoxelRigidBody *body, uint64_t stableId,
                           const VoxelRigidBodyDescription *description);
    void (*bodyRelease)(VoxelRigidBody *body);
    void (*bodyWake)(VoxelRigidBody *body);
    uint32_t (*stepScratchBytes)(uint32_t bodyCount);
    bool (*step)(VoxelRigidBody *bodies, uint32_t bodyCount,
                 const VoxelCollisionSource *collision,
                 const VoxelRigidStepSettings *settings, void *scratch,
                 uint32_t scratchBytes);
    bool (*stepEx)(VoxelRigidBody *bodies, uint32_t bodyCount,
                   const VoxelCollisionSource *collision,
                   const VoxelRigidStepSettings *settings, void *scratch,
                   uint32_t scratchBytes, const VoxelRigidStepOptions *options);
    bool (*stepCached)(VoxelRigidBody *bodies, uint32_t bodyCount,
                       const VoxelCollisionSource *collision,
                       const VoxelRigidStepSettings *settings, void *scratch,
                       uint32_t scratchBytes, VoxelRigidContactCache *contactCache);
    bool (*stepIndexed)(VoxelRigidBody *bodies, uint32_t bodyCount,
                        const VoxelCollisionSource *collision,
                        const VoxelRigidStepSettings *settings, void *scratch,
                        uint32_t scratchBytes, VoxelRigidContactCache *contactCache,
                        VoxelRigidBroadphase *broadphase);
    uint32_t (*compoundScratchBytes)(uint32_t bodyCount, uint32_t primitiveCount);
    bool (*stepCompoundEx)(VoxelRigidBody *bodies, uint32_t bodyCount,
                           const VoxelCollisionSource *collision,
                           const VoxelRigidStepSettings *settings, void *scratch,
                           uint32_t scratchBytes, const VoxelRigidStepOptions *options,
                           const VoxelRigidCompoundShape *shapes);
    bool (*compoundMassProperties)(const VoxelRigidCompoundBox *boxes,
                                   uint32_t boxCount, double mass,
                                   double outCenter[3], double outHalfExtent[3],
                                   double outInverseInertia[9]);
    bool (*compoundMergeBoxes)(const VoxelRigidCompoundBox *boxes, uint32_t count,
                               VoxelRigidCompoundBox *outBoxes, uint32_t capacity,
                               uint32_t *outCount);
    uint32_t (*contactCacheBytes)(uint32_t bodyCapacity);
    bool (*contactCacheInitialize)(VoxelRigidContactCache *cache, void *storage,
                                   uint32_t bodyCapacity, uint32_t storageBytes);
    void (*contactCacheReset)(VoxelRigidContactCache *cache);
    /* Optional ABI-v1 tail added after the original physics service fields. */
    bool (*ragdollInitialize)(VoxelRagdoll *ragdoll,
                              const VoxelRagdollDefinition *definition);
    void (*ragdollRelease)(VoxelRagdoll *ragdoll);
    void (*ragdollSettingsDefault)(VoxelRagdollSettings *outSettings);
    bool (*ragdollStep)(VoxelRagdoll *ragdoll, const VoxelCollisionSource *collision,
                        const VoxelRigidStepSettings *rigidSettings,
                        const VoxelRagdollSettings *settings, void *scratch,
                        uint32_t scratchBytes, const VoxelRigidStepOptions *options);
    bool (*ragdollDrive)(VoxelRagdoll *ragdoll, double directionX, double directionY,
                         double targetSpeed, double maximumAcceleration);
    bool (*ragdollJump)(VoxelRagdoll *ragdoll, double upwardSpeed);
    /* Optional ABI-v1 tail: per-body state accessors.  A game that drives
     * bodies only through this table does not import the physics library,
     * so removing the artifact disables physics instead of the process. */
    bool (*bodyLocalPosition)(const VoxelRigidBody *body, double outPosition[3]);
    void (*bodyOrientationMatrix)(const VoxelRigidBody *body, float outMatrix[9]);
    bool (*bodyLinearVelocity)(const VoxelRigidBody *body, double outVelocity[3]);
    bool (*bodyAngularVelocity)(const VoxelRigidBody *body, double outVelocity[3]);
    bool (*bodyAddLinearVelocity)(VoxelRigidBody *body, const double delta[3]);
    bool (*bodyAddAngularVelocity)(VoxelRigidBody *body, const double delta[3]);
    bool (*bodyTranslateBlocks)(VoxelRigidBody *body, const int64_t blockShift[3]);
} LaiuePhysicsServiceV1;

const LaiueModuleApiV1 *LaiuePhysicsGetStaticModuleApiV1(void);
