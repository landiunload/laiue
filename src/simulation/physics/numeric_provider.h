#pragma once

#include "numeric/numeric_service.h"

#include <stdbool.h>
#include <stdint.h>

LAIUE_PHYSICS_API void PhysicsSetNumericService(const LaiueNumericServiceV1 *service);
LAIUE_PHYSICS_API const LaiueNumericServiceV1 *PhysicsGetNumericService(void);

/* Instance-bound guard for the legacy process-default bridge. */
bool PhysicsTryAcquireNumericService(const void *owner,
                                     const LaiueNumericServiceV1 *service);
void PhysicsReleaseNumericService(const void *owner);

void PhysicsNumericInit(InfiniteCoord *value);
void PhysicsNumericDestroy(InfiniteCoord *value);
bool PhysicsNumericTryCopyAddInt64(InfiniteCoord *out, const InfiniteCoord *source,
                                   int64_t addend);
bool PhysicsNumericTryAddInt64InPlace(InfiniteCoord *value, int64_t addend);
uint64_t PhysicsNumericDivFloorSmallLow(const InfiniteCoord *value, uint64_t divisor,
                                        uint64_t *outRemainder);
bool PhysicsNumericTryAdd(InfiniteCoord *out, const InfiniteCoord *left,
                          const InfiniteCoord *right);
bool PhysicsNumericTryCopyNegate(InfiniteCoord *out, const InfiniteCoord *source);
bool PhysicsNumericTryCopyShiftLeft(InfiniteCoord *out, const InfiniteCoord *source,
                                    uint32_t bitCount);
bool PhysicsNumericTryCopyShiftRight(InfiniteCoord *out, const InfiniteCoord *source,
                                     uint32_t bitCount);
bool PhysicsNumericTrySetFromDouble(InfiniteCoord *out, double value);
double PhysicsNumericToDoubleSaturating(const InfiniteCoord *value);
