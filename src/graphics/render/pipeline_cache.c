#include "render/pipeline_cache.h"

#include "platform/system.h"

#include <string.h>

#define CACHE_HEADER_SIZE 96u
#define CACHE_VERSION 1u

/* Каталог задаётся до создания рендереров, а читается при их создании и
 * записи кеша, возможно с других потоков; короткая спин-блокировка
 * защищает копию строки. */
static wchar_t cacheDirectory[LAIUE_PLATFORM_PATH_CAPACITY];
static volatile uint32_t cacheLock;

static void Lock(void)
{
    for (;;)
    {
        uint32_t expected = 0u;
        if (PlatformAtomicCompareExchangeU32(&cacheLock, &expected, 1u))
            return;
        PlatformCpuRelax();
    }
}

static void Unlock(void)
{
    PlatformAtomicStoreU32Release(&cacheLock, 0u);
}

bool RendererSetPipelineCacheDirectory(const wchar_t *directory)
{
    uint32_t length = 0u;
    if (directory != NULL)
    {
        while (directory[length] != L'\0')
            if (++length >= LAIUE_PLATFORM_PATH_CAPACITY - 64u)
                return false;
    }
    Lock();
    for (uint32_t index = 0u; index < length; ++index)
        cacheDirectory[index] = directory[index];
    cacheDirectory[length] = L'\0';
    Unlock();
    return true;
}

bool RendererPipelineCacheEnabled(void)
{
    Lock();
    const bool enabled = cacheDirectory[0] != L'\0';
    Unlock();
    return enabled;
}

static void AppendHex(wchar_t *output, uint32_t *length, uint32_t value, uint32_t digits)
{
    static const wchar_t hex[] = L"0123456789abcdef";
    for (uint32_t index = digits; index != 0u; --index)
        output[(*length)++] = hex[(value >> ((index - 1u) * 4u)) & 15u];
}

/* <каталог>/pipelines-<бэкенд>-<vendor>-<device>.lpc */
static bool BuildPath(const RendererPipelineCacheIdentity *identity,
                      wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY])
{
    Lock();
    uint32_t length = 0u;
    while (cacheDirectory[length] != L'\0')
    {
        output[length] = cacheDirectory[length];
        ++length;
    }
    Unlock();
    if (length == 0u)
        return false;
    if (output[length - 1u] != L'/' && output[length - 1u] != L'\\')
        output[length++] = L'/';
    static const wchar_t prefix[] = L"pipelines-";
    for (uint32_t index = 0u; prefix[index] != L'\0'; ++index)
        output[length++] = prefix[index];
    const wchar_t *backend = identity->backend == RENDERER_BACKEND_D3D12 ? L"d3d12" : L"vulkan";
    for (uint32_t index = 0u; backend[index] != L'\0'; ++index)
        output[length++] = backend[index];
    output[length++] = L'-';
    AppendHex(output, &length, identity->vendorId, 4u);
    output[length++] = L'-';
    AppendHex(output, &length, identity->deviceId, 4u);
    static const wchar_t suffix[] = L".lpc";
    for (uint32_t index = 0u; suffix[index] != L'\0'; ++index)
        output[length++] = suffix[index];
    output[length] = L'\0';
    return true;
}

static void Store32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static void Store64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static void WriteHeader(uint8_t header[CACHE_HEADER_SIZE],
                        const RendererPipelineCacheIdentity *identity, uint64_t payloadSize,
                        const uint8_t digest[32])
{
    memset(header, 0, CACHE_HEADER_SIZE);
    memcpy(header, "LPSOCACH", 8u);
    Store32(header + 8u, CACHE_VERSION);
    Store32(header + 12u, identity->backend);
    Store32(header + 16u, identity->vendorId);
    Store32(header + 20u, identity->deviceId);
    Store64(header + 24u, identity->driverVersion);
    memcpy(header + 32u, identity->uuid, 16u);
    Store64(header + 48u, payloadSize);
    memcpy(header + 56u, digest, 32u);
}

bool RendererPipelineCacheLoad(const RendererPipelineCacheIdentity *identity, uint8_t **outPayload,
                               uint64_t *outSize, RendererPipelineCacheStatusV1 *status)
{
    *outPayload = NULL;
    *outSize = 0u;
    wchar_t *path = PlatformAllocate(LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL)
        return false;
    if (!BuildPath(identity, path))
    {
        PlatformFree(path);
        status->state = RENDERER_PIPELINE_CACHE_DISABLED;
        return false;
    }
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    if (!PlatformPathExists(path))
    {
        PlatformFree(path);
        status->state = RENDERER_PIPELINE_CACHE_EMPTY;
        return true;
    }
    const bool read = PlatformReadEntireFile(
        path, CACHE_HEADER_SIZE + (uint64_t)RENDERER_PIPELINE_CACHE_MAXIMUM_BYTES, &bytes, &size);
    PlatformFree(path);
    status->state = RENDERER_PIPELINE_CACHE_REJECTED;
    if (!read || size < CACHE_HEADER_SIZE)
    {
        PlatformFree(bytes);
        return true;
    }
    uint8_t digest[32];
    uint8_t expected[CACHE_HEADER_SIZE];
    const uint64_t payloadSize = size - CACHE_HEADER_SIZE;
    if (!PlatformSha256(bytes + CACHE_HEADER_SIZE, payloadSize, digest))
    {
        PlatformFree(bytes);
        return true;
    }
    WriteHeader(expected, identity, payloadSize, digest);
    /* Совпадение всего заголовка разом проверяет и устройство, и версию
     * драйвера, и длину, и хеш данных. */
    if (!PlatformConstantTimeEqual(bytes, expected, CACHE_HEADER_SIZE) || payloadSize == 0u)
    {
        PlatformFree(bytes);
        return true;
    }
    uint8_t *payload = PlatformAllocate((size_t)payloadSize, false);
    if (payload == NULL)
    {
        PlatformFree(bytes);
        return true;
    }
    memcpy(payload, bytes + CACHE_HEADER_SIZE, (size_t)payloadSize);
    PlatformFree(bytes);
    *outPayload = payload;
    *outSize = payloadSize;
    status->state = RENDERER_PIPELINE_CACHE_LOADED;
    status->loadedBytes = payloadSize;
    return true;
}

bool RendererPipelineCacheStore(const RendererPipelineCacheIdentity *identity, const void *payload,
                                uint64_t size)
{
    if (payload == NULL || size == 0u || size > RENDERER_PIPELINE_CACHE_MAXIMUM_BYTES)
        return false;
    wchar_t *path = PlatformAllocate(LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    uint8_t *file = PlatformAllocate(CACHE_HEADER_SIZE + (size_t)size, false);
    uint8_t digest[32];
    bool stored = false;
    if (path != NULL && file != NULL && BuildPath(identity, path) &&
        PlatformSha256(payload, size, digest))
    {
        WriteHeader(file, identity, size, digest);
        memcpy(file + CACHE_HEADER_SIZE, payload, (size_t)size);
        stored = PlatformWriteFileAtomic(path, file, CACHE_HEADER_SIZE + size);
    }
    PlatformFree(path);
    PlatformFree(file);
    return stored;
}

uint64_t RendererPipelineCacheHash(uint64_t hash, const void *bytes, size_t size)
{
    const uint8_t *cursor = bytes;
    for (size_t index = 0u; index < size; ++index)
    {
        hash ^= cursor[index];
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}
