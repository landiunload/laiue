#include "numeric/numeric_service.h"

#include "platform/system.h"

typedef struct LaiueNumericModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueNumericModuleState;

static uint32_t TryCopyAddInt64(
    InfiniteCoord *out, const InfiniteCoord *source, int64_t addend)
{
    return InfiniteCoordTryCopyAddInt64(out, source, addend) ? 1u : 0u;
}

static uint32_t TryAddInt64InPlace(InfiniteCoord *value, int64_t addend)
{
    return InfiniteCoordTryAddInt64InPlace(value, addend) ? 1u : 0u;
}

static uint32_t TryCopySquareAddInt64(
    InfiniteCoord *out, const InfiniteCoord *source, int64_t addend)
{
    return InfiniteCoordTryCopySquareAddInt64(out, source, addend) ? 1u : 0u;
}

static uint32_t TryCopyShiftRight(
    InfiniteCoord *out, const InfiniteCoord *source, uint32_t bitCount)
{
    return InfiniteCoordTryCopyShiftRight(out, source, bitCount) ? 1u : 0u;
}

static uint32_t TrySubtractToInt64(
    const InfiniteCoord *left, const InfiniteCoord *right, int64_t *outDifference)
{
    return InfiniteCoordTrySubtractToInt64(left, right, outDifference) ? 1u : 0u;
}

static uint32_t EqualsOffsets(
    const InfiniteCoord *left, int64_t leftOffset,
    const InfiniteCoord *right, int64_t rightOffset)
{
    return InfiniteCoordEqualsOffsets(left, leftOffset, right, rightOffset) ? 1u : 0u;
}

static uint32_t EqualsOffset(
    const InfiniteCoord *value, const InfiniteCoord *base, int64_t offset)
{
    return InfiniteCoordEqualsOffset(value, base, offset) ? 1u : 0u;
}

static uint32_t TryAdd(
    InfiniteCoord *out, const InfiniteCoord *left, const InfiniteCoord *right)
{
    return InfiniteCoordTryAdd(out, left, right) ? 1u : 0u;
}

static uint32_t TryCopyNegate(InfiniteCoord *out, const InfiniteCoord *source)
{
    return InfiniteCoordTryCopyNegate(out, source) ? 1u : 0u;
}

static uint32_t TryCopyMultiplyInt64(
    InfiniteCoord *out, const InfiniteCoord *source, int64_t factor)
{
    return InfiniteCoordTryCopyMultiplyInt64(out, source, factor) ? 1u : 0u;
}

static uint32_t TryCopyShiftLeft(
    InfiniteCoord *out, const InfiniteCoord *source, uint32_t bitCount)
{
    return InfiniteCoordTryCopyShiftLeft(out, source, bitCount) ? 1u : 0u;
}

static uint32_t TrySetFromDouble(InfiniteCoord *out, double value)
{
    return InfiniteCoordTrySetFromDouble(out, value) ? 1u : 0u;
}

static const LaiueNumericServiceV1 service = {
    .structSize = sizeof(LaiueNumericServiceV1),
    .abiVersion = LAIUE_NUMERIC_SERVICE_ABI_VERSION_1,
    .init = InfiniteCoordInit,
    .destroy = InfiniteCoordDestroy,
    .tryCopyAddInt64 = TryCopyAddInt64,
    .tryAddInt64InPlace = TryAddInt64InPlace,
    .tryCopySquareAddInt64 = TryCopySquareAddInt64,
    .tryCopyShiftRight = TryCopyShiftRight,
    .trySubtractToInt64 = TrySubtractToInt64,
    .equalsOffsets = EqualsOffsets,
    .swap = InfiniteCoordSwap,
    .divFloorSmallLow = InfiniteCoordDivFloorSmallLow,
    .compareAddInt64ToInt64 = InfiniteCoordCompareAddInt64ToInt64,
    .hashOffset = InfiniteCoordHashOffset,
    .equalsOffset = EqualsOffset,
    .subtractFromInt64Clamped = InfiniteCoordSubtractFromInt64Clamped,
    .formatShortOffsetW = InfiniteCoordFormatShortOffsetW,
    .sign = InfiniteCoordSign,
    .compare = InfiniteCoordCompare,
    .tryAdd = TryAdd,
    .tryCopyNegate = TryCopyNegate,
    .tryCopyMultiplyInt64 = TryCopyMultiplyInt64,
    .tryCopyShiftLeft = TryCopyShiftLeft,
    .toDoubleSaturating = InfiniteCoordToDoubleSaturating,
    .trySetFromDouble = TrySetFromDouble,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL ||
        host->free == NULL)
        return 0u;
    *outContext = NULL;
    LaiueNumericModuleState *state =
        (LaiueNumericModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueNumericModuleState *state = (LaiueNumericModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_NUMERIC_SERVICE_NAME,
        .version = LAIUE_NUMERIC_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return state != NULL && state->host != NULL &&
                   state->host->publishService(state->host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    LaiueNumericModuleState *state = (LaiueNumericModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_NUMERIC_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    LaiueNumericModuleState *state = (LaiueNumericModuleState *)context;
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        state->host = NULL;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
    }
}

static const char *const provides[] = {LAIUE_NUMERIC_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.numeric",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueNumericGetStaticModuleApiV1(void)
{
    return &api;
}

const LaiueNumericServiceV1 *LaiueNumericGetStaticServiceV1(void)
{
    return &service;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
