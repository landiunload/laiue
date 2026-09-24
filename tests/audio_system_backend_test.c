// Проверяет системный вывод через тот же standalone ABI, который использует
// приложение: output provider и mixer загружаются независимо. Если output
// provider удалить, этот тест не маскирует отсутствие звука offscreen-тестом.

#include "audio/audio_service.h"
#include "audio/audio_output_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SKIP_EXIT_CODE 125
#define OBSERVATION_MILLISECONDS 400u
#define POLL_INTERVAL_MILLISECONDS 20u

#if defined(_WIN32)
#define AUDIO_MODULE_NAME L"laiue_audio.dll"
#define AUDIO_OUTPUT_MODULE_NAME L"laiue_audio_output.dll"
#elif defined(__APPLE__)
#define AUDIO_MODULE_NAME L"liblaiue_audio.dylib"
#define AUDIO_OUTPUT_MODULE_NAME L"liblaiue_audio_output.dylib"
#else
#define AUDIO_MODULE_NAME L"liblaiue_audio.so"
#define AUDIO_OUTPUT_MODULE_NAME L"liblaiue_audio_output.so"
#endif

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("Audio system backend check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}
static bool Join(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
                 const wchar_t *name)
{
    uint32_t index = 0u;
    while (root[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
    {
        output[index] = root[index];
        ++index;
    }
    if (root[index] != L'\0') return false;
    if (index != 0u && output[index - 1u] != L'/' && output[index - 1u] != L'\\')
        output[index++] = L'/';
    uint32_t nameIndex = 0u;
    while (name[nameIndex] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index++] = name[nameIndex++];
    if (name[nameIndex] != L'\0') return false;
    output[index] = L'\0';
    return true;
}

LAIUE_TEST_ENTRY(AudioSystemBackendTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t audioPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t outputPath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(audioPath, directory, AUDIO_MODULE_NAME), "audio path fits");
    Expect(Join(outputPath, directory, AUDIO_OUTPUT_MODULE_NAME), "output path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");

    LaiueModuleBinaryV1 binaries[2] = {
        {outputPath, 0u, NULL},
        {audioPath, 0u, NULL},
    };
    Expect(LaiueModuleHostLoad(host, binaries, 2u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t outputVersion = 0u;
    uint32_t outputSize = 0u;
    const LaiueAudioOutputServiceV1 *output =
        (const LaiueAudioOutputServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_AUDIO_OUTPUT_SERVICE_NAME, LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1,
            LAIUE_AUDIO_OUTPUT_SERVICE_V1_LEGACY_SIZE, &outputVersion, &outputSize);
    Expect(output != NULL && outputVersion >= LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1 &&
               outputSize >= LAIUE_AUDIO_OUTPUT_SERVICE_V1_LEGACY_SIZE &&
               output->create != NULL && output->destroy != NULL &&
               output->createWithContext != NULL && output->context != NULL,
           "audio output publishes an instance-bound creation path");

    uint32_t serviceVersion = 0u;
    uint32_t serviceSize = 0u;
    const LaiueAudioServiceV1 *audio =
        (const LaiueAudioServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_AUDIO_SERVICE_NAME, LAIUE_AUDIO_SERVICE_ABI_VERSION_1,
            sizeof(LaiueAudioServiceV1), &serviceVersion, &serviceSize);
    Expect(audio != NULL && serviceVersion >= LAIUE_AUDIO_SERVICE_ABI_VERSION_1 &&
               serviceSize >= sizeof(*audio) && audio->deviceCreate != NULL &&
               audio->deviceDestroy != NULL && audio->deviceGetStats != NULL,
           "audio service is available");

    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_SYSTEM,
        .sampleRate = 0u,
        .frameCountHint = 0u,
        .masterVolume = 0.0f,
    };
    AudioDevice *device = NULL;
    AudioResult result;
    if (audio->deviceCreateWithContext != NULL && audio->context != NULL)
        result = (AudioResult)audio->deviceCreateWithContext(audio->context, &configuration,
                                                             &device);
    else
        result = (AudioResult)audio->deviceCreate(&configuration, &device);
    if (result != AUDIO_RESULT_OK)
    {
        LaiueTestRuntimeWrite("No system audio output available; skipping\n");
        LaiueModuleHostUnloadAll(host);
        LaiueModuleHostDestroy(host);
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    AudioDeviceStats stats;
    Expect(audio->deviceGetStats(device, &stats) != 0u, "stats must be readable");
    Expect(stats.sampleRate >= 8000u && stats.sampleRate <= 384000u,
           "the device must report a plausible sample rate");
    Expect(stats.channelCount == 2u, "mixing is always stereo");
    Expect(stats.bufferFrameCount > 0u, "the device must report its buffer size");

    uint64_t mixedFrames = 0u;
    for (uint32_t waited = 0u; waited < OBSERVATION_MILLISECONDS;
         waited += POLL_INTERVAL_MILLISECONDS)
    {
        PlatformSleepMilliseconds(POLL_INTERVAL_MILLISECONDS);
        if (audio->deviceGetStats(device, &stats) == 0u) continue;
        mixedFrames = stats.mixedFrames;
        if (mixedFrames > 0u) break;
    }
    Expect(mixedFrames > 0u, "the output thread must pull frames from the mixer");

    float frames[8] = {0.0f};
    Expect(audio->renderFrames(device, frames, 4u) == 0u,
           "direct rendering must be refused for a system device");

    audio->deviceDestroy(device);
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);
    LaiueTestRuntimeWrite("Audio system backend checks passed\n");
    LAIUE_TEST_SUCCESS();
}
