// A/B harness for the audio pack loader (ROUND2, 15-audio-pack).
//
// Measures the paths named by the task: `.la` load, PCM16 direct view and the
// unaligned fallback copy, ADPCM decode for short/large samples, and the
// end-to-end load through the content catalog. Linked against laiue_audio.dll,
// so the very same executable measures a baseline DLL and a candidate DLL —
// only the DLL is swapped between runs.
//
// Two modes, selected by environment:
//   R2_15_AUDIO_PACK_WORKLOAD  one workload name, or empty for all;
//   R2_15_AUDIO_PACK_VERIFY    "1" renders every decoded clip and prints a
//                              checksum of the mixed samples, so the output of
//                              baseline and candidate can be compared exactly.
//
// As with the other engine benchmarks it is EXCLUDE_FROM_ALL and never runs in
// CTest. The samples are printed, not just the median, so the analysis can keep
// every raw value.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "audio/audio_pack.h"
#include "content/content_catalog.h"
#include "media/la_encode.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define BENCH_SAMPLE_COUNT 9u
#define BENCH_SAMPLE_RATE 48000u
#define BENCH_RENDER_BLOCK 65536u

// Large/short sample mix: a 4M-frame PCM16 sound is 8 MiB, the same frame count
// of ADPCM is a quarter of it but decodes four million nibbles. Stereo is held
// at two million frames so mono and stereo decode the same number of nibbles.
#define WORK_PCM_VIEW_FRAMES 4000000u
#define WORK_PCM_COPY_FRAMES 4000000u
#define WORK_ADPCM_MONO_FRAMES 4000000u
#define WORK_ADPCM_STEREO_FRAMES 2000000u
#define WORK_FILE_FRAMES 2000000u

#define WORK_PCM_VIEW_ITER 96u
#define WORK_PCM_COPY_ITER 32u
#define WORK_ADPCM_MONO_ITER 8u
#define WORK_ADPCM_STEREO_ITER 4u
#define WORK_FILE_ITER 64u

static volatile uint64_t benchmarkSink;

static char g_selectedWorkload[64];
static bool g_workloadAll;
static bool g_verify;

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

// === Output without CRT ===

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

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        text[15u - index] = digits[value & 0xFu];
        value >>= 4u;
    }
    text[16] = '\0';
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

static void Fail(const char *message)
{
    WriteText("r2_15_audio_pack_benchmark: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

// === Deterministic fixtures ===

static uint64_t Fnv1a(uint64_t hash, const void *bytes, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)bytes;
    for (size_t index = 0u; index < size; ++index)
    {
        hash ^= (uint64_t)cursor[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

static int16_t SampleAt(uint32_t index, uint32_t seed)
{
    uint32_t phase = index % 256u;
    int32_t triangle = phase < 128u ? (int32_t)phase : (int32_t)(256u - phase);
    int32_t value = (triangle - 64) * 96;
    uint32_t noise = (seed * 1664525u + 1013904223u + index * 22695477u) >> 13u;
    value += (int32_t)(noise & 511u) - 256;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
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

// Version-2 `.la` header: source stamp is zero, the file is not derived.
static void WriteLaHeader(uint8_t *bytes, uint32_t channels, uint32_t encoding, uint32_t frames,
                          uint32_t payloadBytes)
{
    bytes[0] = 'L';
    bytes[1] = 'A';
    bytes[2] = 'S';
    bytes[3] = '1';
    WriteU16Le(bytes + 4, SOUND_LA_VERSION);
    WriteU16Le(bytes + 6, SOUND_LA_HEADER_BYTES);
    WriteU16Le(bytes + 8, channels);
    WriteU16Le(bytes + 10, encoding);
    WriteU32Le(bytes + 12, BENCH_SAMPLE_RATE);
    WriteU32Le(bytes + 16, frames);
    WriteU32Le(bytes + 20, payloadBytes);
    for (uint32_t index = 24u; index < SOUND_LA_HEADER_BYTES; ++index) bytes[index] = 0u;
}

typedef struct Fixture
{
    uint8_t *allocation;   // owned block
    uint8_t *bytes;        // .la image (may be allocation + 1 when misaligned)
    uint32_t sizeBytes;
    uint32_t frameCount;
    uint32_t channelCount;
    uint64_t checksum;
} Fixture;

static Fixture BuildPcmFixture(uint32_t frames, uint32_t channels, bool misaligned)
{
    Fixture fixture;
    fixture.frameCount = frames;
    fixture.channelCount = channels;
    uint32_t payloadBytes = frames * channels * 2u;
    uint32_t total = SOUND_LA_HEADER_BYTES + payloadBytes;
    uint32_t slack = misaligned ? 1u : 0u;
    fixture.allocation = (uint8_t *)PlatformAllocate(total + slack, false);
    if (fixture.allocation == NULL) Fail("pcm fixture could not be allocated");
    fixture.bytes = fixture.allocation + slack;
    fixture.sizeBytes = total;
    WriteLaHeader(fixture.bytes, channels, SOUND_ENCODING_PCM16, frames, payloadBytes);
    uint8_t *payload = fixture.bytes + SOUND_LA_HEADER_BYTES;
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        for (uint32_t channel = 0u; channel < channels; ++channel)
        {
            int16_t sample = SampleAt(frame, channel + 1u);
            WriteU16Le(payload + ((size_t)frame * channels + channel) * 2u,
                       (uint32_t)(uint16_t)sample);
        }
    }
    fixture.checksum = Fnv1a(1469598103934665603ull, fixture.bytes, total);
    return fixture;
}

static Fixture BuildAdpcmFixture(uint32_t frames, uint32_t channels)
{
    Fixture fixture;
    fixture.frameCount = frames;
    fixture.channelCount = channels;
    uint32_t total = 0u;
    if (SoundEncodedBytes(SOUND_ENCODING_ADPCM, frames, channels, &total) != SOUND_OK)
        Fail("adpcm fixture size could not be computed");
    fixture.allocation = (uint8_t *)PlatformAllocate(total, false);
    if (fixture.allocation == NULL) Fail("adpcm fixture could not be allocated");
    fixture.bytes = fixture.allocation;
    fixture.sizeBytes = total;

    int16_t *samples = (int16_t *)PlatformAllocate((size_t)frames * channels * sizeof(int16_t), false);
    if (samples == NULL) Fail("adpcm source samples could not be allocated");
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        for (uint32_t channel = 0u; channel < channels; ++channel)
        {
            samples[(size_t)frame * channels + channel] = SampleAt(frame, channel + 7u);
        }
    }
    SoundClip clip = {
        .samples = samples,
        .frameCount = frames,
        .channelCount = channels,
        .sampleRate = BENCH_SAMPLE_RATE,
        .encoding = SOUND_ENCODING_ADPCM,
        .sourceModifiedTime = 0u,
        .sourceSizeBytes = 0u,
    };
    uint32_t written = 0u;
    if (SoundEncode(&clip, fixture.bytes, total, &written) != SOUND_OK || written != total)
        Fail("adpcm fixture could not be encoded");
    PlatformFree(samples);
    fixture.checksum = Fnv1a(1469598103934665603ull, fixture.bytes, total);
    return fixture;
}

// === Catalog for the end-to-end file workload ===

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

typedef struct FilePack
{
    LaiueContentCatalog *catalog;
    Fixture fixture;
} FilePack;

// Пути живут в куче: пять буферов по 32 килослова переполнили бы
// страницу стека, а в сборке без CRT нет __chkstk.
typedef struct FilePaths
{
    wchar_t root[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t sounds[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t directory[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t path[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t active[LAIUE_CONTENT_PATH_CAPACITY];
} FilePaths;

static void PrepareFilePack(FilePack *pack, const wchar_t *executable)
{
    FilePaths *paths = (FilePaths *)PlatformAllocate(sizeof(*paths), false);
    if (paths == NULL) Fail("file pack path scratch could not be allocated");
    if (!Join(paths->root, LAIUE_CONTENT_PATH_CAPACITY, executable, L"r2_15_audio_pack_bench") ||
        !Join(paths->sounds, LAIUE_CONTENT_PATH_CAPACITY, paths->root, L"sounds") ||
        !Join(paths->directory, LAIUE_CONTENT_PATH_CAPACITY, paths->sounds, L"Bench.lap") ||
        !Join(paths->path, LAIUE_CONTENT_PATH_CAPACITY, paths->directory, L"pcm.la"))
    {
        Fail("file pack path construction failed");
    }

    (void)PlatformDeleteFile(paths->path);
    PlatformRemoveDirectory(paths->directory);
    PlatformRemoveDirectory(paths->sounds);
    PlatformRemoveDirectory(paths->root);
    if (!PlatformCreateDirectory(paths->root) || !PlatformCreateDirectory(paths->sounds) ||
        !PlatformCreateDirectory(paths->directory))
    {
        Fail("file pack directories could not be created");
    }

    pack->fixture = BuildPcmFixture(WORK_FILE_FRAMES, 1u, false);
    if (!PlatformWriteEntireFile(paths->path, pack->fixture.bytes, pack->fixture.sizeBytes))
    {
        Fail("file pack sound could not be written");
    }
    static const char text[] = "Bench.lap\n";
    if (!Join(paths->active, LAIUE_CONTENT_PATH_CAPACITY, paths->sounds, L"active.txt") ||
        !PlatformWriteEntireFile(paths->active, text, sizeof(text) - 1u))
    {
        Fail("file pack active.txt could not be written");
    }

    pack->catalog = LaiueContentCatalogCreate(paths->root);
    if (pack->catalog == NULL || !AudioPackActivateIn(pack->catalog, L"Bench.lap"))
    {
        Fail("file pack catalog could not be activated");
    }
    PlatformFree(paths);
}

// === Measurement ===

typedef struct Context
{
    AudioDevice *device;
    const Fixture *fixture;          // NULL for the catalog workload
    const FilePack *filePack;
    const wchar_t *soundName;
} Context;

static void LoadOnce(const Context *context)
{
    AudioPackLoadStatus status = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    AudioClip *clip;
    if (context->fixture != NULL)
    {
        clip = AudioClipLoadMemory(context->device, context->fixture->bytes,
                                   context->fixture->sizeBytes, &status);
    }
    else
    {
        clip = AudioClipLoadFrom(context->device, context->filePack->catalog, context->soundName,
                                 &status);
    }
    if (clip == NULL || status != AUDIO_PACK_LOAD_OK) Fail("a fixture failed to load");
    benchmarkSink += (uint64_t)(AudioClipDurationSeconds(clip) * 1000.0);
    AudioClipDestroy(clip);
}

static double MedianCopy(const double *times)
{
    double copy[BENCH_SAMPLE_COUNT];
    for (uint32_t index = 0u; index < BENCH_SAMPLE_COUNT; ++index) copy[index] = times[index];
    for (uint32_t index = 1u; index < BENCH_SAMPLE_COUNT; ++index)
    {
        double value = copy[index];
        uint32_t insertion = index;
        while (insertion > 0u && copy[insertion - 1u] > value)
        {
            copy[insertion] = copy[insertion - 1u];
            --insertion;
        }
        copy[insertion] = value;
    }
    return copy[BENCH_SAMPLE_COUNT / 2u];
}

static void Report(const char *name, const double *times, uint32_t operations, uint32_t frames,
                   uint32_t channels, uint64_t checksum)
{
    double minimum = times[0];
    double maximum = times[0];
    double sum = 0.0;
    for (uint32_t index = 0u; index < BENCH_SAMPLE_COUNT; ++index)
    {
        if (times[index] < minimum) minimum = times[index];
        if (times[index] > maximum) maximum = times[index];
        sum += times[index];
    }
    WriteText("workload ");
    WriteText(name);
    WriteText(" ops=");
    WriteUnsigned(operations);
    WriteText(" frames=");
    WriteUnsigned(frames);
    WriteText(" channels=");
    WriteUnsigned(channels);
    WriteText(" checksum=");
    WriteHex(checksum);
    WriteText(" median_ms=");
    WriteMilliseconds(MedianCopy(times));
    WriteText(" mean_ms=");
    WriteMilliseconds(sum / (double)BENCH_SAMPLE_COUNT);
    WriteText(" min_ms=");
    WriteMilliseconds(minimum);
    WriteText(" max_ms=");
    WriteMilliseconds(maximum);
    WriteText(" samples_ms");
    for (uint32_t index = 0u; index < BENCH_SAMPLE_COUNT; ++index)
    {
        WriteText(" ");
        WriteMilliseconds(times[index]);
    }
    WriteText("\n");
}

static void Measure(const char *name, const Context *context, uint32_t iterations)
{
    LoadOnce(context);   // Warm up outside the statistics.
    double times[BENCH_SAMPLE_COUNT];
    for (uint32_t sample = 0u; sample < BENCH_SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0u; iteration < iterations; ++iteration) LoadOnce(context);
        times[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }
    const uint32_t frames = context->fixture != NULL ? context->fixture->frameCount
                                                     : context->filePack->fixture.frameCount;
    const uint32_t channels = context->fixture != NULL ? context->fixture->channelCount
                                                       : context->filePack->fixture.channelCount;
    const uint64_t checksum = context->fixture != NULL ? context->fixture->checksum
                                                       : context->filePack->fixture.checksum;
    Report(name, times, iterations, frames, channels, checksum);
}

// Render every decoded clip once and hash the mixed samples, so baseline and
// candidate outputs can be compared exactly rather than by duration.
static void Verify(const char *name, const Context *context)
{
    AudioPackLoadStatus status = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    AudioClip *clip;
    if (context->fixture != NULL)
    {
        clip = AudioClipLoadMemory(context->device, context->fixture->bytes,
                                   context->fixture->sizeBytes, &status);
    }
    else
    {
        clip = AudioClipLoadFrom(context->device, context->filePack->catalog, context->soundName,
                                 &status);
    }
    if (clip == NULL || status != AUDIO_PACK_LOAD_OK) Fail("a fixture failed to verify");

    uint32_t frames = context->fixture != NULL ? context->fixture->frameCount
                                               : context->filePack->fixture.frameCount;
    AudioVoice voice = AudioVoicePlay(context->device, clip, NULL);
    if (voice == AUDIO_VOICE_NONE) Fail("a fixture voice could not be played");

    float *block = (float *)PlatformAllocate((size_t)BENCH_RENDER_BLOCK * 2u * sizeof(float), true);
    if (block == NULL) Fail("verify scratch could not be allocated");

    uint64_t hash = 1469598103934665603ull;
    uint32_t remaining = frames;
    while (remaining != 0u)
    {
        uint32_t count = remaining < BENCH_RENDER_BLOCK ? remaining : BENCH_RENDER_BLOCK;
        if (!AudioDeviceRenderFrames(context->device, block, count)) Fail("verify render failed");
        hash = Fnv1a(hash, block, (size_t)count * 2u * sizeof(float));
        remaining -= count;
    }
    PlatformFree(block);
    AudioDeviceStopAllVoices(context->device);
    AudioClipDestroy(clip);

    WriteText("verify ");
    WriteText(name);
    WriteText(" frames=");
    WriteUnsigned(frames);
    WriteText(" checksum=");
    WriteHex(hash);
    WriteText("\n");
}

LAIUE_TEST_ENTRY(R2AudioPackBenchmarkEntryPoint)
{
    WriteText(g_verify ? "r2 audio pack benchmark (verify)\n" : "r2 audio pack benchmark\n");

    uint32_t selectedLength = PlatformGetEnvironmentUtf8(
        "R2_15_AUDIO_PACK_WORKLOAD", g_selectedWorkload, (uint32_t)sizeof(g_selectedWorkload));
    if (selectedLength >= (uint32_t)sizeof(g_selectedWorkload))
        Fail("workload name is too long");
    g_workloadAll = selectedLength == 0u;

    char verifyText[8];
    uint32_t verifyLength =
        PlatformGetEnvironmentUtf8("R2_15_AUDIO_PACK_VERIFY", verifyText, (uint32_t)sizeof(verifyText));
    g_verify = verifyLength == 1u && verifyText[0] == '1';

    const bool wantView = WantWorkload("pcm_mem_view");
    const bool wantCopy = WantWorkload("pcm_mem_copy");
    const bool wantAdpcmMono = WantWorkload("adpcm_mem_mono");
    const bool wantAdpcmStereo = WantWorkload("adpcm_mem_stereo");
    const bool wantFile = WantWorkload("pcm_la_file");
    if (!wantView && !wantCopy && !wantAdpcmMono && !wantAdpcmStereo && !wantFile)
        Fail("unknown workload");

    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = BENCH_SAMPLE_RATE,
        .frameCountHint = 256u,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    if (AudioDeviceCreate(&configuration, &device) != AUDIO_RESULT_OK)
        Fail("device could not be created");

    wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    if (!PlatformExecutableDirectory(executable, LAIUE_PLATFORM_PATH_CAPACITY))
        Fail("executable directory");

    Fixture viewFixture = {0};
    Fixture copyFixture = {0};
    Fixture adpcmMonoFixture = {0};
    Fixture adpcmStereoFixture = {0};
    FilePack filePack = {0};

    if (wantView) viewFixture = BuildPcmFixture(WORK_PCM_VIEW_FRAMES, 1u, false);
    if (wantCopy) copyFixture = BuildPcmFixture(WORK_PCM_COPY_FRAMES, 1u, true);
    if (wantAdpcmMono) adpcmMonoFixture = BuildAdpcmFixture(WORK_ADPCM_MONO_FRAMES, 1u);
    if (wantAdpcmStereo) adpcmStereoFixture = BuildAdpcmFixture(WORK_ADPCM_STEREO_FRAMES, 2u);
    if (wantFile) PrepareFilePack(&filePack, executable);

    Context context;
    context.device = device;
    context.soundName = L"pcm";

    if (wantView)
    {
        context.fixture = &viewFixture;
        context.filePack = NULL;
        if (g_verify) Verify("pcm_mem_view", &context);
        else Measure("pcm_mem_view", &context, WORK_PCM_VIEW_ITER);
    }
    if (wantCopy)
    {
        context.fixture = &copyFixture;
        context.filePack = NULL;
        if (g_verify) Verify("pcm_mem_copy", &context);
        else Measure("pcm_mem_copy", &context, WORK_PCM_COPY_ITER);
    }
    if (wantAdpcmMono)
    {
        context.fixture = &adpcmMonoFixture;
        context.filePack = NULL;
        if (g_verify) Verify("adpcm_mem_mono", &context);
        else Measure("adpcm_mem_mono", &context, WORK_ADPCM_MONO_ITER);
    }
    if (wantAdpcmStereo)
    {
        context.fixture = &adpcmStereoFixture;
        context.filePack = NULL;
        if (g_verify) Verify("adpcm_mem_stereo", &context);
        else Measure("adpcm_mem_stereo", &context, WORK_ADPCM_STEREO_ITER);
    }
    if (wantFile)
    {
        context.fixture = NULL;
        context.filePack = &filePack;
        if (g_verify) Verify("pcm_la_file", &context);
        else Measure("pcm_la_file", &context, WORK_FILE_ITER);
    }

    if (wantView) PlatformFree(viewFixture.allocation);
    if (wantCopy) PlatformFree(copyFixture.allocation);
    if (wantAdpcmMono) PlatformFree(adpcmMonoFixture.allocation);
    if (wantAdpcmStereo) PlatformFree(adpcmStereoFixture.allocation);
    if (wantFile)
    {
        PlatformFree(filePack.fixture.allocation);
        LaiueContentCatalogDestroy(filePack.catalog);
    }
    AudioDeviceDestroy(device);

    if (benchmarkSink == UINT64_MAX) WriteText("");
    WriteText("r2 audio pack benchmark done\n");
    LAIUE_TEST_SUCCESS();
}
