// Пак моделей через таблицу `laiue.model`, как его получает приложение:
// OBJ разбирается и кладётся рядом кэшем `.obj.lo`, свежий кэш читается
// вместо исходника, models/formats.txt меняет порядок форматов, вместо
// отсутствующей модели выдаётся заглушка, перечисление видит подпапки и
// не дублирует модель с кэшем. Модуль работает и без каталога.

#include "content/content_service.h"
#include "media/model.h"
#include "mod/module_host.h"
#include "model/model_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define MODEL_MODULE_NAME L"laiue_model.dll"
#define CONTENT_MODULE_NAME L"laiue_content.dll"
#elif defined(__APPLE__)
#define MODEL_MODULE_NAME L"liblaiue_model.dylib"
#define CONTENT_MODULE_NAME L"liblaiue_content.dylib"
#else
#define MODEL_MODULE_NAME L"liblaiue_model.so"
#define CONTENT_MODULE_NAME L"liblaiue_content.so"
#endif

static const char g_treeObj[] = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvt 0 0\nvt 1 1\n"
                                "usemtl bark\nf 1/1 2/2 4/2 3/1\n";
static const char g_stoneObj[] = "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n";
static const char g_rockObj[] = "v 0 0 0\nv 3 0 0\nv 0 3 0\nv 0 0 3\nf 1 2 3\nf 1 2 4\n";

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Model pack check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool Join(wchar_t *destination, const wchar_t *base, const wchar_t *part)
{
    uint32_t length = 0u;
    while (base[length] != 0)
    {
        if (length + 1u >= LAIUE_PLATFORM_PATH_CAPACITY)
            return false;
        destination[length] = base[length];
        ++length;
    }
    if (length != 0u && destination[length - 1u] != L'/' && destination[length - 1u] != L'\\')
        destination[length++] = L'/';
    for (uint32_t index = 0u; part[index] != 0; ++index)
    {
        if (length + 1u >= LAIUE_PLATFORM_PATH_CAPACITY)
            return false;
        destination[length++] = part[index];
    }
    destination[length] = 0;
    return true;
}

static uint32_t Length(const char *text)
{
    uint32_t length = 0u;
    while (text[length] != '\0')
        ++length;
    return length;
}

static bool SameWide(const wchar_t *left, const wchar_t *right)
{
    uint32_t index = 0u;
    while (left[index] != 0 && left[index] == right[index])
        ++index;
    return left[index] == right[index];
}

static bool SameText(const char *left, const char *right)
{
    uint32_t index = 0u;
    while (left[index] != '\0' && left[index] == right[index])
        ++index;
    return left[index] == right[index];
}

typedef struct TestPaths
{
    wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t root[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t models[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t pack[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t sub[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t tree[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t treeCache[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t rock[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t rockSource[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t stone[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t stoneCache[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t active[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t formats[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t modelModule[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t contentModule[LAIUE_PLATFORM_PATH_CAPACITY];
} TestPaths;

static TestPaths g_paths;
static uint8_t g_encoded[65536];
static ModelVertex g_vertices[64];
static uint32_t g_indices[192];
static ModelPart g_parts[4];
static uint64_t g_scratch[2048];

// Готовый `.lo` собирается тем же кодеком, что и движок.
static uint32_t EncodeLo(const char *obj, uint32_t *outBytes)
{
    ModelInfo info;
    Expect(ModelInspect(obj, Length(obj), &info) == MODEL_OK && info.vertexCapacity <= 64u &&
               info.indexCapacity <= 192u && info.partCapacity <= 4u &&
               info.scratchBytes <= sizeof(g_scratch),
           "fixture inspects");
    ModelData model;
    memset(&model, 0, sizeof(model));
    model.vertices = g_vertices;
    model.indices = g_indices;
    model.parts = g_parts;
    Expect(ModelDecode(obj, Length(obj), NULL, &info, &model, g_scratch, sizeof(g_scratch)) ==
               MODEL_OK,
           "fixture decodes");
    Expect(ModelEncode(&model, g_encoded, sizeof(g_encoded), outBytes) == MODEL_OK,
           "fixture encodes");
    return model.vertexCount;
}

static void PreparePaths(void)
{
    TestPaths *paths = &g_paths;
    Expect(PlatformExecutableDirectory(paths->executable, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory");
    Expect(Join(paths->root, paths->executable, L"model_pack_test_v1") &&
               Join(paths->models, paths->root, L"models") &&
               Join(paths->pack, paths->models, L"Base.lop") &&
               Join(paths->sub, paths->pack, L"rocks") &&
               Join(paths->tree, paths->pack, L"tree.obj") &&
               Join(paths->treeCache, paths->pack, L"tree.obj.lo") &&
               Join(paths->rock, paths->pack, L"rock.lo") &&
               Join(paths->rockSource, paths->pack, L"rock.obj") &&
               Join(paths->stone, paths->sub, L"stone.obj") &&
               Join(paths->stoneCache, paths->sub, L"stone.obj.lo") &&
               Join(paths->active, paths->models, L"active.txt") &&
               Join(paths->formats, paths->models, L"formats.txt") &&
               Join(paths->modelModule, paths->executable, MODEL_MODULE_NAME) &&
               Join(paths->contentModule, paths->executable, CONTENT_MODULE_NAME),
           "paths fit");
    // Прерванный прошлый прогон мог оставить кэш: тест начинает с чистого.
    const wchar_t *stale[] = {paths->treeCache, paths->stoneCache, paths->formats,
                              paths->rockSource};
    for (uint32_t index = 0u; index < sizeof(stale) / sizeof(stale[0]); ++index)
        (void)PlatformDeleteFile(stale[index]);
    Expect(PlatformCreateDirectory(paths->root) && PlatformCreateDirectory(paths->models) &&
               PlatformCreateDirectory(paths->pack) && PlatformCreateDirectory(paths->sub),
           "directories");
    Expect(PlatformWriteEntireFile(paths->tree, g_treeObj, Length(g_treeObj)) &&
               PlatformWriteEntireFile(paths->stone, g_stoneObj, Length(g_stoneObj)),
           "OBJ fixtures");
    uint32_t rockBytes = 0u;
    (void)EncodeLo(g_rockObj, &rockBytes);
    Expect(PlatformWriteEntireFile(paths->rock, g_encoded, rockBytes), "LO fixture");
    Expect(PlatformWriteEntireFile(paths->active, "Base.lop", 8u), "active pack");
}

static void CheckWithoutCatalog(void)
{
    // Прямой статический путь без каталога: пак недоступен, файл — да.
    const LaiueModelServiceV1 *service = LaiueModelGetStaticServiceV1();
    LaiueModelSetContentService(NULL);
    uint32_t status = 0u;
    LaiueModelV1 *placeholder = service->loadFrom(NULL, L"tree", &status);
    LaiueModelViewV1 view = {0};
    Expect(placeholder != NULL && status == LAIUE_MODEL_LOAD_NO_CATALOG &&
               service->getView(placeholder, &view) != 0u &&
               (view.flags & LAIUE_MODEL_FLAG_PLACEHOLDER) != 0u && view.indexCount == 36u,
           "without a catalog the pack yields the placeholder");
    service->release(placeholder);
    LaiueModelV1 *file = service->loadFile(g_paths.rock, &status);
    Expect(file != NULL && status == LAIUE_MODEL_LOAD_OK && service->getView(file, &view) != 0u &&
               view.vertexCount == 4u && (view.flags & LAIUE_MODEL_FLAG_PLACEHOLDER) == 0u,
           "explicit files load without a catalog");
    service->release(file);
    Expect(service->loadMemory("not a model", 11u, &status) == NULL &&
               status == LAIUE_MODEL_LOAD_INVALID_MODEL,
           "memory that is not a model is refused");
}

static void CheckPack(const LaiueModelServiceV1 *service, const LaiueContentServiceV1 *content)
{
    LaiueContentCatalog *catalog = content->createCatalog(g_paths.root);
    Expect(catalog != NULL, "catalog");

    LaiueModelListV1 packs;
    Expect(service->enumeratePacks(catalog, &packs) != 0u && packs.count == 1u &&
               SameWide(packs.entries[0].name, L"Base.lop") && packs.entries[0].active != 0u,
           "pack is listed as active");
    service->releaseList(&packs);

    uint32_t status = 0u;
    LaiueModelV1 *tree = service->loadFrom(catalog, L"tree", &status);
    LaiueModelViewV1 view = {0};
    Expect(tree != NULL && status == LAIUE_MODEL_LOAD_OK && service->getView(tree, &view) != 0u,
           "OBJ model loads from the pack");
    Expect(view.vertexCount == 4u && view.indexCount == 6u && view.partCount == 1u &&
               SameText(view.parts[0].material, "bark") && (view.flags & LAIUE_MODEL_FLAG_UV) != 0u,
           "OBJ geometry and material survive");
    // OBJ Y-вверх: верх квадрата (y = 1) стал z = 1.
    Expect(view.boundsMax[2] > 0.99f && view.boundsMax[1] < 0.01f, "OBJ axes are converted");
    service->release(tree);
    PlatformPathInformation cache;
    Expect(PlatformGetPathInformation(g_paths.treeCache, &cache) && cache.exists &&
               cache.size != 0u,
           "parsed OBJ is cached next to the source");

    // Свежий кэш читается вместо исходника: подменённая, но верно
    // подписанная геометрия кэша доказывает, что исходник не разбирался.
    PlatformPathInformation source;
    Expect(PlatformGetPathInformation(g_paths.tree, &source), "source information");
    uint32_t bytes = 0u;
    ModelInfo info;
    Expect(ModelInspect(g_rockObj, Length(g_rockObj), &info) == MODEL_OK, "rock inspects");
    ModelData model;
    memset(&model, 0, sizeof(model));
    model.vertices = g_vertices;
    model.indices = g_indices;
    model.parts = g_parts;
    Expect(ModelDecode(g_rockObj, Length(g_rockObj), NULL, &info, &model, g_scratch,
                       sizeof(g_scratch)) == MODEL_OK,
           "rock decodes");
    model.sourceModifiedTime = source.modifiedTime;
    model.sourceSizeBytes = (uint32_t)source.size;
    Expect(ModelEncode(&model, g_encoded, sizeof(g_encoded), &bytes) == MODEL_OK &&
               PlatformWriteEntireFile(g_paths.treeCache, g_encoded, bytes),
           "signed cache replacement");
    tree = service->loadFrom(catalog, L"tree", &status);
    Expect(tree != NULL && status == LAIUE_MODEL_LOAD_OK && service->getView(tree, &view) != 0u &&
               view.vertexCount == 4u && view.indexCount == 6u && view.boundsMax[0] > 2.9f,
           "fresh cache is used instead of the source");
    service->release(tree);

    // Кэш с чужим отпечатком устарел: исходник разбирается снова.
    model.sourceSizeBytes = (uint32_t)source.size + 1u;
    Expect(ModelEncode(&model, g_encoded, sizeof(g_encoded), &bytes) == MODEL_OK &&
               PlatformWriteEntireFile(g_paths.treeCache, g_encoded, bytes),
           "stale cache replacement");
    tree = service->loadFrom(catalog, L"tree", &status);
    Expect(tree != NULL && status == LAIUE_MODEL_LOAD_OK && service->getView(tree, &view) != 0u &&
               view.boundsMax[0] < 1.01f,
           "stale cache is rebuilt from the source");
    service->release(tree);

    LaiueModelV1 *stone = service->loadFrom(catalog, L"rocks/stone", &status);
    Expect(stone != NULL && status == LAIUE_MODEL_LOAD_OK, "nested model loads");
    service->release(stone);

    // Свой формат последний: при наличии исходника играет исходник, а
    // formats.txt с `lo` первой строкой возвращает готовый файл.
    Expect(PlatformWriteEntireFile(g_paths.rockSource, g_stoneObj, Length(g_stoneObj)),
           "rock source");
    LaiueModelV1 *rock = service->loadFrom(catalog, L"rock", &status);
    Expect(rock != NULL && service->getView(rock, &view) != 0u && view.vertexCount == 3u,
           "source wins over the engine format by default");
    service->release(rock);
    Expect(PlatformWriteEntireFile(g_paths.formats, "lo\n", 3u), "formats order");
    rock = service->loadFrom(catalog, L"rock", &status);
    Expect(rock != NULL && service->getView(rock, &view) != 0u && view.vertexCount == 4u,
           "formats.txt puts the engine format first");
    service->release(rock);
    (void)PlatformDeleteFile(g_paths.formats);
    (void)PlatformDeleteFile(g_paths.rockSource);

    LaiueModelV1 *missing = service->loadFrom(catalog, L"no_such_model", &status);
    Expect(missing != NULL && status == LAIUE_MODEL_LOAD_NOT_FOUND &&
               service->getView(missing, &view) != 0u &&
               (view.flags & LAIUE_MODEL_FLAG_PLACEHOLDER) != 0u,
           "missing model yields the placeholder and a status");
    service->release(missing);
    Expect(service->loadFrom(catalog, L"../escape", &status) == NULL &&
               status == LAIUE_MODEL_LOAD_INVALID_MODEL,
           "unsafe names are a program error");

    LaiueModelListV1 models;
    Expect(service->enumerateModels(catalog, &models) != 0u && models.count == 3u,
           "models are listed once each, including subfolders and cached sources");
    bool sawTree = false;
    bool sawRock = false;
    bool sawStone = false;
    for (uint32_t index = 0u; index < models.count; ++index)
    {
        sawTree = sawTree || SameWide(models.entries[index].name, L"tree");
        sawRock = sawRock || SameWide(models.entries[index].name, L"rock");
        sawStone = sawStone || SameWide(models.entries[index].name, L"rocks/stone");
    }
    Expect(sawTree && sawRock && sawStone, "model names drop extensions and keep folders");
    service->releaseList(&models);

    Expect(service->activatePack(catalog, L"") != 0u, "pack deactivates");
    LaiueModelV1 *inactive = service->loadFrom(catalog, L"tree", &status);
    Expect(inactive != NULL && status == LAIUE_MODEL_LOAD_NO_ACTIVE_PACK,
           "no active pack yields the placeholder");
    service->release(inactive);
    content->destroyCatalog(catalog);
}

LAIUE_TEST_ENTRY(ModelPackTestEntryPoint)
{
    PreparePaths();
    CheckWithoutCatalog();

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host");
    const LaiueModuleBinaryV1 binaries[] = {
        {g_paths.modelModule, 0u, NULL},
        {g_paths.contentModule, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, binaries, 2u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueModelServiceV1 *service = (const LaiueModelServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_MODEL_SERVICE_NAME, LAIUE_MODEL_SERVICE_ABI_VERSION_1,
        sizeof(LaiueModelServiceV1), NULL, NULL);
    const LaiueContentServiceV1 *content =
        (const LaiueContentServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_CONTENT_SERVICE_NAME, LAIUE_CONTENT_SERVICE_ABI_VERSION_1,
            sizeof(LaiueContentServiceV1), NULL, NULL);
    Expect(service != NULL && content != NULL, "services are published");
    CheckPack(service, content);
    LaiueModuleHostUnloadAll(host);

    // Модуль без каталога тоже поднимается: каталог для него необязателен.
    Expect(LaiueModuleHostLoad(host, binaries, 1u, &diagnostic) == LAIUE_MODULE_OK,
           "model module starts without the content provider");
    service = (const LaiueModelServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_MODEL_SERVICE_NAME, LAIUE_MODEL_SERVICE_ABI_VERSION_1,
        sizeof(LaiueModelServiceV1), NULL, NULL);
    uint32_t status = 0u;
    LaiueModelV1 *placeholder = service != NULL ? service->loadFrom(NULL, L"tree", &status) : NULL;
    Expect(placeholder != NULL && status == LAIUE_MODEL_LOAD_NO_CATALOG,
           "without content the module still answers with the placeholder");
    service->release(placeholder);
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);
    LaiueTestRuntimeWrite("Model pack checks passed\r\n");
    LAIUE_TEST_SUCCESS();
}
