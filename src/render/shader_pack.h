#pragma once

#include "api.h"
#include "content/content_catalog.h"

#include <stdbool.h>
#include <stdint.h>

#define SHADER_PACK_NAME_MAX LAIUE_CONTENT_NAME_CAPACITY
#define LAIUE_SHADER_CONTRACT_VERSION 1u
#define LAIUE_SHADER_BYTECODE_MAX_BYTES 0x40000u

// Every graphics pipeline owned by the renderer is represented explicitly.
// Slots describe renderer responsibilities, never game materials or effects.
typedef enum LaiueShaderSlot
{
    LAIUE_SHADER_CHUNK_VERTEX = 0,
    LAIUE_SHADER_CHUNK_PIXEL,
    LAIUE_SHADER_PANORAMA_VERTEX,
    LAIUE_SHADER_PANORAMA_PIXEL,
    LAIUE_SHADER_UI_VERTEX,
    LAIUE_SHADER_UI_PIXEL,
    LAIUE_SHADER_SLOT_COUNT,
} LaiueShaderSlot;

#define LAIUE_SHADER_SLOT_MASK(slot) (1u << (uint32_t)(slot))
#define LAIUE_SHADER_ALL_SLOTS_MASK ((1u << (uint32_t)LAIUE_SHADER_SLOT_COUNT) - 1u)

typedef struct LaiueShaderBytecode
{
    const void *bytes;
    uint32_t sizeBytes;
    uint32_t reserved;
} LaiueShaderBytecode;

// Versioned non-owning shader view.  A set bit in overrideMask selects the
// corresponding bytecode entry; an unset bit selects the renderer's embedded
// fallback.  Initialize through LaiueShaderSetInitialize so future fields can
// be added without making callers depend on uninitialized storage.
typedef struct LaiueShaderSet
{
    uint32_t structSize;
    uint32_t contractVersion;
    uint32_t overrideMask;
    uint32_t reserved;
    LaiueShaderBytecode bytecode[LAIUE_SHADER_SLOT_COUNT];
} LaiueShaderSet;

LAIUE_RENDER_API void LaiueShaderSetInitialize(LaiueShaderSet *shaderSet);
LAIUE_RENDER_API bool LaiueShaderSetSetOverride(LaiueShaderSet *shaderSet, LaiueShaderSlot slot,
                                                const void *bytecode, uint32_t sizeBytes);
LAIUE_RENDER_API bool LaiueShaderSetClearOverride(LaiueShaderSet *shaderSet, LaiueShaderSlot slot);
LAIUE_RENDER_API bool LaiueShaderSetIsValid(const LaiueShaderSet *shaderSet);

typedef struct ShaderPackEntry
{
    wchar_t name[SHADER_PACK_NAME_MAX];
    bool active;
} ShaderPackEntry;

typedef struct ShaderPackList
{
    ShaderPackEntry* entries;
    uint32_t count;
} ShaderPackList;

typedef enum ShaderPackLoadStatus
{
    SHADER_PACK_LOAD_NOT_ATTEMPTED = 0,
    SHADER_PACK_LOAD_OK,
    SHADER_PACK_LOAD_NO_ACTIVE_PACK,
    SHADER_PACK_LOAD_INVALID_MANIFEST,
    SHADER_PACK_LOAD_INVALID_SHADER,
    SHADER_PACK_LOAD_EMPTY,
    SHADER_PACK_LOAD_IO_ERROR,
    SHADER_PACK_LOAD_ACTIVATION_ERROR,
    SHADER_PACK_LOAD_PIPELINE_ERROR,
} ShaderPackLoadStatus;

typedef struct ShaderPackLoadedSet ShaderPackLoadedSet;

// Explicit-catalog API.  A loaded set owns its file buffers and may be shared
// between threads as an immutable object.  The returned view remains valid
// until ShaderPackLoadedSetRelease; RendererReloadShaderSet copies it.
LAIUE_RENDER_API bool ShaderPackEnumerateFrom(LaiueContentCatalog *catalog,
                                              ShaderPackList *outList);
LAIUE_RENDER_API bool ShaderPackActivateIn(LaiueContentCatalog *catalog, const wchar_t *name);
LAIUE_RENDER_API ShaderPackLoadedSet *ShaderPackLoadActiveSet(LaiueContentCatalog *catalog,
                                                              ShaderPackLoadStatus *outStatus);
LAIUE_RENDER_API const LaiueShaderSet *ShaderPackLoadedSetGet(const ShaderPackLoadedSet *loadedSet);
LAIUE_RENDER_API void ShaderPackLoadedSetRelease(ShaderPackLoadedSet *loadedSet);

// Compatibility wrappers use the executable-root default catalog.
LAIUE_RENDER_API bool ShaderPackEnumerate(ShaderPackList* outList);
LAIUE_RENDER_API void ShaderPackListRelease(ShaderPackList* list);
LAIUE_RENDER_API bool ShaderPackActivate(const wchar_t* name);

// Legacy 12-out-parameter adapter. New code should use
// ShaderPackLoadActiveSet. On success each non-NULL buffer is transferred to
// the caller and must be released with ShaderPackBytecodeRelease; all outputs
// must be non-NULL.
LAIUE_RENDER_API bool ShaderPackLoadActiveBytecode(
    void** outChunkVS, uint32_t* outChunkVSLength,
    void** outChunkPS, uint32_t* outChunkPSLength,
    void** outPanoramaVS, uint32_t* outPanoramaVSLength,
    void** outPanoramaPS, uint32_t* outPanoramaPSLength,
    void** outUIVS, uint32_t* outUIVSLength,
    void** outUIPS, uint32_t* outUIPSLength,
    ShaderPackLoadStatus* outStatus);
LAIUE_RENDER_API void ShaderPackBytecodeRelease(void *bytecode);
