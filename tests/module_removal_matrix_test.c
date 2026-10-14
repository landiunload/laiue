// Удаление любого модуля обязано отключать только те технологии, которым он
// действительно нужен. Тест открывает каждый собранный модуль, по его
// дескриптору вычисляет замыкание обязательных зависимостей и сравнивает его
// с тем, что запускает загрузчик, когда артефакт удалён, повреждён или
// повреждён и при этом явно выбран профилем как поставщик сервиса. Остальные
// модули обязаны запуститься и опубликовать все объявленные сервисы.

#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define MATRIX_PREFIX L"laiue_"
#define MATRIX_SUFFIX L".dll"
#elif defined(__APPLE__)
#define MATRIX_PREFIX L"liblaiue_"
#define MATRIX_SUFFIX L".dylib"
#else
#define MATRIX_PREFIX L"liblaiue_"
#define MATRIX_SUFFIX L".so"
#endif

#define MATRIX_MAX_MODULES 32u
#define MATRIX_MAX_SERVICES 8u

typedef struct MatrixModule
{
    wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY];
    char id[LAIUE_MODULE_MAX_NAME];
    char
        requires[
            MATRIX_MAX_SERVICES][LAIUE_MODULE_MAX_NAME];
    uint32_t requiresCount;
    char provides[MATRIX_MAX_SERVICES][LAIUE_MODULE_MAX_NAME];
    uint32_t providesCount;
} MatrixModule;

static MatrixModule modules[MATRIX_MAX_MODULES];
static uint32_t moduleCount;
static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
static wchar_t missingPath[LAIUE_PLATFORM_PATH_CAPACITY];
static wchar_t corruptPath[LAIUE_PLATFORM_PATH_CAPACITY];
static LaiueModuleLoadReportEntryV1 reportEntries[MATRIX_MAX_MODULES];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Module removal matrix failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ExpectFor(bool condition, const char *message, const MatrixModule *removed,
                      const MatrixModule *subject)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Module removal matrix failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite(" (removed ");
    LaiueTestRuntimeWrite(removed != NULL ? removed->id : "nothing");
    LaiueTestRuntimeWrite(", module ");
    LaiueTestRuntimeWrite(subject != NULL ? subject->id : "-");
    LaiueTestRuntimeWrite(")\n");
    LaiueTestRuntimeExit(1);
}

static bool AppendWide(wchar_t *output, uint32_t *length, const wchar_t *text)
{
    for (uint32_t index = 0u; text[index] != L'\0'; ++index)
    {
        if (*length + 1u >= LAIUE_PLATFORM_PATH_CAPACITY)
            return false;
        output[(*length)++] = text[index];
    }
    output[*length] = L'\0';
    return true;
}

static bool ArtifactPath(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *name,
                         const wchar_t *suffix)
{
    uint32_t length = 0u;
    output[0] = L'\0';
    if (!AppendWide(output, &length, directory))
        return false;
    if (length != 0u && output[length - 1u] != L'/' && output[length - 1u] != L'\\' &&
        !AppendWide(output, &length, L"/"))
        return false;
    return AppendWide(output, &length, MATRIX_PREFIX) && AppendWide(output, &length, name) &&
           AppendWide(output, &length, suffix);
}

static bool CopyName(char output[LAIUE_MODULE_MAX_NAME], const char *name)
{
    if (name == NULL)
        return false;
    uint32_t index = 0u;
    for (; name[index] != '\0'; ++index)
    {
        if (index + 1u >= LAIUE_MODULE_MAX_NAME)
            return false;
        output[index] = name[index];
    }
    output[index] = '\0';
    return true;
}

static bool SameName(const char *left, const char *right)
{
    uint32_t index = 0u;
    while (left[index] != '\0' && left[index] == right[index])
        ++index;
    return left[index] == right[index];
}

// Описание модуля копируется до закрытия библиотеки: строки дескриптора
// живут в её образе.
static bool ProbeModule(const wchar_t *name)
{
    Expect(moduleCount < MATRIX_MAX_MODULES, "module table has room");
    MatrixModule *module = &modules[moduleCount];
    if (!ArtifactPath(module->path, name, MATRIX_SUFFIX) || !PlatformPathExists(module->path))
        return false;
    PlatformDynamicLibrary library = PlatformDynamicLibraryOpen(module->path);
    Expect(library != NULL, "built module artifact opens");
    void *symbol = PlatformDynamicLibrarySymbol(library, LAIUE_MODULE_ENTRY_NAME_V1);
    Expect(symbol != NULL, "built module exports its entry point");
    LaiueModuleGetApiFnV1 getApi = NULL;
    memcpy(&getApi, &symbol, sizeof(getApi));
    const LaiueModuleApiV1 *api = getApi();
    Expect(api != NULL, "module returns its API");
    const LaiueModuleDescriptorV1 *descriptor = &api->descriptor;
    Expect(CopyName(module->id, descriptor->id), "module id fits");
    Expect(descriptor->requiresCount <= MATRIX_MAX_SERVICES &&
               descriptor->providesCount <= MATRIX_MAX_SERVICES,
           "module service lists fit");
    module->requiresCount = descriptor->requiresCount;
    for (uint32_t index = 0u; index < descriptor->requiresCount; ++index)
        Expect(CopyName(module->requires[index], descriptor->requiresServices[index].name),
               "required service name fits");
    module->providesCount = descriptor->providesCount;
    for (uint32_t index = 0u; index < descriptor->providesCount; ++index)
        Expect(CopyName(module->provides[index], descriptor->providesServices[index]),
               "provided service name fits");
    PlatformDynamicLibraryClose(library);
    ++moduleCount;
    return true;
}

static bool Provides(const MatrixModule *module, const char *service)
{
    for (uint32_t index = 0u; index < module->providesCount; ++index)
        if (SameName(module->provides[index], service))
            return true;
    return false;
}

// Ожидаемый набор: неподвижная точка «все обязательные сервисы есть у уже
// запущенных модулей». Необязательные зависимости запуск не блокируют.
static void ExpectedLoaded(uint32_t removed, bool loaded[MATRIX_MAX_MODULES])
{
    for (uint32_t index = 0u; index < moduleCount; ++index)
        loaded[index] = false;
    bool progress = true;
    while (progress)
    {
        progress = false;
        for (uint32_t index = 0u; index < moduleCount; ++index)
        {
            if (index == removed || loaded[index])
                continue;
            bool ready = true;
            for (uint32_t requirement = 0u; ready && requirement < modules[index].requiresCount;
                 ++requirement)
            {
                ready = false;
                for (uint32_t provider = 0u; provider < moduleCount && !ready; ++provider)
                    ready = loaded[provider] &&
                            Provides(&modules[provider], modules[index].requires[requirement]);
            }
            if (ready)
            {
                loaded[index] = true;
                progress = true;
            }
        }
    }
}

typedef enum MatrixVariant
{
    MATRIX_REMOVED,
    MATRIX_CORRUPT,
    MATRIX_CORRUPT_SELECTED,
} MatrixVariant;

static uint32_t RunProfile(LaiueModuleHost *host, uint32_t removed, MatrixVariant variant)
{
    LaiueModuleBinaryV1 binaries[MATRIX_MAX_MODULES];
    for (uint32_t index = 0u; index < moduleCount; ++index)
    {
        const wchar_t *path = modules[index].path;
        if (index == removed)
            path = variant == MATRIX_REMOVED ? missingPath : corruptPath;
        binaries[index] = (LaiueModuleBinaryV1){path, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    }
    const MatrixModule *removedModule = removed < moduleCount ? &modules[removed] : NULL;
    LaiueModuleProviderSelectionV1 selection = {
        .structSize = sizeof(selection),
        .serviceName = removedModule != NULL && removedModule->providesCount != 0u
                           ? removedModule->provides[0]
                           : NULL,
        .moduleId = removedModule != NULL ? removedModule->id : NULL,
    };
    const bool selected = variant == MATRIX_CORRUPT_SELECTED && selection.serviceName != NULL;
    // Адреса локальных массивов присваиваются отдельно: MSVC в /W4 считает
    // их в инициализаторе агрегата расширением языка (C4221).
    LaiueModuleProfileV1 profile = {
        .structSize = sizeof(profile),
        .flags = LAIUE_MODULE_PROFILE_ALLOW_PARTIAL,
        .binaryCount = moduleCount,
        .providerSelectionCount = selected ? 1u : 0u,
    };
    profile.binaries = binaries;
    profile.providerSelections = selected ? &selection : NULL;
    LaiueModuleLoadReportV1 report;
    LaiueModuleLoadReportInitialize(&report, reportEntries, MATRIX_MAX_MODULES);
    LaiueModuleDiagnostic diagnostic;
    const LaiueModuleStatus status =
        LaiueModuleHostLoadProfileV1(host, &profile, &report, &diagnostic);
    bool expected[MATRIX_MAX_MODULES];
    ExpectedLoaded(removed, expected);
    bool complete = removedModule == NULL;
    for (uint32_t index = 0u; index < moduleCount; ++index)
        complete = complete && expected[index];
    // Модуль без поставщика в этой сборке (UI без графики) тоже даёт
    // частичный профиль; полный профиль обязан загрузиться целиком.
    ExpectFor(complete ? status == LAIUE_MODULE_OK : status == LAIUE_MODULE_PARTIAL,
              complete ? "complete profile loads every module"
                       : "profile without a provider is reported as partial",
              removedModule, NULL);
    uint32_t expectedCount = 0u;
    for (uint32_t index = 0u; index < moduleCount; ++index)
    {
        const MatrixModule *module = &modules[index];
        const bool isLoaded = LaiueModuleHostIsLoaded(host, module->id);
        ExpectFor(isLoaded == expected[index],
                  expected[index] ? "independent module is not running"
                                  : "module without a required provider is running",
                  removedModule, module);
        if (!expected[index])
            continue;
        ++expectedCount;
        for (uint32_t service = 0u; service < module->providesCount; ++service)
            ExpectFor(LaiueModuleHostQueryService(host, module->provides[service], 0u, 0u, NULL,
                                                  NULL) != NULL,
                      "running module did not publish a declared service", removedModule, module);
    }
    ExpectFor(LaiueModuleHostLoadedCount(host) == expectedCount &&
                  report.loadedCount == expectedCount,
              "loaded count matches the dependency closure", removedModule, NULL);
    if (removedModule != NULL)
        for (uint32_t service = 0u; service < removedModule->providesCount; ++service)
        {
            bool otherProvider = false;
            for (uint32_t index = 0u; index < moduleCount; ++index)
                otherProvider =
                    otherProvider || (expected[index] &&
                                      Provides(&modules[index], removedModule->provides[service]));
            if (!otherProvider)
                ExpectFor(LaiueModuleHostQueryService(host, removedModule->provides[service], 0u,
                                                      0u, NULL, NULL) == NULL,
                          "service of a removed module is still published", removedModule, NULL);
        }
    LaiueModuleHostUnloadAll(host);
    ExpectFor(LaiueModuleHostLoadedCount(host) == 0u, "unload leaves no module", removedModule,
              NULL);
    return expectedCount;
}

LAIUE_TEST_ENTRY(ModuleRemovalMatrixTestEntryPoint)
{
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory resolves");
    Expect(ArtifactPath(missingPath, L"removal_matrix_absent", MATRIX_SUFFIX), "missing path fits");
    Expect(ArtifactPath(corruptPath, L"removal_matrix_corrupt", MATRIX_SUFFIX),
           "corrupt path fits");
    static const uint8_t garbage[64] = {
        0x4c, 0x41, 0x49, 0x55, 0x45, 0x00, 0xff, 0x13, 0x37, 0x00, 0x01, 0x02,
    };
    Expect(PlatformWriteEntireFile(corruptPath, garbage, sizeof(garbage)),
           "corrupt artifact is written");

    static const wchar_t *const names[] = {
        L"numeric",       L"task",
        L"content",       L"character",
        L"world",         L"voxel",
        L"voxel_raycast", L"physics",
        L"audio",         L"audio_output",
        L"audio_pack",    L"ui",
        L"mesher",        L"scene_math",
        L"scene",         L"graphics_profile",
        L"window",        L"input",
        L"voxel_render",  L"model",
        L"mesh_world",    L"mesh_world_render",
        L"animation",
    };
    for (uint32_t index = 0u; index < sizeof(names) / sizeof(names[0]); ++index)
        (void)ProbeModule(names[index]);
    // Ровно один поставщик графики: отдельный бэкенд, а совместимый
    // агрегат laiue_render — только если отдельных провайдеров нет.
    if (!ProbeModule(L"graphics_d3d12") && !ProbeModule(L"graphics_vulkan"))
        (void)ProbeModule(L"render");
    Expect(moduleCount >= 8u, "core modules are present");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");

    const uint32_t fullCount = RunProfile(host, moduleCount, MATRIX_REMOVED);
    Expect(fullCount >= 8u, "core modules start in the complete profile");
    uint32_t variants = 0u;
    for (uint32_t removed = 0u; removed < moduleCount; ++removed)
    {
        (void)RunProfile(host, removed, MATRIX_REMOVED);
        (void)RunProfile(host, removed, MATRIX_CORRUPT);
        (void)RunProfile(host, removed, MATRIX_CORRUPT_SELECTED);
        variants += 3u;
    }

    LaiueModuleHostDestroy(host);
    Expect(PlatformDeleteFile(corruptPath), "corrupt artifact is removed");
    Expect(variants == moduleCount * 3u, "every module was removed in every variant");
    char digits[4] = {'0', '0', '\0', '\0'};
    digits[0] = (char)('0' + moduleCount / 10u);
    digits[1] = (char)('0' + moduleCount % 10u);
    LaiueTestRuntimeWrite("Module removal matrix passed for ");
    LaiueTestRuntimeWrite(digits);
    LaiueTestRuntimeWrite(" modules\r\n");
    LAIUE_TEST_SUCCESS();
}
