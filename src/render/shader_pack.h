#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

#define SHADER_PACK_NAME_MAX 64
#define LAIUE_SHADER_CONTRACT_VERSION 1u

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
    SHADER_PACK_LOAD_EMPTY,
    SHADER_PACK_LOAD_IO_ERROR,
    SHADER_PACK_LOAD_ACTIVATION_ERROR,
    SHADER_PACK_LOAD_PIPELINE_ERROR,
} ShaderPackLoadStatus;

LAIUE_RENDER_API bool ShaderPackEnumerate(ShaderPackList* outList);
LAIUE_RENDER_API void ShaderPackListRelease(ShaderPackList* list);
LAIUE_RENDER_API bool ShaderPackActivate(const wchar_t* name);

// Кастомный шейдерпак — каталог <exe>/shaders/<name>.lsp.
// pack.lm обязан объявить `LAIUE SHADER 1` и `contract = 1`.
// Каждый предоставленный байткод внутри него является отдельным файлом .ls;
// отсутствующие стадии берутся из встроенного набора.
// Загружает байткод шейдеров из активного шейдерпака.
// Если пак отсутствует или несовместим — возвращает false.
// Вызывающий владеет возвращёнными буферами (освобождать через HeapFree).
LAIUE_RENDER_API bool ShaderPackLoadActiveBytecode(
    void** outChunkVS, uint32_t* outChunkVSLength,
    void** outChunkPS, uint32_t* outChunkPSLength,
    void** outPanoramaVS, uint32_t* outPanoramaVSLength,
    void** outPanoramaPS, uint32_t* outPanoramaPSLength,
    void** outUIVS, uint32_t* outUIVSLength,
    void** outUIPS, uint32_t* outUIPSLength,
    ShaderPackLoadStatus* outStatus);
