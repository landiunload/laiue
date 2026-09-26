// Ручной benchmark загрузчика звукопака: путь AudioClipLoadFrom.
//
// Модуль, который здесь измеряется, — разбор `.la` и выбор ресурса внутри
// пака. Разложение работы по операциям: чтение файла, разбор/декодирование
// сэмплов, копирование в клип. Внешние форматы (WAV) и кэш рядом с
// исходником проверяются отдельной нагрузкой, потому что их путь проходит
// через media_support и кэш.
//
// Как и прочие benchmarks движка, в ALL и в CTest не входит: его запускают
// осознанно. Медиана нечётного числа выборок вместо среднего, контрольная
// сумма в volatile, чтобы компилятор не выбросил работу.
//
// Нагрузки:
//   pcm_la        авторский `.la` PCM16 большого размера, без исходника;
//   wav_cache     WAV рядом с готовым кэшем `.wav.la` (свежий кэш);
//   lookup_miss   отсутствующий звук: считается только поиск без разбора;
//   enumerate     перечисление пака с тысячей звуков.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "audio/audio_pack.h"
#include "content/content_catalog.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define BENCH_SAMPLE_COUNT 9u
#define BENCH_SAMPLE_RATE 48000u

#define BENCH_PCM_FRAMES 4000000u
#define BENCH_WAV_FRAMES 1000000u
#define BENCH_LIST_FILES 128u
#define BENCH_LIST_META_FRAMES 64u

#define BENCH_PCM_ITERATIONS 6u
#define BENCH_WAV_ITERATIONS 8u
#define BENCH_MISS_ITERATIONS 400u
#define BENCH_LIST_ITERATIONS 4u

#define LA_HEADER_SIZE 40u
#define LA_VERSION_2 2u
#define LA_ENCODING_PCM16 0u

static volatile uint64_t benchmarkSink;

// Выбор одной нагрузки через переменную окружения. Пустое значение
// означает «все»: так A/B можно мерить и одним процессом, и по одной
// нагрузке, когда предыдущие аллокации мешали бы чистому замеру.
static char g_selectedWorkload[64];
static bool g_workloadAll;

static bool WantWorkload(const char *name)
{
    if (g_workloadAll) return true;
    const char *left = g_selectedWorkload;
    while (*left != 0 && *name != 0 && *left == *name)
    {
        ++left;
        ++name;
    }
    return *left == 0 && *name == 0;
}

// === Вывод без CRT ===

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u) digits[length++] = '0';
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index) text[index] = digits[length - index - 1u];
    text[length] = '\0';
    WriteText(text);
}

static void WriteMilliseconds(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.0000");
        return;
    }
    uint64_t scaled = (uint64_t)(value * 10000.0 + 0.5);
    WriteUnsigned(scaled / 10000u);
    WriteText(".");
    uint64_t fraction = scaled % 10000u;
    if (fraction < 1000u) WriteText("0");
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && samples[insertion - 1u] > value)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
    return samples[count / 2u];
}

static void Report(const char *name, const double *times, uint32_t operations)
{
    double copy[BENCH_SAMPLE_COUNT];
    for (uint32_t index = 0u; index < BENCH_SAMPLE_COUNT; ++index) copy[index] = times[index];
    WriteText("workload ");
    WriteText(name);
    WriteText(" ops=");
    WriteUnsigned(operations);
    WriteText(" median_ms=");
    WriteMilliseconds(Median(copy, BENCH_SAMPLE_COUNT));
    WriteText(" samples_ms");
    for (uint32_t index = 0u; index < BENCH_SAMPLE_COUNT; ++index)
    {
        WriteText(" ");
        WriteMilliseconds(times[index]);
    }
    WriteText("\n");
}

// === Файлы ===

static bool Join(wchar_t *destination, uint32_t capacity, const wchar_t *base, const wchar_t *part)
{
    uint32_t length = 0u;
    while (base[length] != L'\0')
    {
        if (length + 1u >= capacity) return false;
        destination[length] = base[length];
        ++length;
    }
    if (length > 0u && destination[length - 1u] != L'/' && destination[length - 1u] != L'\\')
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = L'/';
    }
    for (uint32_t index = 0u; part[index] != L'\0'; ++index)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = part[index];
    }
    destination[length] = L'\0';
    return true;
}

static void WriteU16Le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void WriteU32Le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static int16_t SampleAt(uint32_t index)
{
    uint32_t phase = index % 128u;
    int32_t value = phase < 64u ? (int32_t)phase : (int32_t)(128u - phase);
    return (int16_t)((value - 32) * 128);
}

// Заголовок `.la` версии 2. Отпечаток исходника нулевой: файл ни из чего
// не выведен и устареть не может.
static void WriteLaHeader(uint8_t *bytes, uint32_t channels, uint32_t frameCount,
                          uint32_t payloadBytes)
{
    bytes[0] = 'L';
    bytes[1] = 'A';
    bytes[2] = 'S';
    bytes[3] = '1';
    WriteU16Le(bytes + 4, LA_VERSION_2);
    WriteU16Le(bytes + 6, LA_HEADER_SIZE);
    WriteU16Le(bytes + 8, channels);
    WriteU16Le(bytes + 10, LA_ENCODING_PCM16);
    WriteU32Le(bytes + 12, BENCH_SAMPLE_RATE);
    WriteU32Le(bytes + 16, frameCount);
    WriteU32Le(bytes + 20, payloadBytes);
    for (uint32_t index = 24u; index < LA_HEADER_SIZE; ++index) bytes[index] = 0u;
}

static bool WriteLaFile(const wchar_t *path, uint32_t frameCount, uint32_t channels)
{
    uint32_t payloadBytes = frameCount * channels * 2u;
    uint32_t total = LA_HEADER_SIZE + payloadBytes;
    uint8_t *bytes = PlatformAllocate(total, false);
    if (bytes == NULL) return false;
    WriteLaHeader(bytes, channels, frameCount, payloadBytes);
    for (uint32_t frame = 0u; frame < frameCount; ++frame)
    {
        int16_t value = SampleAt(frame);
        for (uint32_t channel = 0u; channel < channels; ++channel)
        {
            WriteU16Le(bytes + LA_HEADER_SIZE + ((size_t)frame * channels + channel) * 2u,
                       (uint32_t)(uint16_t)value);
        }
    }
    bool written = PlatformWriteEntireFile(path, bytes, total);
    PlatformFree(bytes);
    return written;
}

// Канонический WAV PCM16: только `fmt ` и `data`.
static bool WriteWaveFile(const wchar_t *path, uint32_t frameCount, uint32_t channels)
{
    uint32_t dataBytes = frameCount * channels * 2u;
    uint32_t total = 44u + dataBytes;
    uint8_t *bytes = PlatformAllocate(total, false);
    if (bytes == NULL) return false;
    const char *tags = "RIFFWAVEfmt data";
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        bytes[index] = (uint8_t)tags[index];
        bytes[8u + index] = (uint8_t)tags[4u + index];
        bytes[12u + index] = (uint8_t)tags[8u + index];
        bytes[36u + index] = (uint8_t)tags[12u + index];
    }
    WriteU32Le(bytes + 4, 36u + dataBytes);
    WriteU32Le(bytes + 16, 16u);
    WriteU16Le(bytes + 20, 1u);
    WriteU16Le(bytes + 22, (uint32_t)channels);
    WriteU32Le(bytes + 24, BENCH_SAMPLE_RATE);
    WriteU32Le(bytes + 28, BENCH_SAMPLE_RATE * channels * 2u);
    WriteU16Le(bytes + 32, (uint32_t)(channels * 2u));
    WriteU16Le(bytes + 34, 16u);
    WriteU32Le(bytes + 40, dataBytes);
    for (uint32_t frame = 0u; frame < frameCount; ++frame)
    {
        int16_t value = SampleAt(frame);
        for (uint32_t channel = 0u; channel < channels; ++channel)
        {
            WriteU16Le(bytes + 44u + ((size_t)frame * channels + channel) * 2u,
                       (uint32_t)(uint16_t)value);
        }
    }
    bool written = PlatformWriteEntireFile(path, bytes, total);
    PlatformFree(bytes);
    return written;
}

// Имя i-го синтетического звука вида `listNNN.la`.
static void BuildListName(uint32_t index, wchar_t *name)
{
    static const wchar_t prefix[] = L"list";
    uint32_t length = 0u;
    for (uint32_t position = 0u; prefix[position] != 0; ++position) name[length++] = prefix[position];
    char digits[8];
    uint32_t digitCount = 0u;
    uint32_t value = index;
    if (value == 0u) digits[digitCount++] = '0';
    while (value != 0u)
    {
        digits[digitCount++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    for (uint32_t position = 0u; position < digitCount; ++position)
        name[length++] = (wchar_t)digits[digitCount - position - 1u];
    static const wchar_t suffix[] = L".la";
    for (uint32_t position = 0u; suffix[position] != 0; ++position) name[length++] = suffix[position];
    name[length] = 0;
}

static bool WriteActiveFile(const wchar_t *soundsDirectory, wchar_t *path)
{
    static const char text[] = "Bench.lap\n";
    return Join(path, LAIUE_CONTENT_PATH_CAPACITY, soundsDirectory, L"active.txt") &&
           PlatformWriteEntireFile(path, text, sizeof(text) - 1u);
}

// === Измерения ===

typedef struct BenchContext
{
    AudioDevice *device;
    LaiueContentCatalog *catalog;
    float frames[2];
} BenchContext;

static void LoadOnce(const BenchContext *context, const wchar_t *soundName,
                     AudioPackLoadStatus expected)
{
    AudioPackLoadStatus status = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    AudioClip *clip = AudioClipLoadFrom(context->device, context->catalog, soundName, &status);
    if (clip == NULL || status != expected)
    {
        WriteText("audio pack benchmark load failed\n");
        LaiueTestRuntimeExit(1);
    }
    benchmarkSink += (uint64_t)(AudioClipDurationSeconds(clip) * 1000.0);
    AudioClipDestroy(clip);
    AudioDeviceRenderFrames(context->device, (float *)context->frames, 1u);
}

static void MeasureSound(const BenchContext *context, const wchar_t *soundName,
                         AudioPackLoadStatus expected, uint32_t iterations, const char *reportName)
{
    double times[BENCH_SAMPLE_COUNT];
    for (uint32_t sample = 0u; sample < BENCH_SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
        {
            LoadOnce(context, soundName, expected);
        }
        times[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }
    Report(reportName, times, iterations);
}

static void MeasureEnumerate(const BenchContext *context)
{
    double times[BENCH_SAMPLE_COUNT];
    uint32_t lastCount = 0u;
    for (uint32_t sample = 0u; sample < BENCH_SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0u; iteration < BENCH_LIST_ITERATIONS; ++iteration)
        {
            AudioPackList list;
            if (!AudioPackEnumerateSoundsFrom(context->catalog, &list))
            {
                WriteText("audio pack benchmark enumerate failed\n");
                LaiueTestRuntimeExit(1);
            }
            benchmarkSink += list.count;
            lastCount = list.count;
            AudioPackListRelease(&list);
        }
        times[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }
    WriteText("enumerate_count ");
    WriteUnsigned(lastCount);
    WriteText("\n");
    Report("enumerate", times, BENCH_LIST_ITERATIONS);
}

// === Сценарий ===

// Пути живут в куче: пять буферов по тридцать два килослова
// переполнили бы страницу стека, а в сборке без CRT нет __chkstk.
typedef struct BenchPaths
{
    wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t root[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t sounds[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t pack[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t path[LAIUE_CONTENT_PATH_CAPACITY];
} BenchPaths;

LAIUE_TEST_ENTRY(AudioPackBenchmarkEntryPoint)
{
    WriteText("laiue audio pack benchmark\n");

    uint32_t selectedLength = PlatformGetEnvironmentUtf8(
        "LAIUE_AUDIO_PACK_BENCH_WORKLOAD", g_selectedWorkload,
        (uint32_t)sizeof(g_selectedWorkload));
    if (selectedLength >= (uint32_t)sizeof(g_selectedWorkload))
    {
        WriteText("audio pack benchmark workload name is too long\n");
        LaiueTestRuntimeExit(1);
    }
    g_workloadAll = selectedLength == 0u;
    const bool wantPcm = WantWorkload("pcm_la");
    const bool wantWav = WantWorkload("wav_cache");
    const bool wantMiss = WantWorkload("lookup_miss");
    const bool wantList = WantWorkload("enumerate");
    if (!wantPcm && !wantWav && !wantMiss && !wantList)
    {
        WriteText("audio pack benchmark unknown workload\n");
        LaiueTestRuntimeExit(1);
    }

    BenchPaths *paths = PlatformAllocate(sizeof(*paths), false);
    if (paths == NULL)
    {
        WriteText("audio pack benchmark path scratch could not be allocated\n");
        LaiueTestRuntimeExit(1);
    }
    if (!PlatformExecutableDirectory(paths->executable, LAIUE_PLATFORM_PATH_CAPACITY) ||
        !Join(paths->root, LAIUE_CONTENT_PATH_CAPACITY, paths->executable,
              L"audio_pack_benchmark_v2") ||
        !Join(paths->sounds, LAIUE_CONTENT_PATH_CAPACITY, paths->root, L"sounds") ||
        !Join(paths->pack, LAIUE_CONTENT_PATH_CAPACITY, paths->sounds, L"Bench.lap"))
    {
        WriteText("audio pack benchmark path construction failed\n");
        LaiueTestRuntimeExit(1);
    }

    // Прежний прогон мог оставить файлы: список должен считаться от
    // известного состояния.
    for (uint32_t index = 0u; index < BENCH_LIST_FILES; ++index)
    {
        wchar_t name[64];
        BuildListName(index, name);
        if (Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->pack, name))
            PlatformDeleteFile(paths->path);
    }
    if (Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->pack, L"big.la"))
        PlatformDeleteFile(paths->path);
    if (Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->pack, L"cached.wav"))
        PlatformDeleteFile(paths->path);
    if (Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->pack, L"cached.wav.la"))
        PlatformDeleteFile(paths->path);
    if (Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->sounds, L"active.txt"))
        PlatformDeleteFile(paths->path);
    PlatformRemoveDirectory(paths->pack);
    PlatformRemoveDirectory(paths->sounds);
    PlatformRemoveDirectory(paths->root);

    if (!PlatformCreateDirectory(paths->root) || !PlatformCreateDirectory(paths->sounds) ||
        !PlatformCreateDirectory(paths->pack) || !WriteActiveFile(paths->sounds, paths->path))
    {
        WriteText("audio pack benchmark setup failed (directories)\n");
        LaiueTestRuntimeExit(1);
    }

    if (wantPcm && Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->pack, L"big.la") &&
        !WriteLaFile(paths->path, BENCH_PCM_FRAMES, 1u))
    {
        WriteText("audio pack benchmark could not write the pcm sound\n");
        LaiueTestRuntimeExit(1);
    }
    if (wantWav && Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->pack, L"cached.wav") &&
        !WriteWaveFile(paths->path, BENCH_WAV_FRAMES, 1u))
    {
        WriteText("audio pack benchmark could not write the wav source\n");
        LaiueTestRuntimeExit(1);
    }

    for (uint32_t index = 0u; wantList && index < BENCH_LIST_FILES; ++index)
    {
        wchar_t name[64];
        BuildListName(index, name);
        if (!Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->pack, name) ||
            !WriteLaFile(paths->path, BENCH_LIST_META_FRAMES, 1u))
        {
            WriteText("audio pack benchmark could not write the list sound\n");
            LaiueTestRuntimeExit(1);
        }
    }

    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = BENCH_SAMPLE_RATE,
        .frameCountHint = BENCH_LIST_META_FRAMES,
        .masterVolume = 1.0f,
    };
    BenchContext context;
    context.device = NULL;
    context.catalog = NULL;
    context.frames[0] = 0.0f;
    context.frames[1] = 0.0f;
    if (AudioDeviceCreate(&configuration, &context.device) != AUDIO_RESULT_OK)
    {
        WriteText("audio pack benchmark device could not be created\n");
        LaiueTestRuntimeExit(1);
    }
    context.catalog = LaiueContentCatalogCreate(paths->root);
    if (context.catalog == NULL || !AudioPackActivateIn(context.catalog, L"Bench.lap"))
    {
        WriteText("audio pack benchmark catalog could not be activated\n");
        LaiueTestRuntimeExit(1);
    }

    // Прогрев: первый разбор платит за страницы файловой системы. Для
    // WAV он заодно строит кэш `cached.wav.la`, который дальше и
    // измеряется.
    if (wantWav) LoadOnce(&context, L"cached", AUDIO_PACK_LOAD_OK);
    if (wantPcm) LoadOnce(&context, L"big", AUDIO_PACK_LOAD_OK);
    if (wantMiss) LoadOnce(&context, L"absent", AUDIO_PACK_LOAD_SOUND_NOT_FOUND);

    if (wantPcm)
    {
        MeasureSound(&context, L"big", AUDIO_PACK_LOAD_OK, BENCH_PCM_ITERATIONS, "pcm_la");
    }
    if (wantWav)
    {
        MeasureSound(&context, L"cached", AUDIO_PACK_LOAD_OK, BENCH_WAV_ITERATIONS, "wav_cache");
    }
    if (wantMiss)
    {
        MeasureSound(&context, L"absent", AUDIO_PACK_LOAD_SOUND_NOT_FOUND, BENCH_MISS_ITERATIONS,
                     "lookup_miss");
    }
    if (wantList)
    {
        MeasureEnumerate(&context);
    }

    AudioDeviceDestroy(context.device);
    LaiueContentCatalogDestroy(context.catalog);
    PlatformFree(paths);

    if (benchmarkSink == UINT64_MAX) WriteText("");
    WriteText("audio pack benchmark done\n");
    LAIUE_TEST_SUCCESS();
}
