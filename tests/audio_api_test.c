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

    PlatformFree(frames);
    AudioClipDestroy(clip);
    AudioDeviceDestroy(device);

    LaiueTestRuntimeWrite("Audio mixer checks passed\n");
    LAIUE_TEST_SUCCESS();
}
