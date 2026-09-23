#pragma once

#include "numeric/numeric_service.h"

#include <stdbool.h>
#include <stdint.h>

LAIUE_WORLD_API void WorldSetNumericService(const LaiueNumericServiceV1 *service);
LAIUE_WORLD_API const LaiueNumericServiceV1 *WorldGetNumericService(void);

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
