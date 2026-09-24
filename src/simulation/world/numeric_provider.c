#include "world/numeric_provider.h"

#include <stddef.h>

static const LaiueNumericServiceV1 *g_numericService;

static bool HasField(const LaiueNumericServiceV1 *service, size_t offset,
                     size_t size)
{
    return service != NULL && service->structSize >= offset + size;
}

static const LaiueNumericServiceV1 *ValidateService(
    const LaiueNumericServiceV1 *service)
{
    if (service == NULL || service->abiVersion != LAIUE_NUMERIC_SERVICE_ABI_VERSION_1 ||
        service->structSize < offsetof(LaiueNumericServiceV1, init) +
                                  sizeof(service->init))
        return NULL;
    return service;
}

static const LaiueNumericServiceV1 *ValidService(void)
{
    return ValidateService(g_numericService);
}

void WorldSetNumericService(const LaiueNumericServiceV1 *service)
{
    if (service == NULL || service->abiVersion != LAIUE_NUMERIC_SERVICE_ABI_VERSION_1 ||
        service->structSize < offsetof(LaiueNumericServiceV1, init) +
                                  sizeof(service->init))
        g_numericService = NULL;
    else
        g_numericService = service;
}

const LaiueNumericServiceV1 *WorldGetNumericService(void)
{
    return ValidService();
}

void WorldNumericInit(InfiniteCoord *value)
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

void WorldNumericDestroy(InfiniteCoord *value)
{
    const LaiueNumericServiceV1 *service = ValidService();
    if (service != NULL &&
        HasField(service, offsetof(LaiueNumericServiceV1, destroy), sizeof(service->destroy)) &&
        service->destroy != NULL && value != NULL)
        service->destroy(value);
}

bool WorldNumericTryCopyAddInt64(InfiniteCoord *out, const InfiniteCoord *source,
                                 int64_t addend)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, tryCopyAddInt64),
                    sizeof(service->tryCopyAddInt64)) &&
           service->tryCopyAddInt64 != NULL && out != NULL && source != NULL &&
           service->tryCopyAddInt64(out, source, addend) != 0u;
}

bool WorldNumericEqualsOffsets(const InfiniteCoord *left, int64_t leftOffset,
                               const InfiniteCoord *right, int64_t rightOffset)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, equalsOffsets),
                    sizeof(service->equalsOffsets)) &&
           service->equalsOffsets != NULL && left != NULL && right != NULL &&
           service->equalsOffsets(left, leftOffset, right, rightOffset) != 0u;
}

void WorldNumericSwap(InfiniteCoord *a, InfiniteCoord *b)
{
    const LaiueNumericServiceV1 *service = ValidService();
    if (service != NULL &&
        HasField(service, offsetof(LaiueNumericServiceV1, swap), sizeof(service->swap)) &&
        service->swap != NULL && a != NULL && b != NULL)
        service->swap(a, b);
}

uint64_t WorldNumericHashOffset(const InfiniteCoord *base, int64_t offset)
{
    const LaiueNumericServiceV1 *service = ValidService();
    return service != NULL &&
                   HasField(service, offsetof(LaiueNumericServiceV1, hashOffset),
                            sizeof(service->hashOffset)) &&
                   service->hashOffset != NULL && base != NULL
               ? service->hashOffset(base, offset)
               : 0u;
}

void WorldNumericFormatShortOffsetW(const InfiniteCoord *base, int64_t offset,
                                    wchar_t *outText, uint32_t capacity)
{
    const LaiueNumericServiceV1 *service = ValidService();
    if (service != NULL &&
        HasField(service, offsetof(LaiueNumericServiceV1, formatShortOffsetW),
                 sizeof(service->formatShortOffsetW)) &&
        service->formatShortOffsetW != NULL && base != NULL && outText != NULL &&
        capacity != 0u)
        service->formatShortOffsetW(base, offset, outText, capacity);
    else if (outText != NULL && capacity != 0u)
        outText[0] = L'\0';
}

void WorldNumericInitWithService(const LaiueNumericServiceV1 *service,
                                 InfiniteCoord *value)
{
    service = ValidateService(service);
    if (service != NULL && HasField(service, offsetof(LaiueNumericServiceV1, init),
                                    sizeof(service->init)) && service->init != NULL &&
        value != NULL)
        service->init(value);
    else if (value != NULL)
    {
        value->limbs = NULL;
        value->limbCount = 0u;
        value->sign = 0;
    }
}

void WorldNumericDestroyWithService(const LaiueNumericServiceV1 *service,
                                    InfiniteCoord *value)
{
    service = ValidateService(service);
    if (service != NULL && HasField(service, offsetof(LaiueNumericServiceV1, destroy),
                                    sizeof(service->destroy)) && service->destroy != NULL &&
        value != NULL)
        service->destroy(value);
}

bool WorldNumericTryCopyAddInt64WithService(
    const LaiueNumericServiceV1 *service, InfiniteCoord *out,
    const InfiniteCoord *source, int64_t addend)
{
    service = ValidateService(service);
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, tryCopyAddInt64),
                    sizeof(service->tryCopyAddInt64)) &&
           service->tryCopyAddInt64 != NULL && out != NULL && source != NULL &&
           service->tryCopyAddInt64(out, source, addend) != 0u;
}

bool WorldNumericEqualsOffsetsWithService(
    const LaiueNumericServiceV1 *service, const InfiniteCoord *left,
    int64_t leftOffset, const InfiniteCoord *right, int64_t rightOffset)
{
    service = ValidateService(service);
    return service != NULL &&
           HasField(service, offsetof(LaiueNumericServiceV1, equalsOffsets),
                    sizeof(service->equalsOffsets)) &&
           service->equalsOffsets != NULL && left != NULL && right != NULL &&
           service->equalsOffsets(left, leftOffset, right, rightOffset) != 0u;
}

void WorldNumericSwapWithService(const LaiueNumericServiceV1 *service,
                                 InfiniteCoord *a, InfiniteCoord *b)
{
    service = ValidateService(service);
    if (service != NULL && HasField(service, offsetof(LaiueNumericServiceV1, swap),
                                    sizeof(service->swap)) && service->swap != NULL &&
        a != NULL && b != NULL)
        service->swap(a, b);
}

uint64_t WorldNumericHashOffsetWithService(const LaiueNumericServiceV1 *service,
                                           const InfiniteCoord *base,
                                           int64_t offset)
{
    service = ValidateService(service);
    return service != NULL &&
                   HasField(service, offsetof(LaiueNumericServiceV1, hashOffset),
                            sizeof(service->hashOffset)) &&
                   service->hashOffset != NULL && base != NULL
               ? service->hashOffset(base, offset)
               : 0u;
}

void WorldNumericFormatShortOffsetWWithService(
    const LaiueNumericServiceV1 *service, const InfiniteCoord *base,
    int64_t offset, wchar_t *outText, uint32_t capacity)
{
    service = ValidateService(service);
    if (service != NULL &&
        HasField(service, offsetof(LaiueNumericServiceV1, formatShortOffsetW),
                 sizeof(service->formatShortOffsetW)) &&
        service->formatShortOffsetW != NULL && base != NULL && outText != NULL &&
        capacity != 0u)
        service->formatShortOffsetW(base, offset, outText, capacity);
    else if (outText != NULL && capacity != 0u)
        outText[0] = L'\0';
}
