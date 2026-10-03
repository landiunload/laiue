// Многократное создание и разрушение системного вывода не должно копить
// память. ALSA загружается в рантайме, и без освобождения её глобального
// кэша конфигурации перед dlclose каждый цикл терял около 100 КиБ —
// даже когда устройства нет и открытие завершается ошибкой. Поэтому тест
// не требует звуковой карты: он гоняет циклы и сверяет рост RSS процесса.

#include "audio/audio_output_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define WARMUP_CYCLES 8u
#define MEASURED_CYCLES 256u
// До исправления рост составлял около 25 МиБ на 256 циклов; после него —
// нуль с точностью до страниц аллокатора.
#define MAXIMUM_GROWTH_KIB 4096L

// AddressSanitizer и ThreadSanitizer держат освобождённую память у себя, и
// RSS там растёт без всякой утечки. В такой сборке потерю ловит LeakSanitizer при
// выходе процесса, а сравнение RSS выполняется только в обычной.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CYCLE_TEST_RSS_CHECK 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define CYCLE_TEST_RSS_CHECK 0
#endif
#endif
#ifndef CYCLE_TEST_RSS_CHECK
#define CYCLE_TEST_RSS_CHECK 1
#endif

// Valgrind подменяет аллокатор и тоже не возвращает освобождённое сразу;
// утечки он ищет сам, поэтому под ним сравнение RSS не выполняется.
static bool RunningUnderValgrind(void)
{
    const char *preload = getenv("LD_PRELOAD");
    return preload != NULL && strstr(preload, "vgpreload") != NULL;
}

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Audio output cycle check failed: ");
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
    if (root[index] != L'\0')
        return false;
    if (index != 0u && output[index - 1u] != L'/')
        output[index++] = L'/';
    uint32_t nameIndex = 0u;
    while (name[nameIndex] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index++] = name[nameIndex++];
    if (name[nameIndex] != L'\0')
        return false;
    output[index] = L'\0';
    return true;
}

static long ResidentKib(void)
{
    FILE *file = fopen("/proc/self/statm", "r");
    if (file == NULL)
        return -1L;
    long size = 0L;
    long resident = 0L;
    int fields = fscanf(file, "%ld %ld", &size, &resident);
    fclose(file);
    return fields == 2 ? resident * 4L : -1L;
}

static void LAIUE_MODULE_CALL RenderSilence(void *context, float *frames, uint32_t frameCount)
{
    (void)context;
    for (uint32_t sample = 0u; sample < frameCount * 2u; ++sample)
        frames[sample] = 0.0f;
}

static void Cycle(const LaiueAudioOutputServiceV1 *output, uint32_t count, uint32_t *opened)
{
    const LaiueAudioOutputDescription description = {
        .sampleRate = 0u,
        .frameCountHint = 0u,
        .render = RenderSilence,
        .context = NULL,
    };
    for (uint32_t cycle = 0u; cycle < count; ++cycle)
    {
        LaiueAudioOutputBackend *backend = NULL;
        if (output->createWithContext(output->context, &description, &backend) != 0u &&
            backend != NULL)
        {
            output->destroy(backend);
            ++*opened;
        }
    }
}

int main(void)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t outputPath[LAIUE_PLATFORM_PATH_CAPACITY];
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory is available");
    Expect(Join(outputPath, directory, L"liblaiue_audio_output.so"), "output path fits");

    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host creates");
    LaiueModuleBinaryV1 binary = {outputPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    const LaiueAudioOutputServiceV1 *output =
        (const LaiueAudioOutputServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_AUDIO_OUTPUT_SERVICE_NAME, LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1,
            LAIUE_AUDIO_OUTPUT_SERVICE_V1_CONTEXT_SIZE, NULL, NULL);
    Expect(output != NULL && output->createWithContext != NULL && output->destroy != NULL,
           "audio output service is available");

    uint32_t opened = 0u;
    Cycle(output, WARMUP_CYCLES, &opened);
    long before = ResidentKib();
    Expect(before > 0L, "resident size is readable");
    Cycle(output, MEASURED_CYCLES, &opened);
    long after = ResidentKib();
    Expect(after > 0L, "resident size is readable after cycles");

    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);

    printf("audio output cycles: %u opened, resident growth %ld KiB\n", opened, after - before);
    Expect(!CYCLE_TEST_RSS_CHECK || RunningUnderValgrind() || after - before <= MAXIMUM_GROWTH_KIB,
           "repeated system output create/destroy must not accumulate memory");
    LaiueTestRuntimeWrite("Audio output cycle checks passed\n");
    return 0;
}
