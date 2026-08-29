#include "content/content_catalog.h"
#include "platform/system.h"
#include "render/shader_pack.h"
#include "render/texture_pack.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct RenderPackTestPaths
{
    wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t root[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t shaders[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t shaderPack[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifest[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t shaderFile[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t textures[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t texturePack[LAIUE_PLATFORM_PATH_CAPACITY];
} RenderPackTestPaths;

static void PackExpect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Render pack API check failed: ");
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

static void PreparePack(RenderPackTestPaths *paths)
{
    PackExpect(PlatformExecutableDirectory(paths->executable, LAIUE_PLATFORM_PATH_CAPACITY),
               "executable directory");
    PackExpect(
        Join(paths->root, LAIUE_PLATFORM_PATH_CAPACITY, paths->executable,
             L"render_pack_api_test_v1") &&
            Join(paths->shaders, LAIUE_PLATFORM_PATH_CAPACITY, paths->root, L"shaders") &&
            Join(paths->shaderPack, LAIUE_PLATFORM_PATH_CAPACITY, paths->shaders,
                 L"Complete.lsp") &&
            Join(paths->manifest, LAIUE_PLATFORM_PATH_CAPACITY, paths->shaderPack, L"pack.lm") &&
            Join(paths->textures, LAIUE_PLATFORM_PATH_CAPACITY, paths->root, L"textures") &&
            Join(paths->texturePack, LAIUE_PLATFORM_PATH_CAPACITY, paths->textures, L"Neutral.ltp"),
        "pack path construction");
    PackExpect(PlatformCreateDirectory(paths->root) && PlatformCreateDirectory(paths->shaders) &&
                   PlatformCreateDirectory(paths->shaderPack) &&
                   PlatformCreateDirectory(paths->textures),
               "pack directory creation");

    static const char manifest[] = "\xef\xbb\xbf"
                                   "LAIUE SHADER 1\n"
                                   "name = Complete\n"
                                   "contract = 1\n";
    PackExpect(PlatformWriteEntireFile(paths->manifest, manifest, sizeof(manifest) - 1U),
               "shader manifest write");

    static const wchar_t *const fileNames[LAIUE_SHADER_SLOT_COUNT] = {
        L"chunk_vs.ls",    L"chunk_ps.ls", L"panorama_vs.ls",
        L"panorama_ps.ls", L"ui_vs.ls",    L"ui_ps.ls",
    };
    for (uint32_t index = 0U; index < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++index)
    {
        uint8_t bytecode[8] = {
            'D', 'X', 'B', 'C', (uint8_t)index, 0x31U, 0x32U, 0x33U,
        };
        PackExpect(Join(paths->shaderFile, LAIUE_PLATFORM_PATH_CAPACITY, paths->shaderPack,
                        fileNames[index]) &&
                       PlatformWriteEntireFile(paths->shaderFile, bytecode, sizeof(bytecode)),
                   "shader stage write");
    }
    const uint8_t placeholder = 0x42U;
    PackExpect(PlatformWriteEntireFile(paths->texturePack, &placeholder, sizeof(placeholder)),
               "texture pack placeholder write");
}

static void TestShaderSetHelpers(void)
{
    static const uint8_t bytes[4] = {'D', 'X', 'B', 'C'};
    LaiueShaderSet shaderSet;
    LaiueShaderSetInitialize(&shaderSet);
    PackExpect(LaiueShaderSetIsValid(&shaderSet), "initialized fallback set");
    for (uint32_t index = 0U; index < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++index)
    {
        PackExpect(
            LaiueShaderSetSetOverride(&shaderSet, (LaiueShaderSlot)index, bytes, sizeof(bytes)),
            "shader slot override");
    }
    PackExpect(shaderSet.overrideMask == LAIUE_SHADER_ALL_SLOTS_MASK &&
                   LaiueShaderSetIsValid(&shaderSet),
               "complete shader set validation");
    PackExpect(LaiueShaderSetClearOverride(&shaderSet, LAIUE_SHADER_UI_PIXEL) &&
                   LaiueShaderSetIsValid(&shaderSet),
               "shader slot fallback reset");
    shaderSet.reserved = 1U;
    PackExpect(!LaiueShaderSetIsValid(&shaderSet), "non-zero reserved shader field");
}

static void TestPackLoading(LaiueContentCatalog *catalog, RenderPackTestPaths *paths)
{
    PackExpect(ShaderPackActivateIn(catalog, L"Complete.lsp"), "shader pack activation");
    ShaderPackLoadStatus status = SHADER_PACK_LOAD_NOT_ATTEMPTED;
    ShaderPackLoadedSet *loaded = ShaderPackLoadActiveSet(catalog, &status);
    const LaiueShaderSet *shaderSet = ShaderPackLoadedSetGet(loaded);
    PackExpect(loaded != NULL && status == SHADER_PACK_LOAD_OK && shaderSet != NULL &&
                   shaderSet->overrideMask == LAIUE_SHADER_ALL_SLOTS_MASK &&
                   LaiueShaderSetIsValid(shaderSet),
               "complete shader pack load");
    for (uint32_t index = 0U; index < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++index)
    {
        const uint8_t *bytes = shaderSet->bytecode[index].bytes;
        PackExpect(shaderSet->bytecode[index].sizeBytes == 8U && bytes[0] == 'D' &&
                       bytes[1] == 'X' && bytes[4] == (uint8_t)index,
                   "loaded shader stage ownership");
    }
    ShaderPackLoadedSetRelease(loaded);

    ShaderPackList shaders;
    PackExpect(ShaderPackEnumerateFrom(catalog, &shaders), "shader pack enumeration");
    bool foundActive = false;
    for (uint32_t index = 0U; index < shaders.count; ++index)
    {
        foundActive = foundActive || (WideEquals(shaders.entries[index].name, L"Complete.lsp") &&
                                      shaders.entries[index].active);
    }
    PackExpect(foundActive, "active shader pack enumeration state");
    ShaderPackListRelease(&shaders);

    PackExpect(TexturePackActivateIn(catalog, L"Neutral.ltp"), "texture pack activation");
    TexturePackList textures;
    PackExpect(TexturePackEnumerateFrom(catalog, &textures), "texture pack enumeration");
    bool foundTexture = false;
    for (uint32_t index = 0U; index < textures.count; ++index)
    {
        foundTexture = foundTexture || (WideEquals(textures.entries[index].name, L"Neutral.ltp") &&
                                        textures.entries[index].active);
    }
    PackExpect(foundTexture, "active texture pack enumeration state");
    TexturePackListRelease(&textures);

    PackExpect(ShaderPackActivateIn(catalog, NULL), "shader fallback activation");
    loaded = ShaderPackLoadActiveSet(catalog, &status);
    PackExpect(loaded == NULL && status == SHADER_PACK_LOAD_NO_ACTIVE_PACK,
               "missing active shader pack status");

    static const char invalidActive[] = "Missing.lsp\n";
    PackExpect(
        LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_SHADER_PACK, NULL, L"active.txt",
                                     paths->shaderFile, LAIUE_PLATFORM_PATH_CAPACITY) &&
            PlatformWriteEntireFile(paths->shaderFile, invalidActive, sizeof(invalidActive) - 1U),
        "invalid active shader selection write");
    loaded = ShaderPackLoadActiveSet(catalog, &status);
    PackExpect(loaded == NULL && status == SHADER_PACK_LOAD_ACTIVATION_ERROR,
               "invalid active shader selection status");
    PackExpect(ShaderPackActivateIn(catalog, NULL), "invalid shader selection cleanup");
}

LAIUE_TEST_ENTRY(RenderPackApiTestEntryPoint)
{
    RenderPackTestPaths *paths = PlatformAllocate(sizeof(*paths), true);
    PackExpect(paths != NULL, "scratch allocation");
    PreparePack(paths);
    TestShaderSetHelpers();
    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    PackExpect(catalog != NULL, "custom content catalog");
    TestPackLoading(catalog, paths);
    LaiueContentCatalogDestroy(catalog);
    PlatformFree(paths);
    LAIUE_TEST_SUCCESS();
}
