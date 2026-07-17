#pragma once

#include "api.h"
#include "content/content_format.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#define LAIUE_CONTENT_NAME_CAPACITY 128u
#define LAIUE_CONTENT_PATH_CAPACITY 32768u

typedef struct LaiueContentEntry
{
    wchar_t name[LAIUE_CONTENT_NAME_CAPACITY];
    bool active;
    bool directory;
} LaiueContentEntry;

typedef struct LaiueContentList
{
    LaiueContentEntry* entries;
    uint32_t count;
} LaiueContentList;

// Перечисляет только один точный формат: пакет одного типа никогда не
// смешивается с пакетами другой категории.
LAIUE_CONTENT_API bool LaiueContentEnumerate(
    LaiueContentType type, LaiueContentList* outList);
LAIUE_CONTENT_API void LaiueContentListRelease(LaiueContentList* list);

// Активный пак хранится в <каталог>/active.txt в UTF-8. Одиночные форматы
// активировать нельзя. name == NULL или пустая строка очищают выбор.
LAIUE_CONTENT_API bool LaiueContentSetActivePack(
    LaiueContentType type, const wchar_t* name);
LAIUE_CONTENT_API bool LaiueContentGetActivePack(
    LaiueContentType type, wchar_t* destination, uint32_t capacity);

// Строит путь внутри каталога типа. childName предназначен для содержимого
// каталогов-паков (например, MyShaders.lsp/chunk_vs.ls) и может быть NULL.
LAIUE_CONTENT_API bool LaiueContentBuildPath(LaiueContentType type,
    const wchar_t* name, const wchar_t* childName,
    wchar_t* destination, uint32_t capacity);
