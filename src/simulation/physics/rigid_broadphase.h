#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Persistent, caller-owned broadphase. Allocation and descriptor must not
// overlap. Only Initialize/Reset/StepIndexed may modify the descriptor/storage;
// diagnostic counters may be read after a successful step. Node indices are
// private; proxy slots are body-array indices.
typedef struct VoxelRigidBroadphase
{
    void *storage;
    uint32_t storageBytes;
    uint32_t bodyCapacity;
    uint32_t root;
    uint32_t freeList;
    uint32_t proxyCount;
    uint32_t updatedProxyCount;
    uint32_t visitedNodeCount;
    // The simulation's synchronization owner sets this after updating ALL slots.
    uint32_t indexedBodyCount;
} VoxelRigidBroadphase;

LAIUE_PHYSICS_API uint32_t VoxelRigidBroadphaseBytes(uint32_t bodyCapacity);
// Invalid arguments leave the descriptor and allocation unchanged.
LAIUE_PHYSICS_API bool VoxelRigidBroadphaseInitialize(VoxelRigidBroadphase *broadphase,
                                                      void *storage, uint32_t bodyCapacity,
                                                      uint32_t storageBytes);
LAIUE_PHYSICS_API void VoxelRigidBroadphaseReset(VoxelRigidBroadphase *broadphase);

// Engine-internal operations, intentionally not part of the module export ABI.
// All bounds must be finite, ordered binary64 values. Proxies carry a small
// conservative fat margin; callers must still perform exact collision tests.
bool RigidBroadphaseValid(const VoxelRigidBroadphase *broadphase, uint32_t bodyCount);
bool RigidBroadphaseSetProxy(VoxelRigidBroadphase *broadphase, uint32_t slot,
                              const double minimum[3], const double maximum[3]);
void RigidBroadphaseRemoveProxy(VoxelRigidBroadphase *broadphase, uint32_t slot);
// Output order follows tree topology, NOT stableId. The solver must canonicalize
// results before any order-sensitive processing. A bounded stack and a
// parent-link fallback need no allocation or recursive calls.
// false on insufficient output space; outCount reports only the written prefix.
bool RigidBroadphaseQuery(VoxelRigidBroadphase *broadphase, const double minimum[3],
                           const double maximum[3], uint32_t *outSlots,
                           uint32_t capacity, uint32_t *outCount);
