#include "navigation/nav_internal.h"

#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#define LAIUE_NAV_SQRT_SSE2 1
#include <emmintrin.h>
#elif defined(_M_ARM64) || defined(__aarch64__)
#define LAIUE_NAV_SQRT_NEON 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

static bool HeapLess(const NavHeapEntry *first, const NavHeapEntry *second)
{
    if (first->f != second->f)
        return first->f < second->f;
    if (first->g != second->g)
        return first->g > second->g;
    return first->node < second->node;
}

bool NavHeapPush(LaiueNavQuery *query, uint64_t f, uint64_t g, uint32_t node)
{
    if (query->heapCount == query->heapCapacity)
    {
        if (query->heapCapacity > UINT32_MAX / 2u)
            return false;
        const uint32_t capacity = query->heapCapacity == 0u ? 256u : query->heapCapacity * 2u;
        NavHeapEntry *grown =
            PlatformReallocate(query->heap, (size_t)capacity * sizeof(*grown), false);
        if (grown == NULL)
            return false;
        query->heap = grown;
        query->heapCapacity = capacity;
    }
    NavHeapEntry entry = {f, g, node};
    uint32_t index = query->heapCount++;
    while (index != 0u)
    {
        const uint32_t parent = (index - 1u) / 2u;
        if (!HeapLess(&entry, &query->heap[parent]))
            break;
        query->heap[index] = query->heap[parent];
        index = parent;
    }
    query->heap[index] = entry;
    return true;
}

NavHeapEntry NavHeapPop(LaiueNavQuery *query)
{
    const NavHeapEntry top = query->heap[0];
    const NavHeapEntry last = query->heap[--query->heapCount];
    const uint32_t count = query->heapCount;
    uint32_t index = 0u;
    for (;;)
    {
        const uint32_t left = index * 2u + 1u;
        if (left >= count)
            break;
        uint32_t child = left;
        if (left + 1u < count && HeapLess(&query->heap[left + 1u], &query->heap[left]))
            child = left + 1u;
        if (!HeapLess(&query->heap[child], &last))
            break;
        query->heap[index] = query->heap[child];
        index = child;
    }
    if (count != 0u)
        query->heap[index] = last;
    return top;
}

bool NavQueryReset(LaiueNavQuery *query, uint32_t nodeCount)
{
    if (nodeCount > query->capacity)
    {
        uint32_t capacity = query->capacity == 0u ? 1024u : query->capacity;
        while (capacity < nodeCount)
            capacity = capacity > UINT32_MAX / 2u ? nodeCount : capacity * 2u;
        NavNodeState *nodes = PlatformAllocate((size_t)capacity * sizeof(*nodes), true);
        if (nodes == NULL)
            return false;
        PlatformFree(query->nodes);
        query->nodes = nodes;
        query->capacity = capacity;
        query->currentStamp = 0u;
    }
    /* Новое значение отметки делает всё прежнее состояние недействительным
     * без очистки массива; очистка нужна раз в 2^32 поисков. */
    if (++query->currentStamp == 0u)
    {
        memset(query->nodes, 0, (size_t)query->capacity * sizeof(*query->nodes));
        query->currentStamp = 1u;
    }
    query->heapCount = 0u;
    query->expansions = 0u;
    query->foundCost = 0u;
    query->failure = LAIUE_NAV_OK;
    return true;
}

static uint64_t SqrtEstimate(uint64_t value)
{
#if defined(LAIUE_NAV_SQRT_SSE2)
    /* Значение до 2^63 переводится в double без потери порядка величины. */
    const double estimate = _mm_cvtsd_f64(
        _mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd((double)(int64_t)(value >> 1u) * 2.0)));
    return (uint64_t)(int64_t)estimate;
#elif defined(LAIUE_NAV_SQRT_NEON)
    const float64x1_t estimate = vsqrt_f64(vdup_n_f64((double)(int64_t)(value >> 1u) * 2.0));
    return (uint64_t)(int64_t)vget_lane_f64(estimate, 0);
#else
    /* Побитовый метод без плавающей точки. */
    uint64_t result = 0u;
    uint64_t bit = UINT64_C(1) << 62u;
    while (bit > value)
        bit >>= 2u;
    while (bit != 0u)
    {
        if (value >= result + bit)
        {
            value -= result + bit;
            result = (result >> 1u) + bit;
        }
        else
            result >>= 1u;
        bit >>= 2u;
    }
    return result;
#endif
}

uint64_t NavIntegerSqrt(uint64_t value)
{
    /* Оценка и точная целая поправка: результат — ровно floor(sqrt(value))
     * при любом режиме плавающей точки, поэтому детерминизм от компилятора
     * не зависит. */
    uint64_t root = SqrtEstimate(value);
    if (root > UINT64_C(0xFFFFFFFF))
        root = UINT64_C(0xFFFFFFFF);
    while (root * root > value)
        --root;
    while (root < UINT64_C(0xFFFFFFFF) && (root + 1u) * (root + 1u) <= value)
        ++root;
    return root;
}

LaiueNavStatus LaiueNavQueryCreate(LaiueNavQuery **outQuery)
{
    if (outQuery == NULL)
        return LAIUE_NAV_INVALID_ARGUMENT;
    *outQuery = PlatformAllocate(sizeof(LaiueNavQuery), true);
    return *outQuery == NULL ? LAIUE_NAV_OUT_OF_MEMORY : LAIUE_NAV_OK;
}

void LaiueNavQueryDestroy(LaiueNavQuery *query)
{
    if (query == NULL)
        return;
    PlatformFree(query->nodes);
    PlatformFree(query->heap);
    PlatformFree(query);
}
