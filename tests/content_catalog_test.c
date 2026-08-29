#include "content/content_catalog.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CatalogTestPaths
{
    wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t root[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t shaders[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t textures[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t alpha[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t alphaLower[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t beta[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t texturePack[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t built[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t copiedRoot[LAIUE_CONTENT_PATH_CAPACITY];
} CatalogTestPaths;

static void CatalogExpect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Content catalog check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool WideEquals(const wchar_t *left, const wchar_t *right)
{
    uint32_t index = 0U;
    while (left[index] != L'\0' && left[index] == right[index])
    {
        ++index;
    }
    return left[index] == right[index];
}

static bool Join(wchar_t *destination, uint32_t capacity, const wchar_t *first,
                 const wchar_t *second)
{
    uint32_t length = 0U;
    const wchar_t *parts[2] = {first, second};
    destination[0] = L'\0';
    for (uint32_t partIndex = 0U; partIndex < 2U; ++partIndex)
    {
        const wchar_t *part = parts[partIndex];
        if (part == NULL || part[0] == L'\0')
        {
            continue;
        }
        if (length > 0U && destination[length - 1U] != L'/' && destination[length - 1U] != L'\\')
        {
            if (length + 1U >= capacity)
            {
                return false;
            }
            destination[length++] = L'/';
        }
        for (uint32_t index = 0U; part[index] != L'\0'; ++index)
        {
            if (length + 1U >= capacity)
            {
                return false;
            }
            destination[length++] = part[index];
        }
    }
    destination[length] = L'\0';
    return true;
}

static bool ListContains(const LaiueContentList *list, const wchar_t *name, bool active)
{
    for (uint32_t index = 0U; index < list->count; ++index)
    {
        if (WideEquals(list->entries[index].name, name) && list->entries[index].active == active)
        {
            return true;
        }
    }
    return false;
}

#if !defined(_WIN32)
static bool DirectoryContainsExactName(const wchar_t *directory, const wchar_t *name)
{
    PlatformDirectoryIterator *iterator = PlatformAllocate(sizeof(*iterator), false);
    PlatformDirectoryEntry *entry = PlatformAllocate(sizeof(*entry), false);
    if (iterator == NULL || entry == NULL || !PlatformDirectoryOpen(iterator, directory))
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        return false;
    }
    bool found = false;
    while (PlatformDirectoryNext(iterator, entry))
    {
        if (WideEquals(entry->name, name))
        {
            found = true;
            break;
        }
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(entry);
    PlatformFree(iterator);
    return found;
}
#endif

LAIUE_TEST_ENTRY(ContentCatalogTestEntryPoint)
{
    CatalogTestPaths *paths = PlatformAllocate(sizeof(*paths), true);
    CatalogExpect(paths != NULL, "scratch allocation");
    CatalogExpect(PlatformExecutableDirectory(paths->executable, LAIUE_PLATFORM_PATH_CAPACITY),
                  "executable directory");
    CatalogExpect(
        Join(paths->root, LAIUE_PLATFORM_PATH_CAPACITY, paths->executable,
             L"content_catalog_test_v1") &&
            Join(paths->shaders, LAIUE_PLATFORM_PATH_CAPACITY, paths->root, L"shaders") &&
            Join(paths->textures, LAIUE_PLATFORM_PATH_CAPACITY, paths->root, L"textures") &&
            Join(paths->alpha, LAIUE_PLATFORM_PATH_CAPACITY, paths->shaders, L"Alpha.lsp") &&
            Join(paths->alphaLower, LAIUE_PLATFORM_PATH_CAPACITY, paths->shaders, L"alpha.lsp") &&
            Join(paths->beta, LAIUE_PLATFORM_PATH_CAPACITY, paths->shaders, L"Beta.lsp") &&
            Join(paths->texturePack, LAIUE_PLATFORM_PATH_CAPACITY, paths->textures, L"Neutral.ltp"),
        "test path construction");
    CatalogExpect(PlatformCreateDirectory(paths->root) && PlatformCreateDirectory(paths->shaders) &&
                      PlatformCreateDirectory(paths->textures) &&
                      PlatformCreateDirectory(paths->alpha) && PlatformCreateDirectory(paths->beta),
                  "test directory creation");
    const uint8_t placeholder = 0x42U;
    CatalogExpect(PlatformWriteEntireFile(paths->texturePack, &placeholder, sizeof(placeholder)),
                  "test texture pack creation");

    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    CatalogExpect(catalog != NULL, "explicit catalog creation");
    CatalogExpect(
        LaiueContentCatalogGetRoot(catalog, paths->copiedRoot, LAIUE_CONTENT_PATH_CAPACITY) &&
            WideEquals(paths->copiedRoot, paths->root),
        "catalog root round trip");

    LaiueContentList shaders;
    CatalogExpect(LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_SHADER_PACK, &shaders) &&
                      ListContains(&shaders, L"Alpha.lsp", false) &&
                      ListContains(&shaders, L"Beta.lsp", false),
                  "shader pack enumeration");
    LaiueContentListRelease(&shaders);

#if !defined(_WIN32)
    CatalogExpect(PlatformCreateDirectory(paths->alphaLower),
                  "case-collision test directory creation");
    if (DirectoryContainsExactName(paths->shaders, L"alpha.lsp"))
    {
        CatalogExpect(!LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_SHADER_PACK, &shaders),
                      "ASCII case-colliding content packs were not rejected");
        CatalogExpect(PlatformRemoveDirectory(paths->alphaLower),
                      "case-collision test directory cleanup");
    }
#endif

    CatalogExpect(LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_SHADER_PACK, L"Beta.lsp"),
                  "shader pack activation");
    wchar_t active[LAIUE_CONTENT_NAME_CAPACITY];
    CatalogExpect(LaiueContentCatalogGetActivePack(catalog, LAIUE_CONTENT_SHADER_PACK, active,
                                                   LAIUE_CONTENT_NAME_CAPACITY) &&
                      WideEquals(active, L"Beta.lsp"),
                  "active shader pack round trip");
    CatalogExpect(LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_SHADER_PACK, &shaders) &&
                      ListContains(&shaders, L"Beta.lsp", true),
                  "active shader enumeration state");
    LaiueContentListRelease(&shaders);

    CatalogExpect(LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_SHADER_PACK, L"Beta.lsp",
                                               L"pack.lm", paths->built,
                                               LAIUE_CONTENT_PATH_CAPACITY),
                  "pack child path construction");
    CatalogExpect(!LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_SHADER_PACK, L"../Beta.lsp",
                                                NULL, paths->built, LAIUE_CONTENT_PATH_CAPACITY),
                  "path traversal rejection");

    CatalogExpect(
        LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, L"Neutral.ltp"),
        "texture pack activation");
    LaiueContentList textures;
    CatalogExpect(LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_TEXTURE_PACK, &textures) &&
                      ListContains(&textures, L"Neutral.ltp", true),
                  "texture pack enumeration");
    LaiueContentListRelease(&textures);

    CatalogExpect(LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_SHADER_PACK, NULL) &&
                      !LaiueContentCatalogGetActivePack(catalog, LAIUE_CONTENT_SHADER_PACK, active,
                                                        LAIUE_CONTENT_NAME_CAPACITY),
                  "fallback activation");
    LaiueContentCatalogDestroy(catalog);
    PlatformFree(paths);
    LAIUE_TEST_SUCCESS();
}
