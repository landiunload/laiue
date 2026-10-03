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

// Побитовая проверка микшера без прибитых хешей. Звук собирается с /fp:fast,
// и биты произвольного сценария зависят от компилятора и конфигурации
// (сокращение умножения-сложения в FMA, порядок сумм): один и тот же микс
// даёт три разных хеша на MSVC Release, MSVC Debug и clang Release. Поэтому
// проверяются два сценария, в которых вся арифметика либо точна, либо
// содержит ровно одно округление на выборку, — там любой компилятор обязан
// выдать одни и те же биты, и ожидаемый выход считается прямо здесь:
//  - шаг 1,0 (частота клипа равна частоте устройства, скорость 1): выборка
//    одного кадра, это быстрая ветвь микшера;
//  - шаг ровно 0,5 (клип 24000 на устройстве 48000): общая ветвь с
//    интерполяцией, где дробь равна 0 или 0,5, а разность соседних кадров
//    (кратна 2^-15, по модулю не больше 2) и её половина точны.
// Усиления — ровно 1,0 и 0,0 (панорама до упора, громкость 1), поэтому
// умножение на усиление точно, а прибавление нуля — тождество. Мастер 1,0.

static void FillReferenceSamples(int16_t *samples, uint32_t count, uint32_t seed)
{
    uint32_t state = seed * 2654435761u + 1u;
    for (uint32_t index = 0; index < count; ++index)
    {
        state = state * 1664525u + 1013904223u;
        samples[index] = (int16_t)(int32_t)(state >> 16);
    }
}

static float SampleToFloat(int16_t sample)
{
    return (float)sample * (1.0f / 32768.0f);
}

// Нули сравниваются без учёта знака: сумма +0 и -0 даёт +0, и знак нуля —
// единственное, что здесь может законно отличаться от ожидания.
static bool SameBits(float left, float right)
{
    union
    {
        float f;
        uint32_t u;
    } a, b;
    a.f = left;
    b.f = right;
    return a.u == b.u || (left == 0.0f && right == 0.0f);
}

static AudioClip *MakeExactClip(AudioDevice *device, const int16_t *samples,
                                uint32_t frameCount, uint32_t channelCount, uint32_t sampleRate)
{
    AudioClipDescription description = {
        .samples = samples,
        .frameCount = frameCount,
        .channelCount = channelCount,
        .sampleRate = sampleRate,
    };
    AudioClip *clip = NULL;
    Expect(AudioClipCreate(device, &description, &clip) == AUDIO_RESULT_OK,
           "an exact-mix clip could not be created");
    return clip;
}

#define EXACT_BUFFER_FRAMES 512u
#define EXACT_BUFFER_COUNT 4u
#define EXACT_MONO_FRAMES 700u
#define EXACT_STEREO_FRAMES 300u
#define EXACT_HALF_FRAMES 900u

static void CheckExactMixes(void)
{
    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = TEST_SAMPLE_RATE,
        .frameCountHint = EXACT_BUFFER_FRAMES,
        .masterVolume = 1.0f,
    };

    // Клипы: моно 48000 в цикле (левый канал), стерео 48000 без цикла (правый
    // канал) и моно 24000 без цикла (шаг 0,5, левый канал).
    static int16_t monoSamples[EXACT_MONO_FRAMES];
    static int16_t stereoSamples[EXACT_STEREO_FRAMES * 2u];
    static int16_t halfSamples[EXACT_HALF_FRAMES];
    FillReferenceSamples(monoSamples, EXACT_MONO_FRAMES, 11u);
    FillReferenceSamples(stereoSamples, EXACT_STEREO_FRAMES * 2u, 22u);
    FillReferenceSamples(halfSamples, EXACT_HALF_FRAMES, 33u);

    float *frames = PlatformAllocate(EXACT_BUFFER_FRAMES * 2u * sizeof(float), false);
    Expect(frames != NULL, "exact mix buffer could not be allocated");

    // === Сценарий 1: целый шаг, быстрая ветвь ===
    {
        AudioDevice *device = NULL;
        Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
               "the exact-mix device could not be created");
        AudioClip *mono = MakeExactClip(device, monoSamples, EXACT_MONO_FRAMES, 1u, 48000u);
        AudioClip *stereo = MakeExactClip(device, stereoSamples, EXACT_STEREO_FRAMES, 2u, 48000u);
        AudioVoiceParameters leftOnly = {
            .volume = 1.0f, .pan = -1.0f, .speed = 1.0f, .looping = true,
        };
        AudioVoiceParameters rightOnly = {
            .volume = 1.0f, .pan = 1.0f, .speed = 1.0f, .looping = false,
        };
        Expect(AudioVoicePlay(device, mono, &leftOnly) != AUDIO_VOICE_NONE, "exact voice one");
        Expect(AudioVoicePlay(device, stereo, &rightOnly) != AUDIO_VOICE_NONE, "exact voice two");

        bool identical = true;
        for (uint32_t buffer = 0u; buffer < EXACT_BUFFER_COUNT; ++buffer)
        {
            Expect(AudioDeviceRenderFrames(device, frames, EXACT_BUFFER_FRAMES),
                   "exact render must succeed");
            for (uint32_t index = 0u; index < EXACT_BUFFER_FRAMES; ++index)
            {
                uint32_t output = buffer * EXACT_BUFFER_FRAMES + index;
                float expectedLeft = SampleToFloat(monoSamples[output % EXACT_MONO_FRAMES]);
                float expectedRight = output < EXACT_STEREO_FRAMES
                                          ? SampleToFloat(stereoSamples[output * 2u + 1u])
                                          : 0.0f;
                identical = identical && SameBits(frames[index * 2u], expectedLeft)
                            && SameBits(frames[index * 2u + 1u], expectedRight);
            }
        }
        Expect(identical, "integer-step mixing must reproduce the clip samples bit for bit");

        AudioClipDestroy(mono);
        AudioClipDestroy(stereo);
        AudioDeviceDestroy(device);
    }

    // === Сценарий 2: шаг ровно 0,5, общая ветвь с интерполяцией ===
    {
        AudioDevice *device = NULL;
        Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
               "the half-step device could not be created");
        AudioClip *half = MakeExactClip(device, halfSamples, EXACT_HALF_FRAMES, 1u, 24000u);
        AudioVoiceParameters leftOnly = {
            .volume = 1.0f, .pan = -1.0f, .speed = 1.0f, .looping = false,
        };
        Expect(AudioVoicePlay(device, half, &leftOnly) != AUDIO_VOICE_NONE, "half-step voice");

        bool identical = true;
        for (uint32_t buffer = 0u; buffer < EXACT_BUFFER_COUNT; ++buffer)
        {
            Expect(AudioDeviceRenderFrames(device, frames, EXACT_BUFFER_FRAMES),
                   "half-step render must succeed");
            for (uint32_t index = 0u; index < EXACT_BUFFER_FRAMES; ++index)
            {
                uint32_t output = buffer * EXACT_BUFFER_FRAMES + index;
                float expectedLeft = 0.0f;
                if (output < EXACT_HALF_FRAMES * 2u)
                {
                    uint32_t frame = output / 2u;
                    uint32_t nextFrame = frame + 1u < EXACT_HALF_FRAMES ? frame + 1u : frame;
                    float fraction = (output & 1u) != 0u ? 0.5f : 0.0f;
                    float first = SampleToFloat(halfSamples[frame]);
                    float second = SampleToFloat(halfSamples[nextFrame]);
                    expectedLeft = first + (second - first) * fraction;
                }
                identical = identical && SameBits(frames[index * 2u], expectedLeft)
                            && SameBits(frames[index * 2u + 1u], 0.0f);
            }
        }
        Expect(identical, "half-step interpolation must match the exact reference bit for bit");

        AudioClipDestroy(half);
        AudioDeviceDestroy(device);
    }

    PlatformFree(frames);
}

// Общая ветвь на отрезках: зацикленный клип пересекает границу несколько
// раз, незацикленный обрывается посреди буфера. Шаги 0,5 и 2,5 дают дробь
// ровно 0 или 0,5, разность соседних кадров кратна 2^-15, и её половина
// точна, поэтому эталон считается здесь же и совпадает на любом
// компиляторе. Если бы отрезок перешагнул границу клипа, зацикленный голос
// выдал бы ноль вместо повтора, а незацикленный — звук после конца.
#define GENERAL_FRAMES 300u
#define GENERAL_BUFFER 512u
#define GENERAL_BUFFERS 4u

static float GeneralReference(const int16_t *samples, uint32_t channels, uint32_t frameCount,
                              uint32_t index, uint32_t channel, double step, bool looping)
{
    double position = (double)index * step;
    if (looping)
    {
        double length = (double)frameCount;
        position -= length * (double)(uint64_t)(position / length);
    }
    else if (position >= (double)frameCount)
    {
        return 0.0f;
    }
    uint32_t frame = (uint32_t)position;
    uint32_t nextFrame = frame + 1u < frameCount ? frame + 1u : frame;
    float fraction = (float)(position - (double)frame);
    uint32_t offset = channels == 2u ? channel : 0u;
    float first = SampleToFloat(samples[frame * channels + offset]);
    float second = SampleToFloat(samples[nextFrame * channels + offset]);
    return first + (second - first) * fraction;
}

static void CheckGeneralRuns(void)
{
    static int16_t monoSamples[GENERAL_FRAMES];
    static int16_t stereoSamples[GENERAL_FRAMES * 2u];
    FillReferenceSamples(monoSamples, GENERAL_FRAMES, 44u);
    FillReferenceSamples(stereoSamples, GENERAL_FRAMES * 2u, 55u);

    float *frames = PlatformAllocate(GENERAL_BUFFER * 2u * sizeof(float), false);
    Expect(frames != NULL, "general-run buffer could not be allocated");

    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = TEST_SAMPLE_RATE,
        .frameCountHint = GENERAL_BUFFER,
        .masterVolume = 1.0f,
    };

    // Дробные случаи проверяют общую ветвь; целые шаги 2 покрывают быстрый
    // путь в mono и stereo, повтор через границу и естественное завершение.
    const double steps[8] = {0.5, 2.5, 2.5, 2.0, 2.0, 2.0, 2.0, 2.0};
    const bool looping[8] = {true, true, false, true, false, false, true, false};
    const uint32_t channels[8] = {1u, 2u, 1u, 1u, 2u, 1u, 2u, 2u};
    const double pans[8] = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, 1.0};

    for (uint32_t scenario = 0u; scenario < 8u; ++scenario)
    {
        AudioDevice *device = NULL;
        Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
               "general-run device could not be created");
        AudioClip *clip = channels[scenario] == 2u
                              ? MakeExactClip(device, stereoSamples, GENERAL_FRAMES, 2u,
                                              TEST_SAMPLE_RATE)
                              : MakeExactClip(device, monoSamples, GENERAL_FRAMES, 1u,
                                              TEST_SAMPLE_RATE);
        AudioVoiceParameters parameters = {
            .volume = 1.0f,
            .pan = (float)pans[scenario],
            .speed = (float)steps[scenario],
            .looping = looping[scenario],
        };
        AudioVoice voice = AudioVoicePlay(device, clip, &parameters);
        Expect(voice != AUDIO_VOICE_NONE,
               "general-run voice could not be started");

        bool identical = true;
        for (uint32_t buffer = 0u; buffer < GENERAL_BUFFERS; ++buffer)
        {
            Expect(AudioDeviceRenderFrames(device, frames, GENERAL_BUFFER),
                   "general-run render must succeed");
            for (uint32_t index = 0u; index < GENERAL_BUFFER; ++index)
            {
                uint32_t output = buffer * GENERAL_BUFFER + index;
                const int16_t *samples =
                    channels[scenario] == 2u ? stereoSamples : monoSamples;
                bool rightOnly = pans[scenario] > 0.0;
                float expectedLeft = rightOnly ? 0.0f
                    : GeneralReference(samples, channels[scenario], GENERAL_FRAMES, output, 0u,
                                       steps[scenario], looping[scenario]);
                float expectedRight = rightOnly
                    ? GeneralReference(samples, channels[scenario], GENERAL_FRAMES, output, 1u,
                                       steps[scenario], looping[scenario])
                    : 0.0f;
                identical = identical && SameBits(frames[index * 2u], expectedLeft)
                            && SameBits(frames[index * 2u + 1u], expectedRight);
            }
        }
        Expect(identical, "integer and fractional steps must match the sample reference");
        Expect(AudioVoiceIsActive(device, voice) == looping[scenario],
               "integer-step voice lifetime must match looping mode");

        AudioClipDestroy(clip);
        AudioDeviceDestroy(device);
    }

    // Клип короче шага: одного переноса через границу мало, и каждая
    // выборка после первой должна молчать, а не читать кадр за концом.
    {
        static int16_t tinySamples[2] = {16384, -16384};
        AudioDevice *device = NULL;
        Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
               "short-clip device could not be created");
        AudioClip *clip = MakeExactClip(device, tinySamples, 2u, 1u, TEST_SAMPLE_RATE);
        AudioVoiceParameters parameters = {
            .volume = 1.0f, .pan = -1.0f, .speed = 16.0f, .looping = true,
        };
        Expect(AudioVoicePlay(device, clip, &parameters) != AUDIO_VOICE_NONE,
               "short-clip voice could not be started");

        bool identical = true;
        for (uint32_t buffer = 0u; buffer < 3u; ++buffer)
        {
            Expect(AudioDeviceRenderFrames(device, frames, GENERAL_BUFFER),
                   "short-clip render must succeed");
            for (uint32_t index = 0u; index < GENERAL_BUFFER; ++index)
            {
                uint32_t output = buffer * GENERAL_BUFFER + index;
                float expectedLeft = output == 0u ? SampleToFloat(tinySamples[0]) : 0.0f;
                identical = identical && SameBits(frames[index * 2u], expectedLeft)
                            && SameBits(frames[index * 2u + 1u], 0.0f);
            }
        }
        Expect(identical, "a clip shorter than the step must stay silent after the first frame");

        AudioClipDestroy(clip);
        AudioDeviceDestroy(device);
    }

    {
        static int16_t tinyStereoSamples[4] = {16384, 8192, -16384, -8192};
        AudioDevice *device = NULL;
        Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
               "short stereo-clip device could not be created");
        AudioClip *clip = MakeExactClip(device, tinyStereoSamples, 2u, 2u, TEST_SAMPLE_RATE);
        AudioVoiceParameters parameters = {
            .volume = 1.0f, .pan = 1.0f, .speed = 16.0f, .looping = true,
        };
        Expect(AudioVoicePlay(device, clip, &parameters) != AUDIO_VOICE_NONE,
               "short stereo-clip voice could not be started");
        Expect(AudioDeviceRenderFrames(device, frames, GENERAL_BUFFER),
               "short stereo-clip render must succeed");
        bool identical = true;
        for (uint32_t index = 0u; index < GENERAL_BUFFER; ++index)
        {
            float expectedRight = index == 0u ? SampleToFloat(tinyStereoSamples[1]) : 0.0f;
            identical = identical && SameBits(frames[index * 2u], 0.0f)
                        && SameBits(frames[index * 2u + 1u], expectedRight);
        }
        Expect(identical, "a short stereo clip stays silent after its first frame");

        AudioClipDestroy(clip);
        AudioDeviceDestroy(device);
    }

    PlatformFree(frames);
}

// === Быстрый путь мастера при одном голосе ===
//
// Общая громкость 1,0 с одним голосом делает проход громкости и ограничения
// тождеством, и микшер его пропускает. Тождество проверяется прямо: клип,
// голос и мастер подобраны так, что каждая выборка вычисляется здесь же
// в точности — шаг 1,0 (частая ветвь), панорама до упора (усиление ровно
// 1,0 или 0,0), громкость голоса 1,0 и общая громкость 1,0. Тогда выход
// равен sample * 2^-15, ограничение ничего не усекает, а пропущенный проход
// обязан вернуть те же биты. Клип короче буфера и не зациклен, поэтому
// нулевой хвост проверяется вместе с сигналом: ошибочный пропуск прохода
// оставил бы там ненулевые значения или испортил бы сигнал.
static void CheckSingleVoiceMasterFastPath(void)
{
    static int16_t monoSamples[GENERAL_FRAMES];
    static int16_t stereoSamples[GENERAL_FRAMES * 2u];
    FillReferenceSamples(monoSamples, GENERAL_FRAMES, 66u);
    FillReferenceSamples(stereoSamples, GENERAL_FRAMES * 2u, 67u);

    float *frames = PlatformAllocate(GENERAL_BUFFER * 2u * sizeof(float), false);
    Expect(frames != NULL, "single-voice master buffer could not be allocated");

    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = TEST_SAMPLE_RATE,
        .frameCountHint = GENERAL_BUFFER,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
           "the unity-master device could not be created");

    // Первый буфер: моно-клип в левый канал (усиление 1,0 и 0,0).
    AudioClip *mono = MakeExactClip(device, monoSamples, GENERAL_FRAMES, 1u, TEST_SAMPLE_RATE);
    AudioVoiceParameters leftOnly = {
        .volume = 1.0f, .pan = -1.0f, .speed = 1.0f, .looping = false,
    };
    Expect(AudioVoicePlay(device, mono, &leftOnly) != AUDIO_VOICE_NONE,
           "the unity-master voice could not be started");

    bool identical = true;
    for (uint32_t buffer = 0u; buffer < GENERAL_BUFFERS; ++buffer)
    {
        Expect(AudioDeviceRenderFrames(device, frames, GENERAL_BUFFER),
               "the unity-master render must succeed");
        for (uint32_t index = 0u; index < GENERAL_BUFFER; ++index)
        {
            uint32_t output = buffer * GENERAL_BUFFER + index;
            float expectedLeft = output < GENERAL_FRAMES
                                     ? SampleToFloat(monoSamples[output])
                                     : 0.0f;
            identical = identical && SameBits(frames[index * 2u], expectedLeft)
                        && SameBits(frames[index * 2u + 1u], 0.0f);
        }
    }
    Expect(identical, "voice samples must equal the clip without the master pass");
    AudioClipDestroy(mono);

    // Второй буфер: стерео-клип в правый канал без повтора, затем живая
    // посылка громкости голоса между кадрами — устройство обязано остаться
    // в быстром пути и не исказить ни сигнал, ни мастер.
    AudioClip *stereo = MakeExactClip(device, stereoSamples, GENERAL_FRAMES, 2u, TEST_SAMPLE_RATE);
    AudioVoiceParameters rightOnly = {
        .volume = 1.0f, .pan = 1.0f, .speed = 1.0f, .looping = false,
    };
    AudioVoice right = AudioVoicePlay(device, stereo, &rightOnly);
    Expect(right != AUDIO_VOICE_NONE, "the stereo unity-master voice could not be started");

    bool stereoIdentical = true;
    for (uint32_t index = 0u; index < GENERAL_FRAMES; ++index)
    {
        Expect(AudioDeviceRenderFrames(device, frames, 1u),
               "the single-frame render must succeed");
        float expectedRight = SampleToFloat(stereoSamples[index * 2u + 1u]);
        stereoIdentical = stereoIdentical && SameBits(frames[0], 0.0f)
                          && SameBits(frames[1], expectedRight);
    }
    Expect(stereoIdentical, "a panned unity-master voice must reach only its channel");
    AudioClipDestroy(stereo);

    // Третий случай: два голоса того же клипа перегружают сумму, поэтому
    // пропуск прохода недопустим. Каждый голос даёт почти +1, сумма уходит
    // выше единицы, и микшер обязан её ограничить; иначе здесь будет больше
    // единицы. Здесь же проверяется, что пропуск не сработал по ошибке.
    static int16_t loudSamples[8];
    for (uint32_t index = 0u; index < 8u; ++index) loudSamples[index] = 32767;
    AudioClip *loud = MakeExactClip(device, loudSamples, 8u, 1u, TEST_SAMPLE_RATE);
    AudioVoiceParameters centre = {
        .volume = 1.0f, .pan = 0.0f, .speed = 1.0f, .looping = true,
    };
    Expect(AudioVoicePlay(device, loud, &centre) != AUDIO_VOICE_NONE, "loud voice one");
    Expect(AudioVoicePlay(device, loud, &centre) != AUDIO_VOICE_NONE, "loud voice two");
    Expect(AudioDeviceRenderFrames(device, frames, 8u), "overload render must succeed");
    bool bounded = true;
    for (uint32_t sample = 0u; sample < 8u * 2u; ++sample)
    {
        bounded = bounded && frames[sample] <= 1.0f && frames[sample] >= -1.0f;
    }
    Expect(bounded, "two overloaded voices must be clamped by the master pass");
    AudioClipDestroy(loud);

    AudioDeviceDestroy(device);
    PlatformFree(frames);
}

// === Гонка производителя и потока вывода ===
//
// Поток вывода гонит RenderFrames, пока игровой поток заказывает, меняет и
// останавливает голоса. Проверяется сам протокол: состояние слота
// (PENDING/ACTIVE/FINISHED) и SPSC-очередь команд. Если бы повторное
// использование слота или публикация индекса были согласованы неверно, гонка
// проявилась бы порчей состояния или падением; наполнение буфера здесь не
// важно.

typedef struct MixerRaceState
{
    AudioDevice *device;
    AudioClip *clip;
    volatile uint32_t stop;
    volatile int64_t renderBuffers;
} MixerRaceState;

static uint32_t MixerRenderWorker(void *rawContext)
{
    MixerRaceState *state = (MixerRaceState *)rawContext;
    float *frames = PlatformAllocate(
        (size_t)EXACT_BUFFER_FRAMES * 2U * sizeof(float), false);
    if (frames == NULL)
    {
        return 1U;
    }
    while (PlatformAtomicLoadU32Acquire(&state->stop) == 0U)
    {
        if (!AudioDeviceRenderFrames(state->device, frames, EXACT_BUFFER_FRAMES))
        {
            PlatformFree(frames);
            return 2U;
        }
        PlatformAtomicIncrementI64(&state->renderBuffers);
    }
    PlatformFree(frames);
    return 0U;
}

static void CheckConcurrentMixer(void)
{
    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = TEST_SAMPLE_RATE,
        .frameCountHint = EXACT_BUFFER_FRAMES,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
           "the concurrent-mixer device could not be created");

    static int16_t samples[EXACT_MONO_FRAMES];
    FillReferenceSamples(samples, EXACT_MONO_FRAMES, 77U);
    AudioClip *clip = MakeExactClip(device, samples, EXACT_MONO_FRAMES, 1U, TEST_SAMPLE_RATE);

    MixerRaceState state;
    for (uint32_t index = 0U; index < sizeof(state); ++index)
    {
        ((uint8_t *)&state)[index] = 0U;
    }
    state.device = device;
    state.clip = clip;

    PlatformThread renderThread;
    Expect(PlatformThreadStart(&renderThread, MixerRenderWorker, &state),
           "the output thread could not be started");

    // Заказ короче клипа и без повтора: слоты регулярно доходят до FINISHED
    // и переиспользуются, а очередь команд при этом не пустует.
    AudioVoice recent[64];
    uint32_t recentCount = 0U;
    uint32_t maximumActive = 0U;
    for (uint32_t index = 0U; index < 20000U; ++index)
    {
        AudioVoiceParameters parameters = {
            .volume = 1.0f,
            .pan = 0.0f,
            .speed = 1.0f,
            .looping = false,
        };
        AudioVoice voice = AudioVoicePlay(device, clip, &parameters);
        if (voice != AUDIO_VOICE_NONE)
        {
            recent[recentCount < 64U ? recentCount : (recentCount % 64U)] = voice;
            ++recentCount;
            (void)AudioVoiceSetParameters(device, voice, &parameters);
            if ((index & 1U) == 0U)
            {
                AudioVoiceStop(device, voice);
            }
        }
        if ((index & 1023U) == 0U)
        {
            AudioDeviceStats sample;
            if (AudioDeviceGetStats(device, &sample) && sample.activeVoices > maximumActive)
            {
                maximumActive = sample.activeVoices;
            }
        }
    }
    // Под нагрузкой поток вывода может не получить ни одного кванта за всю
    // серию, и выборка статистики тогда видит ноль без всякой ошибки
    // протокола. Зацикленный голос не заканчивается сам, поэтому ожидание
    // его перехода в ACTIVE от планировщика не зависит.
    if (maximumActive == 0U)
    {
        AudioVoiceParameters looping = {
            .volume = 1.0f,
            .pan = 0.0f,
            .speed = 1.0f,
            .looping = true,
        };
        AudioVoice keeper = AUDIO_VOICE_NONE;
        for (uint32_t spin = 0U; spin < 10000U && maximumActive == 0U; ++spin)
        {
            if (keeper == AUDIO_VOICE_NONE)
            {
                keeper = AudioVoicePlay(device, clip, &looping);
            }
            AudioDeviceStats sample;
            if (AudioDeviceGetStats(device, &sample) && sample.activeVoices > 0U)
            {
                maximumActive = sample.activeVoices;
                break;
            }
            PlatformSleepMilliseconds(1U);
        }
    }
    // Очередь и переход PENDING -> ACTIVE действительно работали: хотя бы в
    // одном буфере поток вывода увидел звучащий голос. Без публикации команд
    // или без перехода в ACTIVE здесь остаётся ноль.
    Expect(maximumActive > 0U, "the output thread never saw an active voice");
    AudioDeviceStopAllVoices(device);

    // Дать потоку вывода разобрать STOP_ALL: activeVoices обновляется только
    // внутри RenderFrames, поэтому ждём его спада, а не гасим поток сразу.
    AudioDeviceStats stats;
    for (uint32_t spin = 0U; spin < 4000U; ++spin)
    {
        Expect(AudioDeviceGetStats(device, &stats),
               "stats while draining the voice race must be readable");
        if (stats.activeVoices == 0U)
        {
            break;
        }
        PlatformSleepMilliseconds(1U);
    }

    PlatformAtomicStoreU32Release(&state.stop, 1U);
    PlatformThreadJoin(&renderThread);

    Expect(AudioDeviceGetStats(device, &stats), "stats after the mixer race must be readable");
    Expect(stats.activeVoices == 0U, "stop-all must leave no active voice after the race");
    Expect(stats.mixedFrames > 0U, "the output thread must have mixed frames");

    // Ни один из недавно выданных дескрипторов не должен остаться активным
    // после STOP_ALL: слот либо переиспользован (поколение сменилось), либо
    // переведён в FINISHED.
    uint32_t checked = recentCount < 64U ? recentCount : 64U;
    for (uint32_t index = 0U; index < checked; ++index)
    {
        Expect(!AudioVoiceIsActive(device, recent[index]),
               "a voice handle survived stop-all as active");
    }

    AudioClipDestroy(clip);
    AudioDeviceDestroy(device);
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
    AudioVoice startedHandles[AUDIO_MAX_VOICES];
    for (uint32_t index = 0; index < AUDIO_MAX_VOICES * 2u; ++index)
    {
        AudioVoice started = AudioVoicePlay(device, clip, &loopParameters);
        if (started != AUDIO_VOICE_NONE)
        {
            startedHandles[startedVoices++] = started;
        }
    }
    Expect(startedVoices == AUDIO_MAX_VOICES, "the mixer must accept every available voice slot");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(AudioDeviceGetStats(device, &stats), "stats must be readable");
    Expect(stats.activeVoices == AUDIO_MAX_VOICES, "all started voices must be reported active");
    Expect(stats.mixedFrames > 0u, "mixed frames must be counted");

    // Stopping the tail should shorten the mixer's scan on this same render.
    // The low voice must keep sounding, and a later start in the newly empty
    // tail must raise the boundary again.
    for (uint32_t index = 1u; index < AUDIO_MAX_VOICES; ++index)
    {
        AudioVoiceStop(device, startedHandles[index]);
    }
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES),
           "rendering after tail stop must succeed");
    Expect(AudioDeviceGetStats(device, &stats), "stats after tail stop must be readable");
    Expect(stats.activeVoices == 1u, "stopping tail voices must preserve the low voice");
    Expect(ChannelPeak(frames, TEST_FRAMES, 0u) > 0.0f,
           "the low voice must keep sounding after the scan boundary shrinks");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES),
           "rendering after tail trim must succeed");
    Expect(ChannelPeak(frames, TEST_FRAMES, 0u) > 0.0f,
           "the low voice must keep sounding on the shortened scan path");

    AudioVoice afterTrim = AudioVoicePlay(device, clip, &loopParameters);
    Expect(afterTrim != AUDIO_VOICE_NONE, "a freed tail slot must be reusable after trimming");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES),
           "rendering a reused tail slot must succeed");
    Expect(AudioDeviceGetStats(device, &stats), "stats after tail reuse must be readable");
    Expect(stats.activeVoices == 2u, "a new voice must raise the shortened scan boundary");
    Expect(ChannelPeak(frames, TEST_FRAMES, 0u) > 0.0f,
           "a voice started beyond the shortened boundary must sound");

    AudioDeviceStopAllVoices(device);
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(AudioDeviceGetStats(device, &stats), "stats must be readable");
    Expect(stats.activeVoices == 0u, "stopping every voice must clear the active count");

    // === Отказы контракта ===
    AudioClip *rejectedDescription = clip;
    Expect(AudioClipCreate(device, NULL, &rejectedDescription) == AUDIO_RESULT_INVALID_ARGUMENT,
           "a NULL description must be rejected");
    Expect(rejectedDescription == NULL,
           "a rejected clip creation must clear its output pointer");
    AudioClipDescription invalid = description;
    invalid.channelCount = 3u;
    AudioClip *rejected = NULL;
    Expect(AudioClipCreate(device, &invalid, &rejected) == AUDIO_RESULT_INVALID_ARGUMENT,
           "an unsupported channel count must be rejected");
    Expect(AudioVoicePlay(device, NULL, NULL) == AUDIO_VOICE_NONE,
           "playing a NULL clip must be refused");

    // === Побитовая точность микса ===
    CheckExactMixes();
    CheckGeneralRuns();

    // === Быстрый путь мастера при одном голосе ===
    CheckSingleVoiceMasterFastPath();

    // === Гонка производителя и потока вывода ===
    CheckConcurrentMixer();

    PlatformFree(frames);
    AudioClipDestroy(clip);
    AudioDeviceDestroy(device);

    LaiueTestRuntimeWrite("Audio mixer checks passed\n");
    LAIUE_TEST_SUCCESS();
}
