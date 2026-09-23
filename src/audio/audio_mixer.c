// Микшер голосов. Поток вывода никогда не ждёт приложение: команды
// проходят через кольцо с одним потребителем, а состояние голоса после
// старта принадлежит только потоку вывода. Приложение может звать API из
// нескольких потоков — их между собой разводит producerLock, которого
// поток вывода не касается вовсе.

#include "audio/audio.h"
#include "audio/audio_backend.h"
#include "audio/audio_offscreen.h"
#include "audio/audio_output_service.h"
#include "math/scalar.h"
#include "platform/system.h"

#include <string.h>

#define AUDIO_MIX_CHANNELS 2u
#define AUDIO_COMMAND_CAPACITY 256u
#define AUDIO_DEFAULT_SAMPLE_RATE 48000u
// Скорость ограничивается, чтобы шаг чтения не превращал короткий клип в
// щелчок и не уводил интерполяцию за границы буфера.
#define AUDIO_MIN_SPEED 0.05f
#define AUDIO_MAX_SPEED 16.0f

static const LaiueAudioOutputServiceV1 *g_audioOutputService;

void AudioMixerSetOutputService(const LaiueAudioOutputServiceV1 *service)
{
    g_audioOutputService = service;
}

static float ClampFloat(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

struct AudioClip
{
    int16_t *samples;
    uint32_t frameCount;
    uint32_t channelCount;
    uint32_t sampleRate;

    // Клип помнит своё устройство, чтобы разрушение могло остановить
    // голоса, которые его читают, и отложить освобождение до момента,
    // когда поток вывода гарантированно на него больше не смотрит.
    AudioDevice *device;
    AudioClip *retireNext;
    uint64_t retireFrame;
};

typedef enum VoiceState
{
    VOICE_FREE = 0,
    VOICE_PENDING,    // слот занят приложением, команда ещё в пути
    VOICE_ACTIVE,     // голосом владеет поток вывода
    VOICE_FINISHED,   // отзвучал; слот переиспользуется приложением
} VoiceState;

typedef struct VoiceGains
{
    float left;
    float right;
} VoiceGains;

typedef struct VoiceSlot
{
    // Состояние — единственное поле, которое читают и пишут оба потока.
    volatile uint32_t state;
    uint32_t generation;

    // Частота клипа принадлежит потоку приложения: она нужна, чтобы
    // пересчитать шаг при смене скорости, а поле clip к тому моменту уже
    // принадлежит потоку вывода и читать его отсюда было бы гонкой.
    uint32_t clipSampleRate;

    // Ниже — собственность потока вывода после перехода в ACTIVE.
    const AudioClip *clip;
    double position;      // позиция чтения в кадрах исходного клипа
    double step;          // на сколько кадров исходника сдвигаться за кадр вывода
    VoiceGains gains;
    bool looping;
} VoiceSlot;

typedef enum CommandType
{
    COMMAND_START = 0,
    COMMAND_UPDATE,
    COMMAND_STOP,
    COMMAND_STOP_ALL,
} CommandType;

typedef struct AudioCommand
{
    uint32_t type;
    uint32_t slot;
    uint32_t generation;
    const AudioClip *clip;
    double step;
    VoiceGains gains;
    bool looping;
} AudioCommand;

struct AudioDevice
{
    // Offscreen is owned by the mixer. System output is an optional
    // provider and is deliberately held as an opaque handle/table pair so
    // this module has no platform-backend imports.
    AudioBackend *offscreenBackend;
    const LaiueAudioOutputServiceV1 *outputService;
    LaiueAudioOutputBackend *outputBackend;
    AudioBackendKind backendKind;
    uint32_t sampleRate;

    // Кольцо команд: пишет приложение, читает поток вывода.
    AudioCommand commands[AUDIO_COMMAND_CAPACITY];
    volatile uint32_t commandWrite;
    volatile uint32_t commandRead;

    // Разводит между собой потоки приложения. Поток вывода его не берёт.
    PlatformMutex producerLock;
    bool producerLockReady;

    VoiceSlot voices[AUDIO_MAX_VOICES];
    // Клип каждого слота глазами потока приложения: поле slot->clip
    // принадлежит потоку вывода, и читать его отсюда было бы гонкой.
    const AudioClip *slotClips[AUDIO_MAX_VOICES];
    AudioClip *retiredClips;
    uint32_t nextGeneration;

    volatile uint32_t masterVolumeBits;
    volatile uint32_t activeVoices;
    volatile int64_t droppedCommands;
    volatile int64_t mixedFrames;
};

static void ReleaseRetiredClips(AudioDevice *device, bool releaseEverything);

// Громкость живёт как биты float в атомарном слове: поток вывода читает
// её каждый буфер, приложение меняет в любой момент.
static float BitsToFloat(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t FloatToBits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static VoiceGains ComputeGains(float volume, float pan)
{
    // Панорама равной мощности: сумма квадратов усилений постоянна,
    // поэтому громкость не проваливается в центре.
    float clampedPan = ClampFloat(pan, -1.0f, 1.0f);
    float right = (clampedPan + 1.0f) * 0.5f;
    float left = 1.0f - right;
    float clampedVolume = ClampFloat(volume, 0.0f, 1.0f);

    VoiceGains gains;
    gains.left = ScalarSqrt(left) * clampedVolume;
    gains.right = ScalarSqrt(right) * clampedVolume;
    return gains;
}

static double ComputeStep(uint32_t clipSampleRate, uint32_t deviceSampleRate, float speed)
{
    float clampedSpeed = ClampFloat(speed, AUDIO_MIN_SPEED, AUDIO_MAX_SPEED);
    return ((double)clipSampleRate / (double)deviceSampleRate) * (double)clampedSpeed;
}

static void FillParameters(const AudioVoiceParameters *source, AudioVoiceParameters *target)
{
    if (source != NULL)
    {
        *target = *source;
        return;
    }
    target->volume = 1.0f;
    target->pan = 0.0f;
    target->speed = 1.0f;
    target->looping = false;
}

// === Кольцо команд ===

static bool PushCommand(AudioDevice *device, const AudioCommand *command)
{
    uint32_t write = device->commandWrite;
    uint32_t next = (write + 1u) % AUDIO_COMMAND_CAPACITY;
    if (next == PlatformAtomicLoadU32Acquire(&device->commandRead))
    {
        PlatformAtomicIncrementI64(&device->droppedCommands);
        return false;
    }
    device->commands[write] = *command;
    // Запись видна потоку вывода только после публикации индекса.
    PlatformAtomicStoreU32Release(&device->commandWrite, next);
    return true;
}

static void ApplyCommand(AudioDevice *device, const AudioCommand *command)
{
    if (command->type == COMMAND_STOP_ALL)
    {
        for (uint32_t index = 0; index < AUDIO_MAX_VOICES; ++index)
        {
            VoiceSlot *slot = &device->voices[index];
            if (PlatformAtomicLoadU32Acquire(&slot->state) != (uint32_t)VOICE_ACTIVE) continue;
            slot->clip = NULL;
            PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_FINISHED);
        }
        return;
    }

    if (command->slot >= AUDIO_MAX_VOICES) return;
    VoiceSlot *slot = &device->voices[command->slot];
    // Дескриптор мог устареть, пока команда шла: поколение это ловит.
    if (slot->generation != command->generation) return;

    switch ((CommandType)command->type)
    {
    case COMMAND_START:
        if (PlatformAtomicLoadU32Acquire(&slot->state) != (uint32_t)VOICE_PENDING) return;
        slot->clip = command->clip;
        slot->position = 0.0;
        slot->step = command->step;
        slot->gains = command->gains;
        slot->looping = command->looping;
        PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_ACTIVE);
        break;
    case COMMAND_UPDATE:
        if (PlatformAtomicLoadU32Acquire(&slot->state) != (uint32_t)VOICE_ACTIVE) return;
        slot->step = command->step;
        slot->gains = command->gains;
        slot->looping = command->looping;
        break;
    case COMMAND_STOP:
    {
        uint32_t state = PlatformAtomicLoadU32Acquire(&slot->state);
        if (state != (uint32_t)VOICE_ACTIVE && state != (uint32_t)VOICE_PENDING) return;
        slot->clip = NULL;
        PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_FINISHED);
        break;
    }
    default:
        break;
    }
}

static void DrainCommands(AudioDevice *device)
{
    uint32_t read = device->commandRead;
    uint32_t write = PlatformAtomicLoadU32Acquire(&device->commandWrite);
    while (read != write)
    {
        ApplyCommand(device, &device->commands[read]);
        read = (read + 1u) % AUDIO_COMMAND_CAPACITY;
    }
    PlatformAtomicStoreU32Release(&device->commandRead, read);
}

// === Смешивание ===

// Линейная интерполяция между соседними кадрами источника: без неё
// изменение скорости даёт слышимый ступенчатый шум.
static void SampleClip(const AudioClip *clip, double position, float *outLeft, float *outRight)
{
    uint32_t frame = (uint32_t)position;
    if (frame >= clip->frameCount)
    {
        *outLeft = 0.0f;
        *outRight = 0.0f;
        return;
    }
    uint32_t nextFrame = frame + 1u < clip->frameCount ? frame + 1u : frame;
    float fraction = (float)(position - (double)frame);

    const float scale = 1.0f / 32768.0f;
    if (clip->channelCount == 2u)
    {
        float leftA = (float)clip->samples[frame * 2u] * scale;
        float rightA = (float)clip->samples[frame * 2u + 1u] * scale;
        float leftB = (float)clip->samples[nextFrame * 2u] * scale;
        float rightB = (float)clip->samples[nextFrame * 2u + 1u] * scale;
        *outLeft = leftA + (leftB - leftA) * fraction;
        *outRight = rightA + (rightB - rightA) * fraction;
        return;
    }

    float monoA = (float)clip->samples[frame] * scale;
    float monoB = (float)clip->samples[nextFrame] * scale;
    float mono = monoA + (monoB - monoA) * fraction;
    *outLeft = mono;
    *outRight = mono;
}

// Смешивает одну выборку в готовые кадры. Тело совпадает с прежней общей
// ветвью (SampleClip плюс накопление), поэтому компилятор обязан выдать ту
// же последовательность операций. Не встраивается: иначе оптимизатор
// разворачивает конкретный цикл и меняет контракцию FMA на хвосте.
#if defined(_MSC_VER) && !defined(__clang__)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void MixSample(const AudioClip *clip, double position, float left, float right,
                      float *outFrame)
{
    float sampleLeft;
    float sampleRight;
    SampleClip(clip, position, &sampleLeft, &sampleRight);
    outFrame[0] += sampleLeft * left;
    outFrame[1] += sampleRight * right;
}

static void MixVoice(VoiceSlot *slot, float *frames, uint32_t frameCount)
{
    const AudioClip *clip = slot->clip;
    double position = slot->position;
    double step = slot->step;
    float left = slot->gains.left;
    float right = slot->gains.right;

    // Целый шаг (частота клипа совпала с частотой устройства при скорости 1):
    // дробная часть позиции в цикле тождественно равна нулю, поэтому
    // интерполяция вырождается в выборку одного кадра, а позиция остаётся
    // целой. Отрезок до границы клипа проходится без проверки в каждой
    // выборке. Арифметика выборки та же — (float)sample * (1/32768) — и
    // результат совпадает с общей ветвью побитово.
    if (step == 1.0 && position == (double)(uint32_t)position)
    {
        const int16_t *samples = clip->samples;
        const uint32_t clipFrames = clip->frameCount;
        const float scale = 1.0f / 32768.0f;
        uint32_t frame = (uint32_t)position;
        uint32_t index = 0u;
        if (clip->channelCount == 2u)
        {
            while (index < frameCount)
            {
                if (frame >= clipFrames)
                {
                    if (!slot->looping)
                    {
                        slot->clip = NULL;
                        PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_FINISHED);
                        slot->position = (double)frame;
                        return;
                    }
                    frame -= clipFrames;
                }
                uint32_t run = clipFrames - frame;
                uint32_t tail = frameCount - index;
                uint32_t count = run < tail ? run : tail;
                for (uint32_t sample = 0u; sample < count; ++sample)
                {
                    float channelLeft = (float)samples[frame * 2u] * scale;
                    float channelRight = (float)samples[frame * 2u + 1u] * scale;
                    frames[(index + sample) * 2u] += channelLeft * left;
                    frames[(index + sample) * 2u + 1u] += channelRight * right;
                    ++frame;
                }
                index += count;
            }
        }
        else
        {
            while (index < frameCount)
            {
                if (frame >= clipFrames)
                {
                    if (!slot->looping)
                    {
                        slot->clip = NULL;
                        PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_FINISHED);
                        slot->position = (double)frame;
                        return;
                    }
                    frame -= clipFrames;
                }
                uint32_t run = clipFrames - frame;
                uint32_t tail = frameCount - index;
                uint32_t count = run < tail ? run : tail;
                for (uint32_t sample = 0u; sample < count; ++sample)
                {
                    float mono = (float)samples[frame] * scale;
                    frames[(index + sample) * 2u] += mono * left;
                    frames[(index + sample) * 2u + 1u] += mono * right;
                    ++frame;
                }
                index += count;
            }
        }
        slot->position = (double)frame;
        return;
    }

    // Общая ветвь: дробный шаг, линейная интерполяция. Проверка границы
    // клипа и ветвление по числу каналов вынесены из внутреннего цикла:
    // отрезок до ближайшей границы (конец клипа или буфера) считается из
    // позиции и шага один раз, а арифметика выборки остаётся прежней —
    // position += step подряд и (float)(position - frame), — поэтому выход
    // совпадает с прежней ветвью побитово.
    const uint32_t clipFrames = clip->frameCount;
    const double clipFramesD = (double)clipFrames;
    uint32_t index = 0u;
    while (index < frameCount)
    {
        if (position >= clipFramesD)
        {
            if (!slot->looping)
            {
                slot->clip = NULL;
                PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_FINISHED);
                slot->position = position;
                return;
            }
            // Повтор без разрыва: остаток шага переносится в начало.
            position -= clipFramesD;
            if (position < 0.0) position = 0.0;
            if (position >= clipFramesD)
            {
                // Клип короче шага: одного вычитания мало, и выборка молчит.
                // Прежняя общая ветвь в этом случае отдаёт тишину, а позиция
                // растёт дальше — так же поступаем и здесь.
                position += step;
                ++index;
                continue;
            }
        }

        // Число выборок до границы клипа. Точное деление может ошибиться на
        // единицу из-за накопленного округления, поэтому берётся запас.
        uint32_t run = frameCount - index;
        double ratio = (clipFramesD - position) / step;
        if (ratio < (double)run) run = (uint32_t)ratio;
        if (run > 2u) run -= 2u;
        else run = 0u;
        if (run == 0u) run = 1u;

        // Основной отрезок проходится без проверки границы и кратен четырём:
        // развёрнутый хвост компилятора, где контракция FMA иначе пропадает,
        // тогда не исполняется. Меньший остаток идёт через MixSample.
        uint32_t sample = 0u;
        if (run >= 4u)
        {
            uint32_t bulk = run & ~3u;
            if (clip->channelCount == 2u)
            {
                const int16_t *samples = clip->samples;
                for (; sample < bulk; ++sample)
                {
                    uint32_t frame = (uint32_t)position;
                    uint32_t nextFrame = frame + 1u < clipFrames ? frame + 1u : frame;
                    float fraction = (float)(position - (double)frame);
                    float leftA = (float)samples[frame * 2u] * (1.0f / 32768.0f);
                    float rightA = (float)samples[frame * 2u + 1u] * (1.0f / 32768.0f);
                    float leftB = (float)samples[nextFrame * 2u] * (1.0f / 32768.0f);
                    float rightB = (float)samples[nextFrame * 2u + 1u] * (1.0f / 32768.0f);
                    float sampleLeft = leftA + (leftB - leftA) * fraction;
                    float sampleRight = rightA + (rightB - rightA) * fraction;
                    frames[(index + sample) * 2u] += sampleLeft * left;
                    frames[(index + sample) * 2u + 1u] += sampleRight * right;
                    position += step;
                }
            }
            else
            {
                const int16_t *samples = clip->samples;
                for (; sample < bulk; ++sample)
                {
                    uint32_t frame = (uint32_t)position;
                    uint32_t nextFrame = frame + 1u < clipFrames ? frame + 1u : frame;
                    float fraction = (float)(position - (double)frame);
                    float monoA = (float)samples[frame] * (1.0f / 32768.0f);
                    float monoB = (float)samples[nextFrame] * (1.0f / 32768.0f);
                    float sampleMono = monoA + (monoB - monoA) * fraction;
                    frames[(index + sample) * 2u] += sampleMono * left;
                    frames[(index + sample) * 2u + 1u] += sampleMono * right;
                    position += step;
                }
            }
        }
        for (; sample < run; ++sample)
        {
            MixSample(clip, position, left, right, &frames[(index + sample) * 2u]);
            position += step;
        }
        index += run;
    }
    slot->position = position;
}

static void RenderFrames(void *context, float *frames, uint32_t frameCount)
{
    AudioDevice *device = (AudioDevice *)context;
    memset(frames, 0, (size_t)frameCount * AUDIO_MIX_CHANNELS * sizeof(float));

    DrainCommands(device);

    uint32_t active = 0u;
    for (uint32_t index = 0; index < AUDIO_MAX_VOICES; ++index)
    {
        VoiceSlot *slot = &device->voices[index];
        if (PlatformAtomicLoadU32Acquire(&slot->state) != (uint32_t)VOICE_ACTIVE) continue;
        if (slot->clip == NULL) continue;
        MixVoice(slot, frames, frameCount);
        if (PlatformAtomicLoadU32Acquire(&slot->state) == (uint32_t)VOICE_ACTIVE) ++active;
    }
    PlatformAtomicStoreU32Release(&device->activeVoices, active);

    float master = BitsToFloat(PlatformAtomicLoadU32Acquire(&device->masterVolumeBits));
    uint32_t sampleCount = frameCount * AUDIO_MIX_CHANNELS;
    for (uint32_t index = 0; index < sampleCount; ++index)
    {
        // Мягкое ограничение отсутствует намеренно: сумма голосов
        // обрезается по диапазону, а решение о запасе громкости
        // принадлежит приложению, как и остальная политика микса.
        frames[index] = ClampFloat(frames[index] * master, -1.0f, 1.0f);
    }
    PlatformAtomicAddI64(&device->mixedFrames, (int64_t)frameCount);
}

// === Устройство ===

AudioResult AudioDeviceCreate(const AudioDeviceConfiguration *configuration,
                              AudioDevice **outDevice)
{
    if (outDevice == NULL) return AUDIO_RESULT_INVALID_ARGUMENT;
    *outDevice = NULL;

    AudioDeviceConfiguration resolved = {
        .backend = AUDIO_BACKEND_SYSTEM,
        .sampleRate = 0u,
        .frameCountHint = 0u,
        .masterVolume = 1.0f,
    };
    if (configuration != NULL) resolved = *configuration;
    if (resolved.backend != AUDIO_BACKEND_SYSTEM &&
        resolved.backend != AUDIO_BACKEND_OFFSCREEN)
        return AUDIO_RESULT_INVALID_ARGUMENT;

    AudioDevice *device = PlatformAllocate(sizeof(*device), true);
    if (device == NULL) return AUDIO_RESULT_OUT_OF_MEMORY;

    if (!PlatformMutexInitialize(&device->producerLock))
    {
        PlatformFree(device);
        return AUDIO_RESULT_PLATFORM_INITIALIZATION_FAILED;
    }
    device->producerLockReady = true;
    device->nextGeneration = 1u;
    device->masterVolumeBits = FloatToBits(ClampFloat(resolved.masterVolume, 0.0f, 1.0f));

    AudioBackendDescription description = {
        .sampleRate = resolved.sampleRate,
        .frameCountHint = resolved.frameCountHint,
        .render = RenderFrames,
        .context = device,
    };
    device->backendKind = resolved.backend;
    bool created = false;
    if (resolved.backend == AUDIO_BACKEND_OFFSCREEN)
    {
        created = AudioOffscreenBackendCreate(&description, &device->offscreenBackend);
    }
    else if (g_audioOutputService != NULL && g_audioOutputService->create != NULL)
    {
        LaiueAudioOutputDescription outputDescription = {
            .sampleRate = description.sampleRate,
            .frameCountHint = description.frameCountHint,
            .render = description.render,
            .context = description.context,
        };
        created = g_audioOutputService->create(&outputDescription, &device->outputBackend) != 0u &&
                  device->outputBackend != NULL;
        if (created)
        {
            device->outputService = g_audioOutputService;
            if (device->outputService->sampleRate == NULL ||
                device->outputService->channelCount == NULL ||
                device->outputService->bufferFrameCount == NULL ||
                device->outputService->underrunCount == NULL)
            {
                if (device->outputService->destroy != NULL)
                    device->outputService->destroy(device->outputBackend);
                device->outputService = NULL;
                device->outputBackend = NULL;
                created = false;
            }
        }
    }
    if (!created)
    {
        AudioDeviceDestroy(device);
        return AUDIO_RESULT_BACKEND_INITIALIZATION_FAILED;
    }

    if (resolved.backend == AUDIO_BACKEND_OFFSCREEN)
    {
        device->sampleRate = device->offscreenBackend->sampleRate;
    }
    else
    {
        device->sampleRate = device->outputService->sampleRate(device->outputBackend);
        uint32_t channelCount = device->outputService->channelCount(device->outputBackend);
        uint32_t bufferFrameCount =
            device->outputService->bufferFrameCount(device->outputBackend);
        if (device->sampleRate == 0u || channelCount == 0u || bufferFrameCount == 0u)
        {
            device->outputService->destroy(device->outputBackend);
            device->outputService = NULL;
            device->outputBackend = NULL;
            AudioDeviceDestroy(device);
            return AUDIO_RESULT_BACKEND_INITIALIZATION_FAILED;
        }
    }
    *outDevice = device;
    return AUDIO_RESULT_OK;
}

void AudioDeviceDestroy(AudioDevice *device)
{
    if (device == NULL) return;
    // Бэкенд разрушается первым: после этого поток вывода не работает и
    // остальное состояние можно освобождать без синхронизации.
    if (device->offscreenBackend != NULL)
        device->offscreenBackend->vtable->destroy(device->offscreenBackend);
    if (device->outputBackend != NULL && device->outputService != NULL &&
        device->outputService->destroy != NULL)
        device->outputService->destroy(device->outputBackend);
    // Поток вывода остановлен, поэтому отложенные клипы можно освободить
    // безусловно: смотреть на них больше некому.
    ReleaseRetiredClips(device, true);
    if (device->producerLockReady) PlatformMutexDestroy(&device->producerLock);
    PlatformFree(device);
}

void AudioDeviceSetMasterVolume(AudioDevice *device, float volume)
{
    if (device == NULL) return;
    PlatformAtomicStoreU32Release(&device->masterVolumeBits,
                                  FloatToBits(ClampFloat(volume, 0.0f, 1.0f)));
}

float AudioDeviceGetMasterVolume(const AudioDevice *device)
{
    if (device == NULL) return 0.0f;
    return BitsToFloat(PlatformAtomicLoadU32Acquire(&device->masterVolumeBits));
}

bool AudioDeviceGetStats(const AudioDevice *device, AudioDeviceStats *outStats)
{
    if (device == NULL || outStats == NULL) return false;
    outStats->sampleRate = device->sampleRate;
    outStats->channelCount = AUDIO_MIX_CHANNELS;
    outStats->bufferFrameCount = device->backendKind == AUDIO_BACKEND_OFFSCREEN
                                     ? (device->offscreenBackend != NULL
                                            ? device->offscreenBackend->bufferFrameCount
                                            : 0u)
                                     : (device->outputBackend != NULL &&
                                                device->outputService != NULL
                                            ? device->outputService->bufferFrameCount(
                                                  device->outputBackend)
                                            : 0u);
    outStats->activeVoices = PlatformAtomicLoadU32Acquire(&device->activeVoices);
    outStats->droppedCommands = (uint64_t)PlatformAtomicLoadI64(&device->droppedCommands);
    outStats->underruns = device->backendKind == AUDIO_BACKEND_OFFSCREEN
                              ? (device->offscreenBackend != NULL
                                     ? device->offscreenBackend->vtable->underrunCount(
                                           device->offscreenBackend)
                                     : 0u)
                              : (device->outputBackend != NULL && device->outputService != NULL
                                     ? device->outputService->underrunCount(device->outputBackend)
                                     : 0u);
    outStats->mixedFrames = (uint64_t)PlatformAtomicLoadI64(&device->mixedFrames);
    return true;
}

// === Клипы ===

AudioResult AudioClipCreate(AudioDevice *device, const AudioClipDescription *description,
                            AudioClip **outClip)
{
    if (outClip == NULL) return AUDIO_RESULT_INVALID_ARGUMENT;
    *outClip = NULL;
    if (device == NULL || description == NULL || description->samples == NULL
        || description->frameCount == 0u || description->sampleRate == 0u
        || (description->channelCount != 1u && description->channelCount != 2u))
        return AUDIO_RESULT_INVALID_ARGUMENT;

    uint32_t sampleCount = description->frameCount * description->channelCount;
    if (sampleCount / description->channelCount != description->frameCount)
        return AUDIO_RESULT_INVALID_ARGUMENT;

    AudioClip *clip = PlatformAllocate(sizeof(*clip), true);
    if (clip == NULL) return AUDIO_RESULT_OUT_OF_MEMORY;

    clip->samples = PlatformAllocate((size_t)sampleCount * sizeof(int16_t), false);
    if (clip->samples == NULL)
    {
        PlatformFree(clip);
        return AUDIO_RESULT_OUT_OF_MEMORY;
    }
    memcpy(clip->samples, description->samples, (size_t)sampleCount * sizeof(int16_t));
    clip->frameCount = description->frameCount;
    clip->channelCount = description->channelCount;
    clip->sampleRate = description->sampleRate;
    clip->device = device;

    *outClip = clip;
    return AUDIO_RESULT_OK;
}

// Освобождает клипы, выведенные из игры настолько давно, что поток
// вывода успел с тех пор подготовить хотя бы один буфер: к этому моменту
// он разобрал команды остановки и на клип больше не смотрит.
static void ReleaseRetiredClips(AudioDevice *device, bool releaseEverything)
{
    AudioClip **link = &device->retiredClips;
    uint64_t mixedFrames = (uint64_t)PlatformAtomicLoadI64(&device->mixedFrames);
    while (*link != NULL)
    {
        AudioClip *clip = *link;
        if (!releaseEverything && mixedFrames < clip->retireFrame)
        {
            link = &clip->retireNext;
            continue;
        }
        *link = clip->retireNext;
        if (clip->samples != NULL) PlatformFree(clip->samples);
        PlatformFree(clip);
    }
}

void AudioClipDestroy(AudioClip *clip)
{
    if (clip == NULL) return;
    AudioDevice *device = clip->device;
    if (device == NULL)
    {
        if (clip->samples != NULL) PlatformFree(clip->samples);
        PlatformFree(clip);
        return;
    }

    PlatformMutexLock(&device->producerLock);
    // Голоса этого клипа останавливаются сами: контракт запрещает рушить
    // звучащий клип, но падать на нарушении библиотека не должна.
    bool allStopsQueued = true;
    for (uint32_t index = 0; index < AUDIO_MAX_VOICES; ++index)
    {
        if (device->slotClips[index] != clip) continue;
        device->slotClips[index] = NULL;

        VoiceSlot *slot = &device->voices[index];
        uint32_t state = PlatformAtomicLoadU32Acquire(&slot->state);
        if (state != (uint32_t)VOICE_ACTIVE && state != (uint32_t)VOICE_PENDING) continue;
        AudioCommand command = {
            .type = COMMAND_STOP,
            .slot = index,
            .generation = slot->generation,
        };
        if (!PushCommand(device, &command)) allStopsQueued = false;
    }

    // Кольцо переполнено — команда остановки не дошла, и голос может ещё
    // читать клип. Тогда память освобождается только вместе с
    // устройством: течь лучше, чем читать освобождённое.
    clip->retireFrame =
        allStopsQueued ? (uint64_t)PlatformAtomicLoadI64(&device->mixedFrames) + 1u : UINT64_MAX;
    clip->retireNext = device->retiredClips;
    device->retiredClips = clip;
    ReleaseRetiredClips(device, false);
    PlatformMutexUnlock(&device->producerLock);
}

double AudioClipDurationSeconds(const AudioClip *clip)
{
    if (clip == NULL || clip->sampleRate == 0u) return 0.0;
    return (double)clip->frameCount / (double)clip->sampleRate;
}

// === Голоса ===

static AudioVoice MakeVoiceHandle(uint32_t slot, uint32_t generation)
{
    // Слот в младших битах, поколение в старших: устаревший дескриптор
    // указывает на существующий слот, но не совпадает поколением.
    return (generation << 8) | (slot & 0xffu);
}

static bool SplitVoiceHandle(AudioVoice voice, uint32_t *outSlot, uint32_t *outGeneration)
{
    if (voice == AUDIO_VOICE_NONE) return false;
    *outSlot = voice & 0xffu;
    *outGeneration = voice >> 8;
    return *outSlot < AUDIO_MAX_VOICES;
}

_Static_assert(AUDIO_MAX_VOICES <= 256u, "the voice handle packs the slot into eight bits");

AudioVoice AudioVoicePlay(AudioDevice *device, const AudioClip *clip,
                          const AudioVoiceParameters *parameters)
{
    if (device == NULL || clip == NULL) return AUDIO_VOICE_NONE;

    AudioVoiceParameters resolved;
    FillParameters(parameters, &resolved);

    AudioCommand command = {
        .type = COMMAND_START,
        .clip = clip,
        .step = ComputeStep(clip->sampleRate, device->sampleRate, resolved.speed),
        .gains = ComputeGains(resolved.volume, resolved.pan),
        .looping = resolved.looping,
    };

    PlatformMutexLock(&device->producerLock);
    AudioVoice handle = AUDIO_VOICE_NONE;
    for (uint32_t index = 0; index < AUDIO_MAX_VOICES; ++index)
    {
        VoiceSlot *slot = &device->voices[index];
        uint32_t state = PlatformAtomicLoadU32Acquire(&slot->state);
        if (state != (uint32_t)VOICE_FREE && state != (uint32_t)VOICE_FINISHED) continue;

        // Поколение растёт при каждом переиспользовании, поэтому
        // дескриптор прежнего владельца слота становится недействителен.
        slot->clipSampleRate = clip->sampleRate;
        device->slotClips[index] = clip;
        slot->generation = device->nextGeneration++;
        if (device->nextGeneration == 0u) device->nextGeneration = 1u;
        command.slot = index;
        command.generation = slot->generation;
        PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_PENDING);

        if (!PushCommand(device, &command))
        {
            device->slotClips[index] = NULL;
            PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_FREE);
            break;
        }
        handle = MakeVoiceHandle(index, slot->generation);
        break;
    }
    PlatformMutexUnlock(&device->producerLock);
    return handle;
}

bool AudioVoiceSetParameters(AudioDevice *device, AudioVoice voice,
                             const AudioVoiceParameters *parameters)
{
    uint32_t slotIndex = 0u;
    uint32_t generation = 0u;
    if (device == NULL || !SplitVoiceHandle(voice, &slotIndex, &generation)) return false;

    AudioVoiceParameters resolved;
    FillParameters(parameters, &resolved);

    PlatformMutexLock(&device->producerLock);
    VoiceSlot *slot = &device->voices[slotIndex];
    bool accepted = false;
    if (slot->generation == generation)
    {
        uint32_t state = PlatformAtomicLoadU32Acquire(&slot->state);
        if (state == (uint32_t)VOICE_ACTIVE || state == (uint32_t)VOICE_PENDING)
        {
            AudioCommand command = {
                .type = COMMAND_UPDATE,
                .slot = slotIndex,
                .generation = generation,
                .clip = NULL,
                .step = ComputeStep(slot->clipSampleRate, device->sampleRate, resolved.speed),
                .gains = ComputeGains(resolved.volume, resolved.pan),
                .looping = resolved.looping,
            };
            accepted = PushCommand(device, &command);
        }
    }
    PlatformMutexUnlock(&device->producerLock);
    return accepted;
}

void AudioVoiceStop(AudioDevice *device, AudioVoice voice)
{
    uint32_t slotIndex = 0u;
    uint32_t generation = 0u;
    if (device == NULL || !SplitVoiceHandle(voice, &slotIndex, &generation)) return;

    PlatformMutexLock(&device->producerLock);
    if (device->voices[slotIndex].generation == generation)
    {
        AudioCommand command = {
            .type = COMMAND_STOP,
            .slot = slotIndex,
            .generation = generation,
        };
        PushCommand(device, &command);
    }
    PlatformMutexUnlock(&device->producerLock);
}

void AudioDeviceStopAllVoices(AudioDevice *device)
{
    if (device == NULL) return;
    AudioCommand command = { .type = COMMAND_STOP_ALL };
    PlatformMutexLock(&device->producerLock);
    PushCommand(device, &command);
    for (uint32_t index = 0; index < AUDIO_MAX_VOICES; ++index) device->slotClips[index] = NULL;
    PlatformMutexUnlock(&device->producerLock);
}

bool AudioVoiceIsActive(const AudioDevice *device, AudioVoice voice)
{
    uint32_t slotIndex = 0u;
    uint32_t generation = 0u;
    if (device == NULL || !SplitVoiceHandle(voice, &slotIndex, &generation)) return false;

    const VoiceSlot *slot = &device->voices[slotIndex];
    if (slot->generation != generation) return false;
    uint32_t state = PlatformAtomicLoadU32Acquire(&slot->state);
    return state == (uint32_t)VOICE_ACTIVE || state == (uint32_t)VOICE_PENDING;
}

// === Offscreen-доступ ===

bool AudioDeviceRenderFrames(AudioDevice *device, float *outFrames, uint32_t frameCount)
{
    if (device == NULL || outFrames == NULL || frameCount == 0u) return false;
    // У системного бэкенда кадры готовит его собственный поток вывода;
    // вызов отсюда наложился бы на него и испортил и микс, и статистику.
    if (device->offscreenBackend == NULL || device->backendKind != AUDIO_BACKEND_OFFSCREEN)
        return false;
    RenderFrames(device, outFrames, frameCount);
    return true;
}
