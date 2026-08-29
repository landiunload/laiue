#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Engine-owned render content. Application/game package formats deliberately
// live outside this catalog.
typedef enum LaiueContentType
{
    LAIUE_CONTENT_SHADER = 0,         // .ls
    LAIUE_CONTENT_SHADER_PACK,        // .lsp
    LAIUE_CONTENT_TEXTURE,            // .lt
    LAIUE_CONTENT_TEXTURE_PACK,       // .ltp
    LAIUE_CONTENT_TYPE_COUNT,
} LaiueContentType;

typedef enum LaiueContentStorage
{
    LAIUE_CONTENT_STORAGE_FILE = 1u << 0,
    LAIUE_CONTENT_STORAGE_DIRECTORY = 1u << 1,
} LaiueContentStorage;

typedef struct LaiueContentFormat
{
    const wchar_t* displayName;
    const wchar_t* directoryName;
    const wchar_t* extension;
    uint32_t storageMask;
    bool pack;
} LaiueContentFormat;

LAIUE_CONTENT_API const LaiueContentFormat* LaiueContentFormatGet(
    LaiueContentType type);
LAIUE_CONTENT_API bool LaiueContentTypeIsPack(LaiueContentType type);
LAIUE_CONTENT_API bool LaiueContentNameMatches(
    LaiueContentType type, const wchar_t* fileName);

// Безопасное имя одной сущности: разрешён Unicode, но запрещены управляющие
// символы, абсолютные пути, разделители каталогов и специальные имена . / ...
LAIUE_CONTENT_API bool LaiueContentNameIsSafe(const wchar_t* name);
