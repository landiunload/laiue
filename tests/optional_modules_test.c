#include "audio/audio_service.h"
#include "audio/audio_pack_service.h"
#include "content/content_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "ui/ui_service.h"

#include <stdbool.h>

#if defined(_WIN32)
#define AUDIO_MODULE_NAME L"laiue_audio.dll"
#define AUDIO_PACK_MODULE_NAME L"laiue_audio_pack.dll"
#define CONTENT_MODULE_NAME L"laiue_content.dll"
#define UI_MODULE_NAME L"laiue_ui.dll"
#elif defined(__APPLE__)
#define AUDIO_MODULE_NAME L"liblaiue_audio.dylib"
#define AUDIO_PACK_MODULE_NAME L"liblaiue_audio_pack.dylib"
#define CONTENT_MODULE_NAME L"liblaiue_content.dylib"
#define UI_MODULE_NAME L"liblaiue_ui.dylib"
#else
#define AUDIO_MODULE_NAME L"liblaiue_audio.so"
#define AUDIO_PACK_MODULE_NAME L"liblaiue_audio_pack.so"
#define CONTENT_MODULE_NAME L"liblaiue_content.so"
#define UI_MODULE_NAME L"liblaiue_ui.so"
#endif

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Join(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
                 const wchar_t *name)
{
    uint32_t index = 0u;
    while (root[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index] = root[index], ++index;
    if (root[index] != L'\0')
        return false;
    if (index != 0u && output[index - 1u] != L'/' && output[index - 1u] != L'\\')
        output[index++] = L'/';
    uint32_t nameIndex = 0u;
    while (name[nameIndex] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index++] = name[nameIndex++];
    if (name[nameIndex] != L'\0')
        return false;
    output[index] = L'\0';
    return true;
}

LAIUE_TEST_ENTRY(OptionalModulesTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t missingPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t audioPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t audioPackPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t contentPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t uiPath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(missingPath, directory, L"laiue_optional_module_absent_9f3c.dll"),
           "missing path fits");
    Expect(Join(audioPath, directory, AUDIO_MODULE_NAME), "audio path fits");
    Expect(Join(audioPackPath, directory, AUDIO_PACK_MODULE_NAME), "audio pack path fits");
    Expect(Join(contentPath, directory, CONTENT_MODULE_NAME), "content path fits");
    Expect(Join(uiPath, directory, UI_MODULE_NAME), "UI path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");

    LaiueModuleBinaryV1 absent = {
        missingPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    Expect(LaiueModuleHostLoad(host, &absent, 1u, &diagnostic) == LAIUE_MODULE_OK,
           "missing optional module is skipped");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "optional absence leaves the host usable");

    LaiueModuleBinaryV1 audio = {audioPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &audio, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueAudioServiceV1 *audioService =
        (const LaiueAudioServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_AUDIO_SERVICE_NAME, LAIUE_AUDIO_SERVICE_ABI_VERSION_1,
            sizeof(LaiueAudioServiceV1), &version, &size);
    Expect(audioService != NULL && version == LAIUE_AUDIO_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*audioService) && audioService->deviceCreate != NULL,
           "audio service is published");
    LaiueModuleHostUnloadAll(host);

    /* A present pack provider must not silently degrade when its mixer
     * dependency is absent: the host rejects only that graph and remains
     * usable for unrelated technologies. */
    LaiueModuleBinaryV1 packWithoutAudio[] = {
        {audioPackPath, 0u, NULL},
        {contentPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, packWithoutAudio,
                               sizeof(packWithoutAudio) / sizeof(packWithoutAudio[0]),
                               &diagnostic) == LAIUE_MODULE_DEPENDENCY_MISSING,
           "audio pack without mixer must report its missing dependency");
    Expect(LaiueModuleHostLoadedCount(host) == 0u,
           "failed audio pack graph must roll back completely");

    /* The pack provider is a real dependency graph: its DLL has no imports
     * from either provider and receives both tables only after the host has
     * started them.  Listing it first also checks that ordering is resolved
     * from the manifest rather than from filesystem order. */
    LaiueModuleBinaryV1 packBinaries[] = {
        {audioPackPath, 0u, NULL},
        {audioPath, 0u, NULL},
        {contentPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, packBinaries,
                               sizeof(packBinaries) / sizeof(packBinaries[0]), &diagnostic) ==
               LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueAudioPackServiceV1 *packService =
        (const LaiueAudioPackServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_AUDIO_PACK_SERVICE_NAME, LAIUE_AUDIO_PACK_SERVICE_ABI_VERSION_1,
            sizeof(LaiueAudioPackServiceV1), &version, &size);
    Expect(packService != NULL && version == LAIUE_AUDIO_PACK_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*packService) && packService->loadMemory != NULL,
           "audio pack service is published after dependencies");
    LaiueModuleHostUnloadAll(host);

    LaiueModuleBinaryV1 ui = {uiPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &ui, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    const LaiueUiServiceV1 *uiService =
        (const LaiueUiServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_UI_SERVICE_NAME, LAIUE_UI_SERVICE_ABI_VERSION_1,
            sizeof(LaiueUiServiceV1), &version, &size);
    Expect(uiService != NULL && version == LAIUE_UI_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*uiService) && uiService->contextCreate != NULL,
           "UI service is published");
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
