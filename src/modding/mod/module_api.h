#pragma once

/*
 * Stable ABI for optional engine technology modules and user extensions.
 *
 * This header is intentionally independent of every LAIUE implementation
 * header. A module is loaded at run time and communicates with the host only
 * through these versioned tables; it must not link to another LAIUE DLL.
 */

#include <stddef.h>
#include <stdint.h>

#define LAIUE_MODULE_ABI_VERSION_1 1u
#define LAIUE_MODULE_ABI_VERSION_CURRENT LAIUE_MODULE_ABI_VERSION_1
#define LAIUE_MODULE_ENTRY_NAME_V1 "LaiueModuleGetApiV1"
#define LAIUE_MODULE_MAX_NAME 96u

#if defined(_WIN32)
#define LAIUE_MODULE_CALL __cdecl
#define LAIUE_MODULE_EXPORT __declspec(dllexport)
#else
#define LAIUE_MODULE_CALL
#define LAIUE_MODULE_EXPORT __attribute__((visibility("default")))
#endif

/* These are typedefs rather than C enums so every value crossing the module
 * boundary has an explicitly fixed-width representation on every compiler. */
typedef uint32_t LaiueModuleStatus;
#define LAIUE_MODULE_OK UINT32_C(0)
#define LAIUE_MODULE_INVALID_ARGUMENT UINT32_C(1)
#define LAIUE_MODULE_ABI_MISMATCH UINT32_C(2)
#define LAIUE_MODULE_DESCRIPTOR_INVALID UINT32_C(3)
#define LAIUE_MODULE_DUPLICATE_ID UINT32_C(4)
#define LAIUE_MODULE_DUPLICATE_SERVICE UINT32_C(5)
#define LAIUE_MODULE_DEPENDENCY_MISSING UINT32_C(6)
#define LAIUE_MODULE_DEPENDENCY_CYCLE UINT32_C(7)
#define LAIUE_MODULE_LOAD_FAILED UINT32_C(8)
#define LAIUE_MODULE_ENTRY_MISSING UINT32_C(9)
#define LAIUE_MODULE_OUT_OF_MEMORY UINT32_C(10)
#define LAIUE_MODULE_CAPACITY UINT32_C(11)
#define LAIUE_MODULE_CREATE_FAILED UINT32_C(12)
#define LAIUE_MODULE_START_FAILED UINT32_C(13)
#define LAIUE_MODULE_BUSY UINT32_C(14)
#define LAIUE_MODULE_SERVICE_INVALID UINT32_C(15)
#define LAIUE_MODULE_SERVICE_NOT_FOUND UINT32_C(16)

typedef uint32_t LaiueModuleLogLevel;
#define LAIUE_MODULE_LOG_DEBUG UINT32_C(0)
#define LAIUE_MODULE_LOG_INFO UINT32_C(1)
#define LAIUE_MODULE_LOG_WARNING UINT32_C(2)
#define LAIUE_MODULE_LOG_ERROR UINT32_C(3)

typedef struct LaiueModuleServiceV1
{
    const char *name;
    uint32_t version;
    const void *table;
    uint32_t tableSize;
} LaiueModuleServiceV1;

typedef struct LaiueModuleHostV1 LaiueModuleHostV1;

/* A dependency names a service interface, not a particular DLL. The provider
 * may expose a newer compatible interface; the host accepts it when its
 * published version is at least minimumVersion. */
typedef struct LaiueModuleRequirementV1
{
    const char *name;
    uint32_t minimumVersion;
} LaiueModuleRequirementV1;

typedef void(LAIUE_MODULE_CALL *LaiueModuleLogFn)(void *context, LaiueModuleLogLevel level,
                                                   const char *moduleId, const char *message);
typedef void *(LAIUE_MODULE_CALL *LaiueModuleAllocateFn)(void *context, uint64_t size);
typedef void *(LAIUE_MODULE_CALL *LaiueModuleReallocateFn)(void *context, void *memory,
                                                            uint64_t size);
typedef void(LAIUE_MODULE_CALL *LaiueModuleFreeFn)(void *context, void *memory);
typedef const void *(LAIUE_MODULE_CALL *LaiueModuleQueryServiceFn)(
    void *context, const char *name, uint32_t minimumVersion, uint32_t minimumSize,
    uint32_t *outVersion, uint32_t *outSize);
typedef LaiueModuleStatus(LAIUE_MODULE_CALL *LaiueModulePublishServiceFn)(
    void *context, const LaiueModuleServiceV1 *service);
typedef LaiueModuleStatus(LAIUE_MODULE_CALL *LaiueModuleUnpublishServiceFn)(
    void *context, const char *name);

struct LaiueModuleHostV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    uint32_t engineVersionMajor;
    uint32_t engineVersionMinor;
    uint32_t engineVersionPatch;
    void *context;
    LaiueModuleLogFn log;
    LaiueModuleAllocateFn allocate;
    LaiueModuleReallocateFn reallocate;
    LaiueModuleFreeFn free;
    LaiueModuleQueryServiceFn queryService;
    LaiueModulePublishServiceFn publishService;
    LaiueModuleUnpublishServiceFn unpublishService;
    uintptr_t reserved[8];
};

typedef struct LaiueModuleDescriptorV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    const char *id;
    const char *version;
    const LaiueModuleRequirementV1 *requiresServices;
    uint32_t requiresCount;
    const char *const *providesServices;
    uint32_t providesCount;
    uintptr_t reserved[4];
} LaiueModuleDescriptorV1;

typedef uint32_t(LAIUE_MODULE_CALL *LaiueModuleCreateFn)(const LaiueModuleHostV1 *host,
                                                         void **outContext);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueModuleStartFn)(void *context);
typedef void(LAIUE_MODULE_CALL *LaiueModuleStopFn)(void *context);
typedef void(LAIUE_MODULE_CALL *LaiueModuleDestroyFn)(void *context);

typedef struct LaiueModuleApiV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueModuleDescriptorV1 descriptor;
    LaiueModuleCreateFn create;
    LaiueModuleStartFn start;
    LaiueModuleStopFn stop;
    LaiueModuleDestroyFn destroy;
    uintptr_t reserved[8];
} LaiueModuleApiV1;

typedef const LaiueModuleApiV1 *(LAIUE_MODULE_CALL *LaiueModuleGetApiFnV1)(void);

/* Every optional DLL/SO exports only this function for ABI 1. */
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void);
