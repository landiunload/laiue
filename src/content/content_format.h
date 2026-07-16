#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Единый контракт пользовательского содержимого laiue.
// Нечётные элементы — одиночные сущности, следующий элемент — их пак.
typedef enum LaiueContentType
{
    LAIUE_CONTENT_RESOURCE = 0,       // .lr
    LAIUE_CONTENT_RESOURCE_PACK,      // .lrp
    LAIUE_CONTENT_MOD,                // .lm
    LAIUE_CONTENT_MOD_PACK,           // .lmp
    LAIUE_CONTENT_SHADER,             // .ls
    LAIUE_CONTENT_SHADER_PACK,        // .lsp
    LAIUE_CONTENT_DATA,               // .ld
    LAIUE_CONTENT_DATA_PACK,          // .ldp
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
