/*
 * Standalone native extension ABI for laiue.
 *
 * Native extensions are trusted in-process code. Loading one gives it the
 * same memory, filesystem and process privileges as the host application.
 * Manifest and path validation prevents accidental path traversal; it is not
 * a sandbox and must never be presented as a security boundary.
 *
 * This header intentionally depends only on compiler-provided C headers.
 * Extensions do not link to engine libraries. Engine and application features
 * are exposed as explicitly versioned services queried through LaiueModHostApiV1.
 * Service interfaces are defined by their providers in separate SDK headers.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define LAIUE_MOD_ABI_VERSION_1 1u
#define LAIUE_MOD_ABI_VERSION_CURRENT LAIUE_MOD_ABI_VERSION_1
#define LAIUE_MOD_ENTRY_NAME_V1 "LaiueModLoadV1"

#if defined(__cplusplus)
#define LAIUE_MOD_EXTERN_C extern "C"
#else
#define LAIUE_MOD_EXTERN_C
#endif

#if defined(_WIN32)
#define LAIUE_MOD_CALL __cdecl
#define LAIUE_MOD_EXPORT LAIUE_MOD_EXTERN_C __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define LAIUE_MOD_CALL
#define LAIUE_MOD_EXPORT LAIUE_MOD_EXTERN_C __attribute__((visibility("default")))
#else
#error Unsupported compiler for the laiue native extension ABI
#endif

typedef int32_t LaiueModResult;

#define LAIUE_MOD_RESULT_OK ((LaiueModResult)0)

typedef enum LaiueModLogLevel
{
    LAIUE_MOD_LOG_DEBUG = 0,
    LAIUE_MOD_LOG_INFORMATION = 1,
    LAIUE_MOD_LOG_WARNING = 2,
    LAIUE_MOD_LOG_ERROR = 3,
} LaiueModLogLevel;

/*
 * A service pointer is borrowed from the host. It remains valid until the
 * extension unload callback returns. The host freezes its service registry
 * while any extension is loaded, so successful queries are stable for the
 * complete loaded lifetime of the extension.
 */
typedef struct LaiueModHostApiV1
{
    uint32_t structSize;
    uint32_t abiVersion;

    uint32_t engineVersionMajor;
    uint32_t engineVersionMinor;
    uint32_t engineVersionPatch;
    uint32_t reservedVersion;

    void *hostContext;
    const char *modId;
    const char *modVersion;

    void(LAIUE_MOD_CALL *log)(void *hostContext, LaiueModLogLevel level, const char *messageUtf8);

    /*
     * Returns NULL when the service is absent, too old or smaller than the
     * requested interface prefix. Service names are case-sensitive ASCII
     * identifiers such as "laiue.world" or "example.inventory".
     */
    const void *(LAIUE_MOD_CALL *queryService)(void *hostContext, const char *serviceName,
                                               uint32_t minimumVersion, uint32_t minimumSize,
                                               uint32_t *outVersion, uint32_t *outSize);

    uintptr_t reserved[8];
} LaiueModHostApiV1;

typedef void(LAIUE_MOD_CALL *LaiueModUnloadV1)(void *modContext);

typedef struct LaiueModExportsV1
{
    /* The host initializes these two fields before calling the entry point. */
    uint32_t structSize;
    uint32_t abiVersion;

    void *modContext;
    LaiueModUnloadV1 unload;

    uintptr_t reserved[8];
} LaiueModExportsV1;

/*
 * Required exported symbol for ABI 1:
 *
 * LAIUE_MOD_EXPORT LaiueModResult LAIUE_MOD_CALL
 * LaiueModLoadV1(const LaiueModHostApiV1* host, LaiueModExportsV1* exports);
 *
 * Return LAIUE_MOD_RESULT_OK only after initialization has completed. The host
 * calls exports->unload exactly once for each successful load, before closing
 * the dynamic library. A failed load must clean up before returning because
 * unload is intentionally not called for failed initialization.
 *
 * An extension that starts worker threads must stop and join them from unload;
 * no extension code or borrowed service pointer may be used after it returns.
 */
typedef LaiueModResult(LAIUE_MOD_CALL *LaiueModLoadFunctionV1)(const LaiueModHostApiV1 *host,
                                                               LaiueModExportsV1 *exports);
