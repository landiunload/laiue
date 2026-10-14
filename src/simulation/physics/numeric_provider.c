#include "physics/numeric_provider.h"

#include <stddef.h>

static const LaiueNumericServiceV1 *g_numericService;
static const void *g_numericOwner;

static bool HasField(const LaiueNumericServiceV1 *service, size_t offset,
                     size_t size)
{
    return service != NULL && service->structSize >= offset + size;
}

static const LaiueNumericServiceV1 *ValidService(void)
{
    const LaiueNumericServiceV1 *service = g_numericService;
    if (service == NULL || service->abiVersion != LAIUE_NUMERIC_SERVICE_ABI_VERSION_1 ||
        service->structSize < offsetof(LaiueNumericServiceV1, init) +
                                  sizeof(service->init))
        return NULL;
    return service;
}

void PhysicsSetNumericService(const LaiueNumericServiceV1 *service)
{
    if (service == NULL)
    {
        if (g_numericOwner == NULL)
            g_numericService = NULL;
        return;
    }
    if (service->abiVersion != LAIUE_NUMERIC_SERVICE_ABI_VERSION_1 ||
        service->structSize < offsetof(LaiueNumericServiceV1, init) +
                                  sizeof(service->init))
        return;
    if (g_numericOwner == NULL || g_numericService == service)
        g_numericService = service;
}

bool PhysicsTryAcquireNumericService(const void *owner,
                                     const LaiueNumericServiceV1 *service)
{
    if (owner == NULL || service == NULL ||
        service->abiVersion != LAIUE_NUMERIC_SERVICE_ABI_VERSION_1 ||
        service->structSize < offsetof(LaiueNumericServiceV1, init) +
                                  sizeof(service->init))
        return false;
    if (g_numericOwner != NULL && g_numericOwner != owner)
        return false;
    if (g_numericService != NULL && g_numericService != service)
        return false;
    g_numericOwner = owner;
    g_numericService = service;
    return true;
}

void PhysicsReleaseNumericService(const void *owner)
{
    if (owner != NULL && g_numericOwner == owner)
    {
        g_numericOwner = NULL;
        g_numericService = NULL;
    }
}

const LaiueNumericServiceV1 *PhysicsGetNumericService(void)
{
    return ValidService();
}

void PhysicsNumericInit(InfiniteCoord *value)
{
    const LaiueNumericServiceV1 *service = ValidService();
    if (service != NULL &&
        HasField(service, offsetof(LaiueNumericServiceV1, init), sizeof(service->init)) &&
        service->init != NULL && value != NULL)
        service->init(value);
    else if (value != NULL)
    {
        value->limbs = NULL;
        value->limbCount = 0u;
        value->sign = 0;
    }
}

void PhysicsNumericDestroy(InfiniteCoord *value)
{
    const LaiueNumericServiceV1 *service = ValidService();
    if (service != NULL &&
        HasField(service, offsetof(LaiueNumericServiceV1, destroy), sizeof(service->destroy)) &&
        service->destroy != NULL && value != NULL)
        service->destroy(value);
}

bool PhysicsNumericTryCopyAddInt64(InfiniteCoord *out, const InfiniteCoord *source,
                                   int64_t addend)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, tryCopyAddInt64),
                    sizeof(service->tryCopyAddInt64)) &&
           service->tryCopyAddInt64 != NULL && out != NULL && source != NULL &&
           service->tryCopyAddInt64(out, source, addend) != 0u;
}

bool PhysicsNumericTryAddInt64InPlace(InfiniteCoord *value, int64_t addend)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, tryAddInt64InPlace),
                    sizeof(service->tryAddInt64InPlace)) &&
           service->tryAddInt64InPlace != NULL && value != NULL &&
           service->tryAddInt64InPlace(value, addend) != 0u;
}

uint64_t PhysicsNumericDivFloorSmallLow(const InfiniteCoord *value, uint64_t divisor,
                                        uint64_t *outRemainder)
{
    const LaiueNumericServiceV1 *service = ValidService();
    if (outRemainder != NULL)
        *outRemainder = 0u;
    return service != NULL &&
                   HasField(service, offsetof(LaiueNumericServiceV1, divFloorSmallLow),
                            sizeof(service->divFloorSmallLow)) &&
                   service->divFloorSmallLow != NULL && value != NULL && divisor != 0u
               ? service->divFloorSmallLow(value, divisor, outRemainder)
               : 0u;
}

bool PhysicsNumericTryAdd(InfiniteCoord *out, const InfiniteCoord *left,
                          const InfiniteCoord *right)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, tryAdd),
                    sizeof(service->tryAdd)) &&
           service->tryAdd != NULL && out != NULL && left != NULL && right != NULL &&
           service->tryAdd(out, left, right) != 0u;
}

bool PhysicsNumericTryCopyNegate(InfiniteCoord *out, const InfiniteCoord *source)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, tryCopyNegate),
                    sizeof(service->tryCopyNegate)) &&
           service->tryCopyNegate != NULL && out != NULL && source != NULL &&
           service->tryCopyNegate(out, source) != 0u;
}

bool PhysicsNumericTryCopyShiftLeft(InfiniteCoord *out, const InfiniteCoord *source,
                                    uint32_t bitCount)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, tryCopyShiftLeft),
                    sizeof(service->tryCopyShiftLeft)) &&
           service->tryCopyShiftLeft != NULL && out != NULL && source != NULL &&
           service->tryCopyShiftLeft(out, source, bitCount) != 0u;
}

bool PhysicsNumericTryCopyShiftRight(InfiniteCoord *out, const InfiniteCoord *source,
                                     uint32_t bitCount)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, tryCopyShiftRight),
                    sizeof(service->tryCopyShiftRight)) &&
           service->tryCopyShiftRight != NULL && out != NULL && source != NULL &&
           service->tryCopyShiftRight(out, source, bitCount) != 0u;
}

bool PhysicsNumericTrySetFromDouble(InfiniteCoord *out, double value)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, trySetFromDouble),
                    sizeof(service->trySetFromDouble)) &&
           service->trySetFromDouble != NULL && out != NULL &&
           service->trySetFromDouble(out, value) != 0u;
}

double PhysicsNumericToDoubleSaturating(const InfiniteCoord *value)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
                   HasField(service, offsetof(LaiueNumericServiceV1, toDoubleSaturating),
                            sizeof(service->toDoubleSaturating)) &&
                   service->toDoubleSaturating != NULL && value != NULL
               ? service->toDoubleSaturating(value)
               : 0.0;
}
