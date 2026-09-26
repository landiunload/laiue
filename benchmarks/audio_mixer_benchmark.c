// Узкий бенчмарк микшера: 0/1/16/64/128 голосов, моно/стерео и разные
// частоты источника. Расширяет engine_benchmark (тот меряет один сценарий
// на 64 голосах), чтобы отдельно видеть тишину, целочисленный шаг,
// ресемплинг и стоимость обхода слотов. Харнесс без CRT общий с тестами.
//
// Методика та же, что в engine_benchmark: медиана нечётного числа выборок и
// контрольная сумма в volatile, чтобы компилятор не выбросил работу.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_COUNT 9u
#define DEVICE_SAMPLE_RATE 48000u
#define BUFFER_FRAMES 480u
#define CLIP_FRAMES 48000u

static volatile uint64_t benchmarkSink;

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
    for (uint32_t index = 0; index < length; ++index) text[index] = digits[length - index - 1u];
    text[length] = '\0';
    WriteText(text);
}

static void WriteHex(uint64_t value)
{
    char digits[17];
    const char *alphabet = "0123456789abcdef";
    uint32_t length = 0u;
    if (value == 0u) digits[length++] = '0';
    while (value != 0u)
    {
        digits[length++] = alphabet[value & 0xful];
        value >>= 4;
    }
    char text[18];
    for (uint32_t index = 0; index < length; ++index) text[index] = digits[length - index - 1u];
    text[length] = '\0';
    WriteText(text);
}

static void WriteMilliseconds(double value)
{
    if (value < 0.0) value = 0.0;
    uint64_t thousandths = (uint64_t)(value * 1000.0 + 0.5);
    WriteUnsigned(thousandths / 1000u);
    WriteText(".");
    uint64_t fraction = thousandths % 1000u;
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static int CompareDouble(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && CompareDouble(&samples[insertion - 1u], &value) > 0)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
    return samples[count / 2u];
}

typedef struct MixScenario
{
    const char *name;
    uint32_t voices;
    uint32_t channels;
    uint32_t sourceRate;
    uint32_t iterations;
    bool unifiedSpeed;   // true — все голоса со скоростью 1 (целочисленный шаг)
} MixScenario;

static uint32_t NextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

// Контрольная сумма по последнему отрендеренному буферу: baseline и candidate
// обязаны совпасть побитово, иначе это не чистый перф-патч.
static uint64_t HashFrames(const float *frames, uint32_t sampleCount)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t index = 0; index < sampleCount; ++index)
    {
        union
        {
            float f;
            uint32_t u;
        } bits;
        bits.f = frames[index];
        hash ^= (uint64_t)bits.u;
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool MakeClip(AudioDevice *device, uint32_t channels, uint32_t sampleRate,
                     AudioClip **outClip)
{
    uint32_t sampleCount = CLIP_FRAMES * channels;
    int16_t *storage = PlatformAllocate((size_t)sampleCount * sizeof(int16_t), false);
    if (storage == NULL) return false;
    uint32_t state = 0x9e3779b9u;
    for (uint32_t index = 0; index < sampleCount; ++index)
    {
        storage[index] = (int16_t)(int32_t)(NextRandom(&state) >> 16);
    }
    AudioClipDescription description = {
        .samples = storage,
        .frameCount = CLIP_FRAMES,
        .channelCount = channels,
        .sampleRate = sampleRate,
    };
    AudioClip *clip = NULL;
    if (AudioClipCreate(device, &description, &clip) != AUDIO_RESULT_OK)
    {
        PlatformFree(storage);
        return false;
    }
    PlatformFree(storage);
    *outClip = clip;
    return true;
}

static bool RunScenario(const MixScenario *scenario, uint64_t *hashOut)
{
    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = DEVICE_SAMPLE_RATE,
        .frameCountHint = BUFFER_FRAMES,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    if (AudioDeviceCreate(&configuration, &device) != AUDIO_RESULT_OK) return false;

    AudioClip *clip = NULL;
    if (!MakeClip(device, scenario->channels, scenario->sourceRate, &clip))
    {
        AudioDeviceDestroy(device);
        return false;
    }

    float *frames = PlatformAllocate(BUFFER_FRAMES * 2u * sizeof(float), true);
    if (frames == NULL)
    {
        AudioClipDestroy(clip);
        AudioDeviceDestroy(device);
        return false;
    }

    for (uint32_t index = 0; index < scenario->voices; ++index)
    {
        AudioVoiceParameters parameters = {
            .volume = 0.5f,
            .pan = ((float)(index % 5u) - 2.0f) * 0.5f,
            .speed = scenario->unifiedSpeed ? 1.0f
                                            : 0.75f + (float)(index % 7u) * 0.1f,
            .looping = true,
        };
        if (AudioVoicePlay(device, clip, &parameters) == AUDIO_VOICE_NONE)
        {
            PlatformFree(frames);
            AudioClipDestroy(clip);
            AudioDeviceDestroy(device);
            return false;
        }
    }

    // Прогрев: страницы, кеш и кристаллизация ветвлений.
    uint32_t warmup = scenario->iterations / 4u;
    if (warmup < 64u) warmup = 64u;
    for (uint32_t index = 0; index < warmup; ++index)
    {
        AudioDeviceRenderFrames(device, frames, BUFFER_FRAMES);
    }

    double samples[SAMPLE_COUNT];
    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0; iteration < scenario->iterations; ++iteration)
        {
            AudioDeviceRenderFrames(device, frames, BUFFER_FRAMES);
            benchmarkSink += (uint64_t)(frames[0] * 1000.0f);
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }

    AudioDeviceRenderFrames(device, frames, BUFFER_FRAMES);
    *hashOut = HashFrames(frames, BUFFER_FRAMES * 2u);

    WriteText("audio.mix.");
    WriteText(scenario->name);
    WriteText(": median ");
    WriteMilliseconds(Median(samples, SAMPLE_COUNT));
    WriteText(" ms for ");
    WriteUnsigned(scenario->iterations);
    WriteText(" renders voices=");
    WriteUnsigned(scenario->voices);
    WriteText(" channels=");
    WriteUnsigned(scenario->channels);
    WriteText(" rate=");
    WriteUnsigned(scenario->sourceRate);
    WriteText(" hash=");
    WriteHex(*hashOut);
    WriteText("\n");

    PlatformFree(frames);
    AudioClipDestroy(clip);
    AudioDeviceDestroy(device);
    return true;
}

LAIUE_TEST_ENTRY(AudioMixerBenchmarkEntryPoint)
{
    static const MixScenario scenarios[] = {
        {"silence", 0u, 1u, DEVICE_SAMPLE_RATE, 20000u, true},
        {"mono1_step1", 1u, 1u, DEVICE_SAMPLE_RATE, 4000u, true},
        {"stereo1_step1", 1u, 2u, DEVICE_SAMPLE_RATE, 4000u, true},
        {"mono1_resample", 1u, 1u, 44100u, 4000u, true},
        {"mono16", 16u, 1u, DEVICE_SAMPLE_RATE, 1000u, false},
        {"stereo16", 16u, 2u, DEVICE_SAMPLE_RATE, 1000u, false},
        {"mono64", 64u, 1u, DEVICE_SAMPLE_RATE, 300u, false},
        {"stereo64", 64u, 2u, DEVICE_SAMPLE_RATE, 300u, false},
        {"mono128", 128u, 1u, DEVICE_SAMPLE_RATE, 150u, false},
        {"stereo128", 128u, 2u, DEVICE_SAMPLE_RATE, 150u, false},
    };

    WriteText("laiue audio mixer benchmark\n");
    WriteText("device_rate=");
    WriteUnsigned(DEVICE_SAMPLE_RATE);
    WriteText(" frames=");
    WriteUnsigned(BUFFER_FRAMES);
    WriteText(" samples=");
    WriteUnsigned(SAMPLE_COUNT);
    WriteText("\n");

    uint64_t totalHash = 0u;
    for (uint32_t index = 0u; index < sizeof(scenarios) / sizeof(scenarios[0]); ++index)
    {
        uint64_t hash = 0u;
        if (!RunScenario(&scenarios[index], &hash))
        {
            WriteText("scenario could not run\n");
            LaiueTestRuntimeExit(1);
        }
        totalHash = totalHash * 31u + hash;
    }

    WriteText("audio.mix.total_hash=");
    WriteHex(totalHash);
    WriteText("\n");
    if (benchmarkSink == UINT64_MAX) WriteText("");
    LAIUE_TEST_SUCCESS();
}
