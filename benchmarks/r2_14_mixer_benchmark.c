// R2 14-mixer: A/B-стенд горячего микширования. Один и тот же
// исполняемый файл линкуется с laiue_audio.dll, поэтому baseline и
// candidate сравниваются подменой ровно одной DLL (интерфейс не меняется).
//
// Сценарии покрывают тишину, целочисленный шаг, ресемплинг, питч, повтор
// через границу клипа, разное число голосов (1/8/16/64) и мягкое
// ограничение при перегрузке суммой. Для каждого сценария печатаются ВСЕ
// сырые выборки времени: анализ делает внешний скрипт, а не харнесс.
//
// Побитовый хеш последнего буфера печатается рядом: baseline и candidate
// обязаны совпасть, иначе это не чистый перф-патч.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_COUNT 9u
#define DEVICE_SAMPLE_RATE 48000u
#define BUFFER_FRAMES 480u

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

static void WriteMicroseconds(double milliseconds)
{
    if (milliseconds < 0.0) milliseconds = 0.0;
    uint64_t microseconds = (uint64_t)(milliseconds * 1000.0 + 0.5);
    WriteUnsigned(microseconds / 1000u);
    WriteText(".");
    uint64_t fraction = microseconds % 1000u;
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

typedef struct MixScenario
{
    const char *name;
    uint32_t voices;
    uint32_t channels;
    uint32_t sourceRate;
    uint32_t clipFrames;
    float speedBase;    // скорость голоса 0
    float speedSpread;  // прибавка (index % 7) * speedSpread
    bool looping;
    float masterVolume;
    uint32_t iterations;
} MixScenario;

static uint32_t NextRandom(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

// Контрольная сумма по последнему отрендеренному буферу: baseline и
// candidate обязаны совпасть побитово.
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
                     uint32_t frameCount, AudioClip **outClip)
{
    uint32_t sampleCount = frameCount * channels;
    int16_t *storage = PlatformAllocate((size_t)sampleCount * sizeof(int16_t), false);
    if (storage == NULL) return false;
    uint32_t state = 0x9e3779b9u;
    for (uint32_t index = 0; index < sampleCount; ++index)
    {
        storage[index] = (int16_t)(int32_t)(NextRandom(&state) >> 16);
    }
    AudioClipDescription description = {
        .samples = storage,
        .frameCount = frameCount,
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
        .masterVolume = scenario->masterVolume,
    };
    AudioDevice *device = NULL;
    if (AudioDeviceCreate(&configuration, &device) != AUDIO_RESULT_OK) return false;

    AudioClip *clip = NULL;
    if (!MakeClip(device, scenario->channels, scenario->sourceRate, scenario->clipFrames, &clip))
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
            .speed = scenario->speedBase + (float)(index % 7u) * scenario->speedSpread,
            .looping = scenario->looping,
        };
        if (AudioVoicePlay(device, clip, &parameters) == AUDIO_VOICE_NONE)
        {
            PlatformFree(frames);
            AudioClipDestroy(clip);
            AudioDeviceDestroy(device);
            return false;
        }
    }

    uint32_t warmup = scenario->iterations / 4u;
    if (warmup < 64u) warmup = 64u;
    for (uint32_t index = 0; index < warmup; ++index)
    {
        AudioDeviceRenderFrames(device, frames, BUFFER_FRAMES);
    }

    WriteText("S ");
    WriteText(scenario->name);
    WriteText(" voices=");
    WriteUnsigned(scenario->voices);
    WriteText(" channels=");
    WriteUnsigned(scenario->channels);
    WriteText(" rate=");
    WriteUnsigned(scenario->sourceRate);
    WriteText(" clip=");
    WriteUnsigned(scenario->clipFrames);
    WriteText(" loop=");
    WriteUnsigned(scenario->looping ? 1u : 0u);
    WriteText(" master=");
    WriteUnsigned((uint64_t)(scenario->masterVolume * 1000.0f + 0.5f));
    WriteText(" iterations=");
    WriteUnsigned(scenario->iterations);
    WriteText("\n");

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0; iteration < scenario->iterations; ++iteration)
        {
            AudioDeviceRenderFrames(device, frames, BUFFER_FRAMES);
            benchmarkSink += (uint64_t)(frames[0] * 1000.0f);
        }
        double elapsed = (PlatformMonotonicSeconds() - start) * 1000.0;

        WriteText("s ");
        WriteText(scenario->name);
        WriteText(" ");
        WriteUnsigned(sample);
        WriteText(" ");
        WriteMicroseconds(elapsed);
        WriteText("\n");
    }

    AudioDeviceRenderFrames(device, frames, BUFFER_FRAMES);
    *hashOut = HashFrames(frames, BUFFER_FRAMES * 2u);

    WriteText("H ");
    WriteText(scenario->name);
    WriteText(" ");
    WriteHex(*hashOut);
    WriteText("\n");

    PlatformFree(frames);
    AudioClipDestroy(clip);
    AudioDeviceDestroy(device);
    return true;
}

LAIUE_TEST_ENTRY(R2MixerBenchmarkEntryPoint)
{
    static const MixScenario scenarios[] = {
        {"silence", 0u, 1u, DEVICE_SAMPLE_RATE, 48000u, 1.0f, 0.0f, true, 1.0f, 20000u},
        {"mono1_step1", 1u, 1u, DEVICE_SAMPLE_RATE, 48000u, 1.0f, 0.0f, true, 1.0f, 4000u},
        {"stereo1_step1", 1u, 2u, DEVICE_SAMPLE_RATE, 48000u, 1.0f, 0.0f, true, 1.0f, 4000u},
        {"mono1_resample", 1u, 1u, 44100u, 48000u, 1.0f, 0.0f, true, 1.0f, 4000u},
        {"stereo1_resample", 1u, 2u, 22050u, 48000u, 1.0f, 0.0f, true, 1.0f, 4000u},
        {"mono1_pitch", 1u, 1u, DEVICE_SAMPLE_RATE, 48000u, 1.5f, 0.0f, true, 1.0f, 4000u},
        {"mono1_boundary", 1u, 1u, DEVICE_SAMPLE_RATE, 240u, 2.0f, 0.0f, true, 1.0f, 4000u},
        {"mono8", 8u, 1u, DEVICE_SAMPLE_RATE, 48000u, 0.75f, 0.1f, true, 1.0f, 2000u},
        {"stereo8", 8u, 2u, DEVICE_SAMPLE_RATE, 48000u, 0.75f, 0.1f, true, 1.0f, 2000u},
        {"mono16", 16u, 1u, DEVICE_SAMPLE_RATE, 48000u, 0.75f, 0.1f, true, 1.0f, 1000u},
        {"stereo16", 16u, 2u, DEVICE_SAMPLE_RATE, 48000u, 0.75f, 0.1f, true, 1.0f, 1000u},
        {"stereo16_resample", 16u, 2u, 44100u, 48000u, 0.75f, 0.1f, true, 1.0f, 1000u},
        {"mono64", 64u, 1u, DEVICE_SAMPLE_RATE, 48000u, 0.75f, 0.1f, true, 1.0f, 300u},
        {"stereo64", 64u, 2u, DEVICE_SAMPLE_RATE, 48000u, 0.75f, 0.1f, true, 1.0f, 300u},
        {"mono64_step1", 64u, 1u, DEVICE_SAMPLE_RATE, 48000u, 1.0f, 0.0f, true, 1.0f, 300u},
        {"stereo64_step1", 64u, 2u, DEVICE_SAMPLE_RATE, 48000u, 1.0f, 0.0f, true, 1.0f, 300u},
        {"mono64_clamp", 64u, 1u, DEVICE_SAMPLE_RATE, 48000u, 0.75f, 0.1f, true, 0.5f, 300u},
    };

    WriteText("R2 14-mixer bench device_rate=");
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
            WriteText("scenario could not run: ");
            WriteText(scenarios[index].name);
            WriteText("\n");
            LaiueTestRuntimeExit(1);
        }
        totalHash = totalHash * 31u + hash;
    }

    WriteText("TOTAL ");
    WriteHex(totalHash);
    WriteText("\n");
    if (benchmarkSink == UINT64_MAX) WriteText("");
    LAIUE_TEST_SUCCESS();
}
