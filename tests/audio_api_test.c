// Микшер целиком: голоса, панорама, скорость, повтор, устаревшие
// дескрипторы и предел числа голосов. Проверяются смешанные сэмплы, а не
// коды возврата: микшер, который «успешно» отдаёт тишину, отличается от
// работающего только содержимым буфера.
//
// Устройство создаётся с offscreen-бэкендом, поэтому тест не требует ни
// звуковой карты, ни звукового сервера и одинаково идёт в CI и локально.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define TEST_SAMPLE_RATE 48000u
#define TEST_FRAMES 512u
#define CLIP_FRAMES 256u

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("Audio check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static float AbsoluteValue(float value)
{
    return value < 0.0f ? -value : value;
}

// Пиковая амплитуда по каналу: сравнение пиков отличает панораму,
// громкость и повтор надёжнее, чем сравнение отдельных сэмплов.
static float ChannelPeak(const float *frames, uint32_t frameCount, uint32_t channel)
{
    float peak = 0.0f;
    for (uint32_t index = 0; index < frameCount; ++index)
    {
        float value = AbsoluteValue(frames[index * 2u + channel]);
        if (value > peak) peak = value;
    }
    return peak;
}

static AudioDevice *CreateOffscreenDevice(void)
{
    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = TEST_SAMPLE_RATE,
        .frameCountHint = TEST_FRAMES,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
           "the offscreen device could not be created");
    Expect(device != NULL, "a successful create must return a device");
    return device;
}

// === Эталонный сценарий ===
// Фиксированный микс, который задействует оба канала, разные длины,
// частоты, скорости и повтор. Хеш выхода снят с микшера до оптимизации;
// любое изменение арифметики микса обязано его сдвинуть.

static AudioClip *MakeReferenceClip(AudioDevice *device, uint32_t frameCount,
                                    uint32_t channelCount, uint32_t sampleRate, uint32_t seed)
{
    int16_t *samples =
        PlatformAllocate((size_t)frameCount * channelCount * sizeof(int16_t), false);
    Expect(samples != NULL, "reference clip samples could not be allocated");
    uint32_t state = seed * 2654435761u + 1u;
    for (uint32_t index = 0; index < frameCount * channelCount; ++index)
    {
        state = state * 1664525u + 1013904223u;
        samples[index] = (int16_t)(int32_t)(state >> 16);
    }
    AudioClipDescription description = {
        .samples = samples,
        .frameCount = frameCount,
        .channelCount = channelCount,
        .sampleRate = sampleRate,
    };
    AudioClip *clip = NULL;
    Expect(AudioClipCreate(device, &description, &clip) == AUDIO_RESULT_OK,
           "a reference clip could not be created");
    PlatformFree(samples);
    return clip;
}

static uint64_t ReferenceHashFrames(const float *frames, uint32_t frameCount)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t index = 0; index < frameCount * 2u; ++index)
    {
        union
        {
            float f;
            uint32_t u;
        } pun;
        pun.f = frames[index];
        uint32_t bits = pun.u;
        for (uint32_t byte = 0; byte < 4u; ++byte)
        {
            hash ^= (bits >> (byte * 8u)) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static uint64_t ReferenceMixHash(void)
{
    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = TEST_SAMPLE_RATE,
        .frameCountHint = 512u,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
           "the reference device could not be created");
    AudioClip *mono = MakeReferenceClip(device, 300u, 1u, 48000u, 11u);
    AudioClip *stereo = MakeReferenceClip(device, 777u, 2u, 44100u, 22u);
    AudioClip *shortClip = MakeReferenceClip(device, 64u, 1u, 32000u, 33u);

    AudioVoiceParameters first = {
        .volume = 0.8f, .pan = -0.3f, .speed = 1.0f, .looping = true,
    };
    AudioVoiceParameters second = {
        .volume = 0.6f, .pan = 0.7f, .speed = 0.913f, .looping = true,
    };
    AudioVoiceParameters third = {
        .volume = 1.0f, .pan = 0.0f, .speed = 1.7f, .looping = false,
    };
    AudioVoiceParameters fourth = {
        .volume = 0.5f, .pan = 1.0f, .speed = 2.5f, .looping = true,
    };
    Expect(AudioVoicePlay(device, mono, &first) != AUDIO_VOICE_NONE, "reference voice one");
    Expect(AudioVoicePlay(device, stereo, &second) != AUDIO_VOICE_NONE, "reference voice two");
    Expect(AudioVoicePlay(device, shortClip, &third) != AUDIO_VOICE_NONE, "reference voice three");
    Expect(AudioVoicePlay(device, mono, &fourth) != AUDIO_VOICE_NONE, "reference voice four");

    float *frames = PlatformAllocate(512u * 2u * sizeof(float), false);
    Expect(frames != NULL, "reference mix buffer could not be allocated");
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t buffer = 0u; buffer < 4u; ++buffer)
    {
        Expect(AudioDeviceRenderFrames(device, frames, 512u), "reference render must succeed");
        uint64_t part = ReferenceHashFrames(frames, 512u);
        hash ^= part + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    }
    PlatformFree(frames);

    AudioClipDestroy(mono);
    AudioClipDestroy(stereo);
    AudioClipDestroy(shortClip);
    AudioDeviceDestroy(device);
    return hash;
}

LAIUE_TEST_ENTRY(AudioApiTestEntryPoint)
{
    AudioDevice *device = CreateOffscreenDevice();

    AudioDeviceStats stats;
    Expect(AudioDeviceGetStats(device, &stats), "stats must be readable");
    Expect(stats.sampleRate == TEST_SAMPLE_RATE, "the device must honour the requested rate");
    Expect(stats.channelCount == 2u, "mixing is always stereo");
    Expect(stats.activeVoices == 0u, "a fresh device has no active voices");

    // Постоянная амплитуда: пик предсказуем и не зависит от фазы.
    int16_t *samples = PlatformAllocate(CLIP_FRAMES * sizeof(int16_t), false);
    Expect(samples != NULL, "clip samples could not be allocated");
    for (uint32_t index = 0; index < CLIP_FRAMES; ++index) samples[index] = 16384;

    AudioClipDescription description = {
        .samples = samples,
        .frameCount = CLIP_FRAMES,
        .channelCount = 1u,
        .sampleRate = TEST_SAMPLE_RATE,
    };
    AudioClip *clip = NULL;
    Expect(AudioClipCreate(device, &description, &clip) == AUDIO_RESULT_OK,
           "the clip could not be created");
    // Клип копирует сэмплы: исходный буфер после создания не нужен.
    PlatformFree(samples);
    Expect(AudioClipDurationSeconds(clip) > 0.0, "the clip must report a duration");

    float *frames = PlatformAllocate(TEST_FRAMES * 2u * sizeof(float), true);
    Expect(frames != NULL, "the mix buffer could not be allocated");

    // === Тишина без голосов ===
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(ChannelPeak(frames, TEST_FRAMES, 0u) == 0.0f, "a device without voices must be silent");

    // === Один голос по центру ===
    AudioVoice voice = AudioVoicePlay(device, clip, NULL);
    Expect(voice != AUDIO_VOICE_NONE, "the voice could not be started");
    Expect(AudioVoiceIsActive(device, voice), "a started voice must report as active");

    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    float centreLeft = ChannelPeak(frames, TEST_FRAMES, 0u);
    float centreRight = ChannelPeak(frames, TEST_FRAMES, 1u);
    Expect(centreLeft > 0.0f, "a playing voice must produce sound");
    Expect(AbsoluteValue(centreLeft - centreRight) < 0.001f,
           "a centred voice must reach both channels equally");

    // Клип короче буфера и не зациклен, поэтому к концу кадра он должен
    // закончиться сам, без остановки со стороны приложения.
    Expect(!AudioVoiceIsActive(device, voice), "a finished voice must stop reporting as active");

    // === Панорама ===
    AudioVoiceParameters leftParameters = {
        .volume = 1.0f, .pan = -1.0f, .speed = 1.0f, .looping = false,
    };
    voice = AudioVoicePlay(device, clip, &leftParameters);
    Expect(voice != AUDIO_VOICE_NONE, "the panned voice could not be started");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(ChannelPeak(frames, TEST_FRAMES, 0u) > 0.0f, "a hard-left voice must fill the left");
    Expect(ChannelPeak(frames, TEST_FRAMES, 1u) < 0.001f,
           "a hard-left voice must leave the right channel silent");

    // === Повтор и скорость ===
    AudioVoiceParameters loopParameters = {
        .volume = 1.0f, .pan = 0.0f, .speed = 1.0f, .looping = true,
    };
    AudioVoice loopVoice = AudioVoicePlay(device, clip, &loopParameters);
    Expect(loopVoice != AUDIO_VOICE_NONE, "the looping voice could not be started");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    // Буфер вдвое длиннее клипа: без повтора вторая половина была бы тихой.
    Expect(ChannelPeak(frames + CLIP_FRAMES * 2u, TEST_FRAMES - CLIP_FRAMES, 0u) > 0.0f,
           "a looping voice must keep sounding past the end of its clip");
    Expect(AudioVoiceIsActive(device, loopVoice), "a looping voice must stay active");

    AudioVoiceParameters fastParameters = loopParameters;
    fastParameters.speed = 2.0f;
    Expect(AudioVoiceSetParameters(device, loopVoice, &fastParameters),
           "parameters of a live voice must be accepted");

    // === Остановка и устаревший дескриптор ===
    AudioVoiceStop(device, loopVoice);
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(!AudioVoiceIsActive(device, loopVoice), "a stopped voice must not stay active");
    Expect(!AudioVoiceSetParameters(device, loopVoice, &loopParameters),
           "a stale handle must be rejected");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(ChannelPeak(frames, TEST_FRAMES, 0u) == 0.0f,
           "no voice may sound after every voice stopped");

    // === Общая громкость ===
    AudioDeviceSetMasterVolume(device, 0.0f);
    Expect(AudioDeviceGetMasterVolume(device) == 0.0f, "master volume must be read back");
    voice = AudioVoicePlay(device, clip, NULL);
    Expect(voice != AUDIO_VOICE_NONE, "the voice could not be started");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(ChannelPeak(frames, TEST_FRAMES, 0u) == 0.0f,
           "zero master volume must silence the mix");
    AudioDeviceSetMasterVolume(device, 1.0f);

    // === Предел числа голосов ===
    // Голоса не вытесняют друг друга: сверх лимита выдача честно
    // отказывает, а уже звучащее не обрывается.
    uint32_t startedVoices = 0u;
    for (uint32_t index = 0; index < AUDIO_MAX_VOICES * 2u; ++index)
    {
        if (AudioVoicePlay(device, clip, &loopParameters) != AUDIO_VOICE_NONE) ++startedVoices;
    }
    Expect(startedVoices <= AUDIO_MAX_VOICES, "the mixer must not exceed its voice limit");
    Expect(startedVoices > 0u, "the mixer must accept at least one voice");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(AudioDeviceGetStats(device, &stats), "stats must be readable");
    Expect(stats.activeVoices > 0u, "active voices must be reported");
    Expect(stats.mixedFrames > 0u, "mixed frames must be counted");

    AudioDeviceStopAllVoices(device);
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(AudioDeviceGetStats(device, &stats), "stats must be readable");
    Expect(stats.activeVoices == 0u, "stopping every voice must clear the active count");

    // === Отказы контракта ===
    Expect(AudioClipCreate(device, NULL, &clip) == AUDIO_RESULT_INVALID_ARGUMENT,
           "a NULL description must be rejected");
    AudioClipDescription invalid = description;
    invalid.channelCount = 3u;
    AudioClip *rejected = NULL;
    Expect(AudioClipCreate(device, &invalid, &rejected) == AUDIO_RESULT_INVALID_ARGUMENT,
           "an unsupported channel count must be rejected");
    Expect(AudioVoicePlay(device, NULL, NULL) == AUDIO_VOICE_NONE,
           "playing a NULL clip must be refused");

    // === Побитовая неизменность микса ===
    Expect(ReferenceMixHash() == 0x95070508644f2382ull,
           "the mixed output must stay bit-identical to the reference");

    PlatformFree(frames);
    AudioClipDestroy(clip);
    AudioDeviceDestroy(device);

    LaiueTestRuntimeWrite("Audio mixer checks passed\n");
    LAIUE_TEST_SUCCESS();
}
