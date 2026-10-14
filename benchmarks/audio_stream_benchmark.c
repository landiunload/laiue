// Ручной PCM/WAV-stream benchmark: один looping голос, одинаковые исходные
// сэмплы, offscreen 48 кГц, mono/stereo, speed 1, pan -1. Сравнивает весь
// decode+mix путь stream с готовым PCM; диска, worker scheduling и MP3 здесь
// нет. Создание, allocation и первоначальное заполнение исключены из замера.
// Семь raw timings и median печатаются без performance assertions.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "audio/audio_stream.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <string.h>

#define BENCH_RATE 48000u
#define BENCH_BUFFERS 10000u
#define BENCH_FRAMES 256u
#define BENCH_REPETITIONS 7u

static volatile uint64_t benchmarkSink;

static void Require(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21], output[22];
    uint32_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    for (uint32_t index = 0u; index < count; ++index)
        output[index] = digits[count - index - 1u];
    output[count] = '\0';
    LaiueTestRuntimeWrite(output);
}

static void WriteNsPerFrame(uint64_t microseconds)
{
    uint64_t thousandths = microseconds * UINT64_C(1000000) / (BENCH_BUFFERS * BENCH_FRAMES);
    WriteUnsigned(thousandths / 1000u);
    LaiueTestRuntimeWrite(".");
    uint64_t fraction = thousandths % 1000u;
    if (fraction < 100u)
        LaiueTestRuntimeWrite("0");
    if (fraction < 10u)
        LaiueTestRuntimeWrite("0");
    WriteUnsigned(fraction);
}

typedef struct MemorySource
{
    uint8_t *bytes;
    uint32_t size;
} MemorySource;

static uint32_t ReadAt(void *context, uint64_t offset, void *bytes, uint32_t count)
{
    const MemorySource *source = (const MemorySource *)context;
    if (offset > source->size || count > source->size - offset)
        return 0u;
    memcpy(bytes, source->bytes + (size_t)offset, count);
    return 1u;
}

static void Put32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static MemorySource MakeWave(uint32_t channels)
{
    MemorySource source = {NULL, 44u + BENCH_RATE * channels * sizeof(int16_t)};
    source.bytes = (uint8_t *)PlatformAllocate(source.size, true);
    Require(source.bytes != NULL, "wave allocation failed");
    memcpy(source.bytes, "RIFF", 4u);
    Put32(source.bytes + 4u, source.size - 8u);
    memcpy(source.bytes + 8u, "WAVEfmt ", 8u);
    Put32(source.bytes + 16u, 16u);
    source.bytes[20] = 1u;
    source.bytes[22] = (uint8_t)channels;
    Put32(source.bytes + 24u, BENCH_RATE);
    Put32(source.bytes + 28u, BENCH_RATE * channels * 2u);
    source.bytes[32] = (uint8_t)(channels * 2u);
    source.bytes[34] = 16u;
    memcpy(source.bytes + 36u, "data", 4u);
    Put32(source.bytes + 40u, source.size - 44u);
    for (uint32_t frame = 0u; frame < BENCH_RATE; ++frame)
        for (uint32_t channel = 0u; channel < channels; ++channel)
        {
            uint16_t sample =
                (uint16_t)(1000u + ((frame % 20000u) * 13u + channel * 1000u) % 20000u);
            uint32_t offset = 44u + (frame * channels + channel) * 2u;
            source.bytes[offset] = (uint8_t)sample;
            source.bytes[offset + 1u] = (uint8_t)(sample >> 8);
        }
    return source;
}

static AudioDevice *CreateDevice(void)
{
    AudioDeviceConfiguration configuration = {AUDIO_BACKEND_OFFSCREEN, BENCH_RATE, BENCH_FRAMES,
                                              1.0f};
    AudioDevice *device = NULL;
    Require(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
            "device creation failed");
    return device;
}

static uint64_t Run(AudioDevice *device, float *output)
{
    double start = PlatformMonotonicSeconds();
    for (uint32_t buffer = 0u; buffer < BENCH_BUFFERS; ++buffer)
        AudioDeviceRenderFrames(device, output, BENCH_FRAMES);
    uint64_t elapsed = (uint64_t)((PlatformMonotonicSeconds() - start) * 1000000.0);
    // Read output outside the timed interval so LTO must preserve useful work.
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t index = 0u; index < BENCH_FRAMES * 2u; ++index)
    {
        uint32_t bits = 0u;
        memcpy(&bits, output + index, sizeof(bits));
        hash = (hash ^ bits) * UINT64_C(1099511628211);
    }
    benchmarkSink = hash;
    return elapsed;
}

static void PrintTimings(const char *label, const uint64_t timings[BENCH_REPETITIONS])
{
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(" raw us: ");
    uint64_t sorted[BENCH_REPETITIONS];
    for (uint32_t index = 0u; index < BENCH_REPETITIONS; ++index)
    {
        WriteUnsigned(timings[index]);
        LaiueTestRuntimeWrite(index + 1u < BENCH_REPETITIONS ? ", " : "\n");
        sorted[index] = timings[index];
    }
    for (uint32_t index = 1u; index < BENCH_REPETITIONS; ++index)
    {
        uint64_t value = sorted[index];
        uint32_t target = index;
        while (target != 0u && sorted[target - 1u] > value)
        {
            sorted[target] = sorted[target - 1u];
            --target;
        }
        sorted[target] = value;
    }
    uint64_t median = sorted[BENCH_REPETITIONS / 2u];
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(" median us: ");
    WriteUnsigned(median);
    LaiueTestRuntimeWrite("; ns/frame: ");
    WriteNsPerFrame(median);
    LaiueTestRuntimeWrite("\n");
}

static void Measure(uint32_t channels)
{
    MemorySource source = MakeWave(channels);
    AudioDevice *pcmDevice = CreateDevice(), *streamDevice = CreateDevice();
    AudioClipDescription description = {(const int16_t *)(source.bytes + 44u), BENCH_RATE, channels,
                                        BENCH_RATE};
    AudioClip *clip = NULL;
    Require(AudioClipCreate(pcmDevice, &description, &clip) == AUDIO_RESULT_OK,
            "clip creation failed");
    AudioStreamDescription streamDescription = {sizeof(streamDescription), &source, ReadAt, NULL,
                                                source.size};
    AudioStream *stream = NULL;
    Require(AudioStreamCreate(streamDevice, &streamDescription, &stream) == AUDIO_RESULT_OK,
            "stream creation failed");
    AudioVoiceParameters parameters = {1.0f, -1.0f, 1.0f, true};
    Require(AudioVoicePlay(pcmDevice, clip, &parameters) != AUDIO_VOICE_NONE, "clip play failed");
    Require(AudioVoicePlayStream(streamDevice, stream, &parameters) != AUDIO_VOICE_NONE,
            "stream play failed");
    float output[BENCH_FRAMES * 2u];
    uint64_t pcmTimings[BENCH_REPETITIONS], streamTimings[BENCH_REPETITIONS];
    Run(pcmDevice, output);
    uint64_t pcmHash = benchmarkSink;
    Run(streamDevice, output);
    Require(pcmHash == benchmarkSink, "warmup output differs between PCM and stream");
    for (uint32_t repetition = 0u; repetition < BENCH_REPETITIONS; ++repetition)
    {
        pcmTimings[repetition] = Run(pcmDevice, output);
        pcmHash = benchmarkSink;
        streamTimings[repetition] = Run(streamDevice, output);
        Require(pcmHash == benchmarkSink, "output differs between PCM and stream");
    }
    LaiueTestRuntimeWrite(channels == 1u ? "mono\n" : "stereo\n");
    PrintTimings("PCM", pcmTimings);
    PrintTimings("WAV stream", streamTimings);
    AudioClipDestroy(clip);
    AudioDeviceDestroy(pcmDevice);
    AudioDeviceDestroy(streamDevice);
    PlatformFree(source.bytes);
}

LAIUE_TEST_ENTRY(AudioStreamBenchmarkEntryPoint)
{
    LaiueTestRuntimeWrite("PCM vs memory WAV stream; 1 looping voice; 48000 Hz; speed 1; pan -1\n"
                          "10000 buffers x 256 frames; 7 paired repetitions after one warmup\n");
#if defined(__clang__)
    LaiueTestRuntimeWrite("compiler: Clang " __clang_version__ "\n");
#elif defined(_MSC_FULL_VER)
    LaiueTestRuntimeWrite("compiler: MSVC full version ");
    WriteUnsigned(_MSC_FULL_VER);
    LaiueTestRuntimeWrite("\n");
#elif defined(__GNUC__)
    LaiueTestRuntimeWrite("compiler: GCC " __VERSION__ "\n");
#endif
    Measure(1u);
    Measure(2u);
    LaiueTestRuntimeWrite("checksum: ");
    WriteUnsigned(benchmarkSink);
    LaiueTestRuntimeWrite("\n");
    LAIUE_TEST_SUCCESS();
}
