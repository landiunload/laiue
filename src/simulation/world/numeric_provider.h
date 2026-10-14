#pragma once

#include "numeric/numeric_service.h"

#include <stdbool.h>
#include <stdint.h>

LAIUE_WORLD_API void WorldSetNumericService(const LaiueNumericServiceV1 *service);
LAIUE_WORLD_API const LaiueNumericServiceV1 *WorldGetNumericService(void);

/* The compatibility entry points below are process-wide by design, but the
 * dynamically loaded world module must not let one host overwrite another.
 * A non-NULL owner token is retained until the matching release. */
bool WorldTryAcquireNumericService(const void *owner,
                                   const LaiueNumericServiceV1 *service);
void WorldReleaseNumericService(const void *owner);

void WorldNumericInit(InfiniteCoord *value);
void WorldNumericDestroy(InfiniteCoord *value);
bool WorldNumericTryCopyAddInt64(InfiniteCoord *out, const InfiniteCoord *source,
                                 int64_t addend);
bool WorldNumericEqualsOffsets(const InfiniteCoord *left, int64_t leftOffset,
                               const InfiniteCoord *right, int64_t rightOffset);
void WorldNumericSwap(InfiniteCoord *a, InfiniteCoord *b);
uint64_t WorldNumericHashOffset(const InfiniteCoord *base, int64_t offset);
void WorldNumericFormatShortOffsetW(const InfiniteCoord *base, int64_t offset,
                                    wchar_t *outText, uint32_t capacity);

/* Instance-bound variants used by World.  The legacy entry points above keep
 * the old process-default ABI for source compatibility. */
void WorldNumericInitWithService(const LaiueNumericServiceV1 *service,
                                 InfiniteCoord *value);
void WorldNumericDestroyWithService(const LaiueNumericServiceV1 *service,
                                    InfiniteCoord *value);
bool WorldNumericTryCopyAddInt64WithService(
    const LaiueNumericServiceV1 *service, InfiniteCoord *out,
    const InfiniteCoord *source, int64_t addend);
bool WorldNumericEqualsOffsetsWithService(
    const LaiueNumericServiceV1 *service, const InfiniteCoord *left,
    int64_t leftOffset, const InfiniteCoord *right, int64_t rightOffset);
void WorldNumericSwapWithService(const LaiueNumericServiceV1 *service,
                                 InfiniteCoord *a, InfiniteCoord *b);
uint64_t WorldNumericHashOffsetWithService(const LaiueNumericServiceV1 *service,
                                           const InfiniteCoord *base,
                                           int64_t offset);
void WorldNumericFormatShortOffsetWWithService(
    const LaiueNumericServiceV1 *service, const InfiniteCoord *base,
    int64_t offset, wchar_t *outText, uint32_t capacity);
