#pragma once

#include "api.h"
#include "content/content_format.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#define LAIUE_CONTENT_NAME_CAPACITY 128u
#define LAIUE_CONTENT_PATH_CAPACITY 32768u

// Immutable, application-owned view of a content tree.  The root can point to
// any directory selected by the embedding application; on desktop, a NULL or
// empty root passed to Create selects the executable directory. Mobile and
// licensed external adapters may reject that default because their executable
// path is not a readable/writable content container. Those application shells
// must stage data-only packs in an OS-provided app directory and pass it here.
//
// A catalog may be shared by any number of threads.  Reads run concurrently;
// active-pack updates are serialized per catalog and are published with an
// atomic file replacement.  Destroy is the only operation that requires the
// caller to have stopped all users.  Use one shared catalog when several
// threads can write active.txt for the same root.
typedef struct LaiueContentCatalog LaiueContentCatalog;

typedef struct LaiueContentEntry
{
    wchar_t name[LAIUE_CONTENT_NAME_CAPACITY];
    bool active;
    bool directory;
} LaiueContentEntry;

typedef struct LaiueContentList
{
    LaiueContentEntry *entries;
    uint32_t count;
} LaiueContentList;

LAIUE_CONTENT_API LaiueContentCatalog *LaiueContentCatalogCreate(const wchar_t *rootDirectory);
LAIUE_CONTENT_API void LaiueContentCatalogDestroy(LaiueContentCatalog *catalog);
LAIUE_CONTENT_API bool LaiueContentCatalogGetRoot(LaiueContentCatalog *catalog,
                                                  wchar_t *destination, uint32_t capacity);

LAIUE_CONTENT_API bool LaiueContentCatalogEnumerate(LaiueContentCatalog *catalog,
                                                    LaiueContentType type,
                                                    LaiueContentList *outList);
LAIUE_CONTENT_API bool LaiueContentCatalogSetActivePack(LaiueContentCatalog *catalog,
                                                        LaiueContentType type, const wchar_t *name);
LAIUE_CONTENT_API bool LaiueContentCatalogGetActivePack(LaiueContentCatalog *catalog,
                                                        LaiueContentType type, wchar_t *destination,
                                                        uint32_t capacity);
LAIUE_CONTENT_API bool LaiueContentCatalogBuildPath(LaiueContentCatalog *catalog,
                                                    LaiueContentType type, const wchar_t *name,
                                                    const wchar_t *childName, wchar_t *destination,
                                                    uint32_t capacity);

// Borrowed process-wide catalog rooted at the executable directory.  It exists
// for compatibility wrappers and must not be destroyed by the application.
LAIUE_CONTENT_API LaiueContentCatalog *LaiueContentCatalogDefault(void);

// Compatibility API.  These calls use LaiueContentCatalogDefault().  New
// library integrations should retain an explicit catalog and use the methods
// above so content location and lifetime are not hidden process-global state.
// Перечисляет только один точный формат: пакет одного типа никогда не
// смешивается с пакетами другой категории.
LAIUE_CONTENT_API bool LaiueContentEnumerate(LaiueContentType type, LaiueContentList *outList);
LAIUE_CONTENT_API void LaiueContentListRelease(LaiueContentList *list);

// Активный пак хранится в <каталог>/active.txt в UTF-8. Одиночные форматы
// активировать нельзя. name == NULL или пустая строка очищают выбор.
LAIUE_CONTENT_API bool LaiueContentSetActivePack(LaiueContentType type, const wchar_t *name);
LAIUE_CONTENT_API bool LaiueContentGetActivePack(LaiueContentType type, wchar_t *destination,
                                                 uint32_t capacity);

// Строит путь внутри каталога типа. childName предназначен для содержимого
// каталогов-паков (например, MyShaders.lsp/chunk_vs.ls) и может быть NULL.
LAIUE_CONTENT_API bool LaiueContentBuildPath(LaiueContentType type, const wchar_t *name,
                                             const wchar_t *childName, wchar_t *destination,
                                             uint32_t capacity);
