// ROUND2 24-shaders: краевые случаи валидации манифеста шейдерпака.
//
// Существующий laiue.render.packs проверяет только маленький корректный
// манифест. Здесь закрываются состояния, от которых зависит новый префиксный
// путь чтения: корректный манифест длиннее префикса (2048 Б) с `contract` в
// хвосте, большой манифест без `contract`, файл больше лимита 4096 Б, неверный
// заголовок, пустой файл и BOM.

#include "content/content_catalog.h"
#include "platform/system.h"
#include "render/shader_pack.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ManifestTestPaths
{
    wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t root[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t shaders[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t pack[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t manifest[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t stage[LAIUE_PLATFORM_PATH_CAPACITY];
} ManifestTestPaths;

static char g_manifest[8192];
static uint32_t g_manifestLength;

static void Expect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("r2_24_shaders manifest test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
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

static void ResetManifest(void)
{
    g_manifestLength = 0U;
}

static void AppendManifest(const char *text)
{
    for (uint32_t index = 0U; text[index] != '\0'; ++index)
    {
        g_manifest[g_manifestLength++] = text[index];
    }
}

static void AppendBom(void)
{
    static const unsigned char bom[3] = {0xefu, 0xbbu, 0xbfu};
    for (uint32_t index = 0U; index < 3U; ++index)
    {
        g_manifest[g_manifestLength++] = (char)bom[index];
    }
}

static void AppendPaddingPastPrefix(void)
{
    static const char padding[] = "pad-pad-pad-pad-pad-pad-pad-pad-pad-pad\n";
    while (g_manifestLength < 2100U)
    {
        AppendManifest(padding);
    }
}

static void PreparePaths(ManifestTestPaths *paths)
{
    Expect(PlatformExecutableDirectory(paths->executable, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory");
    Expect(Join(paths->root, LAIUE_PLATFORM_PATH_CAPACITY, paths->executable,
                L"r2_24_shaders_manifest_test_v1") &&
               Join(paths->shaders, LAIUE_PLATFORM_PATH_CAPACITY, paths->root, L"shaders") &&
               Join(paths->pack, LAIUE_PLATFORM_PATH_CAPACITY, paths->shaders, L"Case.lsp") &&
               Join(paths->manifest, LAIUE_PLATFORM_PATH_CAPACITY, paths->pack, L"pack.lm") &&
               Join(paths->stage, LAIUE_PLATFORM_PATH_CAPACITY, paths->pack, L"chunk_vs.ls"),
           "path construction");
    Expect(PlatformCreateDirectory(paths->root) && PlatformCreateDirectory(paths->shaders) &&
               PlatformCreateDirectory(paths->pack),
           "directory creation");

    static const uint8_t stageBytes[4] = {'D', 'X', 'B', 'C'};
    Expect(PlatformWriteEntireFile(paths->stage, stageBytes, sizeof(stageBytes)), "stage write");
}

static void ExpectStatus(LaiueContentCatalog *catalog, ShaderPackLoadStatus expected,
                         const char *message)
{
    ShaderPackLoadStatus status = SHADER_PACK_LOAD_NOT_ATTEMPTED;
    ShaderPackLoadedSet *loaded = ShaderPackLoadActiveSet(catalog, &status);
    Expect(loaded == NULL && status == expected, message);
    ShaderPackLoadedSetRelease(loaded);
}

static void ExpectOkWithOneSlot(LaiueContentCatalog *catalog, const char *message)
{
    ShaderPackLoadStatus status = SHADER_PACK_LOAD_NOT_ATTEMPTED;
    ShaderPackLoadedSet *loaded = ShaderPackLoadActiveSet(catalog, &status);
    const LaiueShaderSet *set = ShaderPackLoadedSetGet(loaded);
    Expect(loaded != NULL && status == SHADER_PACK_LOAD_OK && set != NULL &&
               set->overrideMask == LAIUE_SHADER_SLOT_MASK(LAIUE_SHADER_CHUNK_VERTEX) &&
               set->bytecode[LAIUE_SHADER_CHUNK_VERTEX].sizeBytes == 4U,
           message);
    ShaderPackLoadedSetRelease(loaded);
}

static void WriteManifest(const ManifestTestPaths *paths)
{
    Expect(PlatformWriteEntireFile(paths->manifest, g_manifest, g_manifestLength),
           "manifest write");
}

LAIUE_TEST_ENTRY(R2ShadersManifestTestEntryPoint)
{
    ManifestTestPaths *paths = PlatformAllocate(sizeof(*paths), true);
    Expect(paths != NULL, "scratch allocation");
    PreparePaths(paths);

    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    Expect(catalog != NULL, "catalog creation");
    Expect(ShaderPackActivateIn(catalog, L"Case.lsp"), "pack activation");

    // 1. Маленький корректный манифест.
    ResetManifest();
    AppendManifest("LAIUE SHADER 1\nname = Case\ncontract = 1\n");
    WriteManifest(paths);
    ExpectOkWithOneSlot(catalog, "small valid manifest");

    // 2. BOM перед заголовком.
    ResetManifest();
    AppendBom();
    AppendManifest("LAIUE SHADER 1\ncontract = 1\n");
    WriteManifest(paths);
    ExpectOkWithOneSlot(catalog, "bom valid manifest");

    // 3. Корректный манифест длиннее префикса: contract в хвосте.
    ResetManifest();
    AppendManifest("LAIUE SHADER 1\n");
    AppendPaddingPastPrefix();
    AppendManifest("contract = 1\n");
    Expect(g_manifestLength > 2048U && g_manifestLength <= 4096U, "large valid size");
    WriteManifest(paths);
    ExpectOkWithOneSlot(catalog, "large valid manifest");

    // 4. Длинный манифест без contract.
    ResetManifest();
    AppendManifest("LAIUE SHADER 1\n");
    AppendPaddingPastPrefix();
    WriteManifest(paths);
    ExpectStatus(catalog, SHADER_PACK_LOAD_INVALID_MANIFEST, "large manifest without contract");

    // 5. Файл больше максимума 4096 Б.
    ResetManifest();
    AppendManifest("LAIUE SHADER 1\ncontract = 1\n");
    while (g_manifestLength <= 4096U)
    {
        AppendManifest("oversized-padding-oversized-padding\n");
    }
    WriteManifest(paths);
    ExpectStatus(catalog, SHADER_PACK_LOAD_INVALID_MANIFEST, "oversized manifest");

    // 6. Неверный заголовок.
    ResetManifest();
    AppendManifest("LAIUE SHADER 2\ncontract = 1\n");
    WriteManifest(paths);
    ExpectStatus(catalog, SHADER_PACK_LOAD_INVALID_MANIFEST, "wrong header");

    // 7. Заголовок есть, contract отсутствует (короткий).
    ResetManifest();
    AppendManifest("LAIUE SHADER 1\nname = Case\n");
    WriteManifest(paths);
    ExpectStatus(catalog, SHADER_PACK_LOAD_INVALID_MANIFEST, "missing contract");

    // 8. Пустой файл.
    ResetManifest();
    WriteManifest(paths);
    ExpectStatus(catalog, SHADER_PACK_LOAD_INVALID_MANIFEST, "empty manifest");

    LaiueContentCatalogDestroy(catalog);
    PlatformFree(paths);
    LAIUE_TEST_SUCCESS();
}
