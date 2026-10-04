// Микшер голосов. Поток вывода никогда не ждёт приложение: команды
// проходят через кольцо с одним потребителем, а состояние голоса после
// старта принадлежит только потоку вывода. Приложение может звать API из
// нескольких потоков — их между собой разводит producerLock, которого
// поток вывода не касается вовсе.

#include "audio/audio.h"
#include "audio/audio_stream_internal.h"
#include "audio/audio_backend.h"
#include "audio/audio_offscreen.h"
#include "audio/audio_output_service.h"
#include "math/scalar.h"
#include "platform/system.h"

#include <string.h>
#include <stdbool.h>
#include <float.h>

// Быстрый путь целочисленного шага разворачивается векторно, когда профиль
// сборки даёт AVX2 и слитное умножение-сложение: скалярный код компилятора
// контрактит `frames += sample * gain` в FMA, и вектор обязан повторить ту же
// одну округлёнку на дорожку, иначе микс перестанет совпадать побитово.
// Прочие профили (SSE2, ARM NEON, внешние) остаются на прежнем скалярном
// пути без изменения арифметики.
#if defined(__AVX2__) && (defined(_MSC_VER) || defined(__FMA__))
#include <immintrin.h>
#define AUDIO_MIX_AVX2_FMA 1
#endif

#define AUDIO_MIX_CHANNELS 2u
#define AUDIO_COMMAND_CAPACITY 256u
#define AUDIO_DEFAULT_SAMPLE_RATE 48000u
// Скорость ограничивается, чтобы шаг чтения не превращал короткий клип в
// щелчок и не уводил интерполяцию за границы буфера.
#define AUDIO_MIN_SPEED 0.05f
#define AUDIO_MAX_SPEED 16.0f

static const LaiueAudioOutputServiceV1 *g_audioOutputService;
static const void *g_audioOutputOwner;

void AudioMixerSetOutputService(const LaiueAudioOutputServiceV1 *service)
{
    if (g_audioOutputOwner != NULL)
    {
        if (service == g_audioOutputService)
            return;
        return;
    }
    g_audioOutputService = service;
}

bool AudioMixerTryAcquireOutputService(const void *owner,
                                       const LaiueAudioOutputServiceV1 *service)
{
    if (owner == NULL)
        return false;
    if (g_audioOutputOwner != NULL && g_audioOutputOwner != owner)
        return false;
    g_audioOutputOwner = owner;
    g_audioOutputService = service;
    return true;
}

void AudioMixerReleaseOutputService(const void *owner)
{
    if (owner != NULL && g_audioOutputOwner == owner)
    {
        g_audioOutputOwner = NULL;
        g_audioOutputService = NULL;
    }
}

static float ClampFloat(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

// Общая громкость и ограничение диапазона по всему буферу. Тело совпадает с
// прежним встроенным циклом RenderFrames: компилятор по-прежнему сам его
// векторизует. Отдельная функция нужна только чтобы вызывать проход по
// условию — когда он заведомо ничего не меняет, его можно пропустить.
static void ApplyMasterGain(float *frames, uint32_t sampleCount, float master)
{
    // Мягкое ограничение отсутствует намеренно: сумма голосов обрезается по
    // диапазону, а решение о запасе громкости принадлежит приложению.
    for (uint32_t index = 0; index < sampleCount; ++index)
    {
        frames[index] = ClampFloat(frames[index] * master, -1.0f, 1.0f);
    }
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
    AudioStream *stream;
    uint32_t streamEpoch;
    double streamOffset;
    bool paused;
    volatile uint32_t positionSequence;
    volatile uint32_t positionLow;
    volatile uint32_t positionHigh;
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
    COMMAND_PAUSE,
    COMMAND_SEEK,
} CommandType;

typedef struct AudioCommand
{
    uint32_t type;
    uint32_t slot;
    uint32_t generation;
    const AudioClip *clip;
    AudioStream *stream;
    uint32_t streamEpoch;
    double position;
    bool paused;
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
    volatile int64_t commandIssued;
    volatile int64_t commandApplied;

    // Разводит между собой потоки приложения. Поток вывода его не берёт.
    PlatformMutex producerLock;
    bool producerLockReady;

    VoiceSlot voices[AUDIO_MAX_VOICES];
    // Клип каждого слота глазами потока приложения: поле slot->clip
    // принадлежит потоку вывода, и читать его отсюда было бы гонкой.
    const AudioClip *slotClips[AUDIO_MAX_VOICES];
    AudioClip *retiredClips;
    AudioStream *streams;
    uint32_t nextGeneration;

    // Верхняя граница обхода слотов. Слоты выдаются с младших индексов,
    // поэтому число одновременно звучавших голосов ограничивает и размах.
    // Поле принадлежит только потоку вывода: ApplyCommand поднимает его при
    // COMMAND_START, а RenderFrames опускает до последнего активного слота.
    uint32_t voiceScanLimit;

    volatile uint32_t masterVolumeBits;
    volatile uint32_t activeVoices;
    volatile int64_t droppedCommands;
    volatile int64_t mixedFrames;
};

static void ReleaseRetiredClips(AudioDevice *device, bool releaseEverything);
static void ReleaseRetiredStreams(AudioDevice *device, bool releaseEverything);

static void PublishPosition(VoiceSlot *slot)
{
    union
    {
        double value;
        uint64_t bits;
    } position;
    position.value = slot->position;
    PlatformAtomicIncrementU32(&slot->positionSequence);
    PlatformAtomicStoreU32Release(&slot->positionLow, (uint32_t)position.bits);
    PlatformAtomicStoreU32Release(&slot->positionHigh, (uint32_t)(position.bits >> 32));
    PlatformAtomicIncrementU32(&slot->positionSequence);
}

static void DiscardOldStreamPackets(AudioStream *stream, uint32_t epoch)
{
    uint32_t read = stream->read;
    uint32_t write = PlatformAtomicLoadU32Acquire(&stream->write);
    while (read != write &&
           (int32_t)(stream->packets[read % AUDIO_STREAM_PACKET_COUNT].epoch - epoch) < 0)
        ++read;
    PlatformAtomicStoreU32Release(&stream->read, read);
}

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
    /* NaN must not reach the sample-index conversions. */
    if (!(speed >= AUDIO_MIN_SPEED))
        speed = AUDIO_MIN_SPEED;
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
    PlatformAtomicIncrementI64(&device->commandIssued);
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
            slot->stream = NULL;
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
        slot->stream = command->stream;
        slot->streamEpoch = command->streamEpoch;
        slot->streamOffset = 0.0;
        slot->paused = false;
        if (slot->stream != NULL)
            DiscardOldStreamPackets(slot->stream, slot->streamEpoch);
        slot->position = 0.0;
        PublishPosition(slot);
        slot->step = command->step;
        slot->gains = command->gains;
        slot->looping = command->looping;
        PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_ACTIVE);
        // Голос занял индекс не ниже любого из ранее звучавших: обход обязан
        // дотянуться до него и в следующих буферах.
        if (command->slot >= device->voiceScanLimit)
            device->voiceScanLimit = command->slot + 1u;
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
        slot->stream = NULL;
        PlatformAtomicStoreU32Release(&slot->state, (uint32_t)VOICE_FINISHED);
        break;
    }
    case COMMAND_PAUSE:
        if (PlatformAtomicLoadU32Acquire(&slot->state) == VOICE_ACTIVE)
            slot->paused = command->paused;
        break;
    case COMMAND_SEEK:
        if (PlatformAtomicLoadU32Acquire(&slot->state) != VOICE_ACTIVE)
            break;
        slot->position = command->position;
        slot->streamOffset = 0.0;
        slot->streamEpoch = command->streamEpoch;
        if (slot->stream != NULL)
            DiscardOldStreamPackets(slot->stream, slot->streamEpoch);
        PublishPosition(slot);
        break;
    default:
        break;
    }
}

// Возвращает true, если из кольца была разобрана хотя бы одна команда:
// вызывающий использует это, чтобы понять, мог ли появиться активный голос.
static bool DrainCommands(AudioDevice *device)
{
    uint32_t read = device->commandRead;
    uint32_t write = PlatformAtomicLoadU32Acquire(&device->commandWrite);
    if (read == write) return false;
    uint32_t applied = 0u;
    while (read != write)
    {
        ++applied;
        ApplyCommand(device, &device->commands[read]);
        read = (read + 1u) % AUDIO_COMMAND_CAPACITY;
    }
    PlatformAtomicStoreU32Release(&device->commandRead, read);
    PlatformAtomicAddI64(&device->commandApplied, applied);
    return true;
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

// Множитель выборки 1/32768 — степень двойки, поэтому на целом шаге его можно
// вынести из цикла в усиление голоса и сэкономить умножение на выборку. Вынос
// точен для конечного усиления, когда оно не меньше FLT_MIN * 2^15 (либо
// равно нулю); положительная бесконечность тоже сохраняет результат. Тогда
// мантисса не теряется, и
// `(float)sample * (1/32768) * gain` считается с тем же единственным
// округлением, что и прежняя последовательность «умножить выборку на 2^-15,
// затем FMA с усилением». Денормальное произведение выносить нельзя: такой
// редкий случай остаётся в общей ветви, которая даёт те же биты.
static inline bool GainsFoldExact(float left, float right)
{
    const float minimumGain = FLT_MIN * 32768.0f;
    return (left == 0.0f || left >= minimumGain)
           && (right == 0.0f || right >= minimumGain);
}

static void MixVoice(VoiceSlot *slot, float *frames, uint32_t frameCount)
{
    const AudioClip *clip = slot->clip;
    double position = slot->position;
    double step = slot->step;
    float left = slot->gains.left;
    float right = slot->gains.right;

    // При точном целом шаге позиция целая, поэтому интерполяция всегда
    // выбирает первый кадр. Очень короткие клипы остаются на общей ветви.
    if (step > 1.0 && step <= (double)clip->frameCount &&
        step == (double)(uint32_t)step && position <= (double)UINT32_MAX &&
        position == (double)(uint32_t)position && GainsFoldExact(left, right))
    {
        const uint32_t stepFrames = (uint32_t)step;
        const uint32_t clipFrames = clip->frameCount;
        const float scaledLeft = left * (1.0f / 32768.0f);
        const float scaledRight = right * (1.0f / 32768.0f);
        const int16_t *samples = clip->samples;
        uint64_t frame = (uint32_t)position;
        uint32_t index = 0u;

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

            uint32_t run = 1u + (clipFrames - 1u - (uint32_t)frame) / stepFrames;
            uint32_t count = frameCount - index;
            if (run < count) count = run;
            if (clip->channelCount == 1u)
            {
                for (uint32_t sample = 0u; sample < count; ++sample)
                {
                    float mono = (float)samples[(size_t)frame];
                    frames[(index + sample) * 2u] += mono * scaledLeft;
                    frames[(index + sample) * 2u + 1u] += mono * scaledRight;
                    frame += stepFrames;
                }
            }
            else
            {
                for (uint32_t sample = 0u; sample < count; ++sample)
                {
                    float channelLeft = (float)samples[(size_t)frame * 2u];
                    float channelRight = (float)samples[(size_t)frame * 2u + 1u];
                    frames[(index + sample) * 2u] += channelLeft * scaledLeft;
                    frames[(index + sample) * 2u + 1u] += channelRight * scaledRight;
                    frame += stepFrames;
                }
            }
            index += count;
        }
        slot->position = (double)frame;
        return;
    }

    // Целый шаг (частота клипа совпала с частотой устройства при скорости 1):
    // дробная часть позиции в цикле тождественно равна нулю, поэтому
    // интерполяция вырождается в выборку одного кадра, а позиция остаётся
    // целой. Отрезок до границы клипа проходится без проверки в каждой
    // выборке. Усиление уже содержит множитель 2^-15 (см. GainsFoldExact),
    // поэтому умножение на выборку в цикле не нужно, а результат совпадает с
    // общей ветвью побитово.
    if (step == 1.0 && position == (double)(uint32_t)position
        && GainsFoldExact(left, right))
    {
        const float scale = 1.0f / 32768.0f;
        const float scaledLeft = left * scale;
        const float scaledRight = right * scale;
        const int16_t *samples = clip->samples;
        const uint32_t clipFrames = clip->frameCount;
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
                uint32_t sample = 0u;
#if defined(AUDIO_MIX_AVX2_FMA)
                // Восемь float за итерацию — четыре стереокадра. Порядок
                // операций на дорожку тот же, что и в скалярной ветви:
                // int16->float и FMA с усилением, содержащим 2^-15, — поэтому
                // биты выхода совпадают.
                const __m256 gainVector = _mm256_setr_ps(
                    scaledLeft, scaledRight, scaledLeft, scaledRight,
                    scaledLeft, scaledRight, scaledLeft, scaledRight);
                for (; sample + 4u <= count; sample += 4u)
                {
                    __m128i packed = _mm_loadu_si128((const __m128i *)(samples + frame * 2u));
                    __m256 converted = _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(packed));
                    float *destination = frames + (index + sample) * 2u;
                    __m256 mix =
                        _mm256_fmadd_ps(gainVector, converted, _mm256_loadu_ps(destination));
                    _mm256_storeu_ps(destination, mix);
                    frame += 4u;
                }
#endif
                for (; sample < count; ++sample)
                {
                    float channelLeft = (float)samples[frame * 2u];
                    float channelRight = (float)samples[frame * 2u + 1u];
                    frames[(index + sample) * 2u] += channelLeft * scaledLeft;
                    frames[(index + sample) * 2u + 1u] += channelRight * scaledRight;
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
                uint32_t sample = 0u;
#if defined(AUDIO_MIX_AVX2_FMA)
                // Восемь моносемплов за итерацию: каждый дублируется в оба
                // канала, что даёт восемь кадров. Дублирование — перестановка
                // уже приведённых к float значений, арифметика не меняется.
                const __m256 gainVector = _mm256_setr_ps(
                    scaledLeft, scaledRight, scaledLeft, scaledRight,
                    scaledLeft, scaledRight, scaledLeft, scaledRight);
                const __m256i duplicateLow = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
                const __m256i duplicateHigh = _mm256_setr_epi32(4, 4, 5, 5, 6, 6, 7, 7);
                for (; sample + 8u <= count; sample += 8u)
                {
                    __m128i packed = _mm_loadu_si128((const __m128i *)(samples + frame));
                    __m256 converted = _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(packed));
                    __m256 low = _mm256_permutevar8x32_ps(converted, duplicateLow);
                    __m256 high = _mm256_permutevar8x32_ps(converted, duplicateHigh);
                    float *destination = frames + (index + sample) * 2u;
                    __m256 mixLow =
                        _mm256_fmadd_ps(gainVector, low, _mm256_loadu_ps(destination));
                    __m256 mixHigh =
                        _mm256_fmadd_ps(gainVector, high, _mm256_loadu_ps(destination + 8u));
                    _mm256_storeu_ps(destination, mixLow);
                    _mm256_storeu_ps(destination + 8u, mixHigh);
                    frame += 8u;
                }
#endif
                for (; sample < count; ++sample)
                {
                    float mono = (float)samples[frame];
                    frames[(index + sample) * 2u] += mono * scaledLeft;
                    frames[(index + sample) * 2u + 1u] += mono * scaledRight;
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

static void MixStreamVoice(VoiceSlot *slot, float *frames, uint32_t frameCount)
{
    AudioStream *stream = slot->stream;
    if (PlatformAtomicLoadU32Acquire(&stream->terminate) != 0u)
    {
        slot->stream = NULL;
        PlatformAtomicStoreU32Release(&slot->state, VOICE_FINISHED);
        return;
    }
    bool underrun = false;
    bool finished = false;
    for (uint32_t index = 0u; index < frameCount; ++index)
    {
        if (slot->position >= stream->info.frameCount)
        {
            if (!slot->looping)
            {
                slot->position = stream->info.frameCount;
                finished = true;
                break;
            }
            slot->position -= (double)(uint64_t)(slot->position / stream->info.frameCount) *
                              stream->info.frameCount;
        }
        float left = 0.0f, right = 0.0f;
        if (!AudioStreamSample(stream, slot->streamEpoch, &slot->streamOffset, slot->step, &left,
                               &right))
        {
            if (PlatformAtomicLoadU32Acquire(&stream->result) != AUDIO_RESULT_OK)
            {
                finished = true;
            }
            underrun = true;
            break;
        }
        frames[index * 2u] += left * slot->gains.left;
        frames[index * 2u + 1u] += right * slot->gains.right;
        slot->position += slot->step;
    }
    if (underrun)
        PlatformAtomicIncrementI64(&stream->underruns);
    if (!slot->looping && slot->position >= stream->info.frameCount)
    {
        slot->position = stream->info.frameCount;
        finished = true;
    }
    if (slot->looping && slot->position >= stream->info.frameCount)
        slot->position -=
            (double)(uint64_t)(slot->position / stream->info.frameCount) * stream->info.frameCount;
    if (finished)
    {
        /* FINISHED is the retirement ACK: no stream access may follow it. */
        slot->stream = NULL;
        PlatformAtomicStoreU32Release(&slot->state, VOICE_FINISHED);
    }
}

static void RenderFrames(void *context, float *frames, uint32_t frameCount)
{
    AudioDevice *device = (AudioDevice *)context;
    memset(frames, 0, (size_t)frameCount * AUDIO_MIX_CHANNELS * sizeof(float));

    bool commandsApplied = DrainCommands(device);

    // Громкость читается здесь же: после memset буфер нулевой, и нулём он
    // остаётся только при конечной громкости — NaN или бесконечность дали бы
    // NaN. Холостой путь это учитывает, чтобы не менять семантику сэмплов.
    uint32_t masterBits = PlatformAtomicLoadU32Acquire(&device->masterVolumeBits);
    bool finiteMaster = (masterBits & 0x7f800000u) != 0x7f800000u;

    // Холостого микшера не касаются ни обход слотов, ни масштабирование:
    // activeVoices и кольцо команд принадлежат потоку вывода, и нулевой счёт
    // без разобранных команд означает, что ни один слот не ACTIVE. Буфер уже
    // нулевой после memset, поэтому тишина не платит за проход по слотам и
    // громкости. Голос становится ACTIVE только через COMMAND_START, значит
    // без команд его появление невозможно.
    if (!commandsApplied && finiteMaster &&
        PlatformAtomicLoadU32Acquire(&device->activeVoices) == 0u && device->voiceScanLimit == 0u)
    {
        PlatformAtomicAddI64(&device->mixedFrames, (int64_t)frameCount);
        return;
    }

    uint32_t active = 0u;
    uint32_t mixedVoices = 0u;
    uint32_t scanLimit = device->voiceScanLimit;
    uint32_t activeScanLimit = 0u;
    // Звучащие голоса лежат ниже voiceScanLimit: все они когда-то прошли
    // COMMAND_START на этом же потоке. Пустые хвостовые слоты не обходятся.
    for (uint32_t index = 0; index < scanLimit; ++index)
    {
        VoiceSlot *slot = &device->voices[index];
        if (PlatformAtomicLoadU32Acquire(&slot->state) != (uint32_t)VOICE_ACTIVE) continue;
        if (slot->clip == NULL && slot->stream == NULL)
            continue;
        if (slot->stream != NULL && PlatformAtomicLoadU32Acquire(&slot->stream->terminate) != 0u)
        {
            slot->stream = NULL;
            PlatformAtomicStoreU32Release(&slot->state, VOICE_FINISHED);
            continue;
        }
        if (!slot->paused)
        {
            if (slot->stream != NULL)
                MixStreamVoice(slot, frames, frameCount);
            else
                MixVoice(slot, frames, frameCount);
            PublishPosition(slot);
        }
        ++mixedVoices;
        if (PlatformAtomicLoadU32Acquire(&slot->state) == (uint32_t)VOICE_ACTIVE)
        {
            ++active;
            activeScanLimit = index + 1u;
        }
    }
    // Reuse the state loads above to trim stopped or naturally finished tail
    // slots. A later COMMAND_START raises this output-thread-owned boundary.
    if (activeScanLimit < scanLimit) device->voiceScanLimit = activeScanLimit;
    PlatformAtomicStoreU32Release(&device->activeVoices, active);

    // Разобранные команды были, но ни один голос не дожил до микса — буфер
    // весь нулевой, и конечная громкость с ограничением вернула бы те же нули.
    uint32_t sampleCount = frameCount * AUDIO_MIX_CHANNELS;
    // В буфер миксовался ровно один голос, а общая громкость равна ровно 1,0.
    // Выборки клипа лежат в [-1,1], усиление голоса тоже (громкость и панорама
    // ограничены единицей), поэтому итог уже в диапазоне, а умножение на 1,0
    // и ограничение — тождество. Полный проход по буферу ничего бы не изменил,
    // и его можно пропустить, не меняя ни одного бита. При двух и более
    // голосах сумма может выйти за диапазон, поэтому там проход обязателен;
    // нефинитная громкость тоже сохраняет прежний путь (NaN остаётся NaN).
    bool unityMaster = masterBits == 0x3f800000u;
    if ((mixedVoices > 0u || !finiteMaster) && (!unityMaster || mixedVoices > 1u))
        ApplyMasterGain(frames, sampleCount, BitsToFloat(masterBits));
    PlatformAtomicAddI64(&device->mixedFrames, (int64_t)frameCount);
}

// === Устройство ===

AudioResult AudioDeviceCreateWithOutputServiceEx(
    const AudioDeviceConfiguration *configuration,
    const LaiueAudioOutputServiceV1 *outputService,
    uint32_t outputServiceSize,
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
    else if (outputService != NULL && outputService->create != NULL)
    {
        LaiueAudioOutputDescription outputDescription = {
            .sampleRate = description.sampleRate,
            .frameCountHint = description.frameCountHint,
            .render = description.render,
            .context = description.context,
        };
        bool hasContextCreate =
            outputServiceSize >= LAIUE_AUDIO_OUTPUT_SERVICE_V1_CONTEXT_SIZE &&
            outputService->structSize >= LAIUE_AUDIO_OUTPUT_SERVICE_V1_CONTEXT_SIZE &&
            outputService->createWithContext != NULL && outputService->context != NULL;
        created = hasContextCreate
                      ? outputService->createWithContext(outputService->context,
                                                         &outputDescription,
                                                         &device->outputBackend) != 0u
                      : outputService->create(&outputDescription, &device->outputBackend) != 0u;
        created = created && device->outputBackend != NULL;
        if (created)
        {
            device->outputService = outputService;
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

AudioResult AudioDeviceCreateWithOutputService(
    const AudioDeviceConfiguration *configuration,
    const LaiueAudioOutputServiceV1 *outputService,
    AudioDevice **outDevice)
{
    uint32_t outputServiceSize = outputService != NULL ? outputService->structSize : 0u;
    return AudioDeviceCreateWithOutputServiceEx(configuration, outputService, outputServiceSize,
                                                outDevice);
}

AudioResult AudioDeviceCreate(const AudioDeviceConfiguration *configuration,
                              AudioDevice **outDevice)
{
    return AudioDeviceCreateWithOutputService(configuration, g_audioOutputService, outDevice);
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
    ReleaseRetiredStreams(device, true);
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
    if (device == NULL || clip == NULL || clip->device != device)
        return AUDIO_VOICE_NONE;

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
            if (accepted)
            {
                for (AudioStream *stream = device->streams; stream != NULL; stream = stream->next)
                    if (!stream->retired && stream->voice == voice)
                        AudioStreamSetLooping(stream, resolved.looping);
            }
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
    /* Offscreen is not a realtime callback: bounded chunks are prefetched on
     * its caller, deterministically, without moving IO into RenderFrames. */
    if (device->streams == NULL)
    {
        RenderFrames(device, outFrames, frameCount);
        return true;
    }
    PlatformMutexLock(&device->producerLock);
    DrainCommands(device);
    uint32_t completed = 0u;
    while (completed < frameCount)
    {
        for (AudioStream *stream = device->streams; stream != NULL; stream = stream->next)
            if (!stream->retired)
                AudioStreamPump(stream);
        uint32_t count = frameCount - completed;
        if (count > 256u)
            count = 256u;
        for (uint32_t index = 0u; index < device->voiceScanLimit; ++index)
        {
            VoiceSlot *slot = &device->voices[index];
            if (slot->stream == NULL || slot->paused || slot->step <= 0.0)
                continue;
            double safe = 4096.0 / slot->step;
            /* Compare against the already bounded output count before casting:
             * tiny source/device ratios may exceed UINT32_MAX. */
            if (safe < count)
                count = safe >= 1.0 ? (uint32_t)safe : 1u;
        }
        RenderFrames(device, outFrames + (size_t)completed * 2u, count);
        completed += count;
    }
    ReleaseRetiredStreams(device, false);
    PlatformMutexUnlock(&device->producerLock);
    return true;
}

static void ReleaseRetiredStreams(AudioDevice *device, bool releaseEverything)
{
    uint64_t mixed = (uint64_t)PlatformAtomicLoadI64(&device->mixedFrames);
    uint64_t applied = (uint64_t)PlatformAtomicLoadI64(&device->commandApplied);
    AudioStream **link = &device->streams;
    while (*link != NULL)
    {
        AudioStream *stream = *link;
        uint32_t index = stream->voice & 255u;
        uint32_t generation = stream->voice >> 8;
        const VoiceSlot *slot = &device->voices[index];
        uint32_t state = PlatformAtomicLoadU32Acquire(&slot->state);
        bool voiceReleased = stream->voice == AUDIO_VOICE_NONE || slot->generation != generation ||
                             state == VOICE_FREE || state == VOICE_FINISHED;
        if (releaseEverything || (stream->retired && voiceReleased &&
                                  stream->retireFrame <= mixed && stream->retireCommand <= applied))
        {
            *link = stream->next;
            AudioStreamFree(stream);
        }
        else
            link = &stream->next;
    }
}

AudioResult AudioStreamCreate(AudioDevice *device, const AudioStreamDescription *description,
                              AudioStream **outStream)
{
    AudioResult result = AudioStreamAllocate(
        device, description, device != NULL && device->backendKind != AUDIO_BACKEND_OFFSCREEN,
        outStream);
    if (result != AUDIO_RESULT_OK)
        return result;
    PlatformMutexLock(&device->producerLock);
    ReleaseRetiredStreams(device, false);
    (*outStream)->next = device->streams;
    device->streams = *outStream;
    PlatformMutexUnlock(&device->producerLock);
    return result;
}

AudioResult AudioStreamOpenFile(AudioDevice *device, const char *path, AudioStream **outStream)
{
    AudioResult result = AudioStreamAllocateFile(
        device, path, device != NULL && device->backendKind != AUDIO_BACKEND_OFFSCREEN, outStream);
    if (result != AUDIO_RESULT_OK)
        return result;
    PlatformMutexLock(&device->producerLock);
    ReleaseRetiredStreams(device, false);
    (*outStream)->next = device->streams;
    device->streams = *outStream;
    PlatformMutexUnlock(&device->producerLock);
    return result;
}

void AudioStreamDestroy(AudioStream *stream)
{
    if (stream == NULL || stream->retired)
        return;
    AudioDevice *device = stream->device;
    PlatformMutexLock(&device->producerLock);
    uint32_t index = stream->voice & 255u;
    uint32_t generation = stream->voice >> 8;
    if (stream->voice != AUDIO_VOICE_NONE && device->voices[index].generation == generation)
    {
        AudioCommand command = {.type = COMMAND_STOP, .slot = index, .generation = generation};
        PushCommand(device, &command);
    }
    stream->retired = true;
    /* terminate is observed even by paused voices; retirement does not
     * rely on a STOP fitting into the command ring. */
    stream->retireFrame = (uint64_t)PlatformAtomicLoadI64(&device->mixedFrames) + 1u;
    stream->retireCommand = (uint64_t)PlatformAtomicLoadI64(&device->commandIssued);
    AudioStreamShutdown(stream);
    ReleaseRetiredStreams(device, false);
    PlatformMutexUnlock(&device->producerLock);
}

AudioVoice AudioVoicePlayStream(AudioDevice *device, AudioStream *stream,
                                const AudioVoiceParameters *parameters)
{
    if (device == NULL || stream == NULL || stream->device != device || stream->retired)
        return AUDIO_VOICE_NONE;
    AudioVoiceParameters resolved;
    FillParameters(parameters, &resolved);
    PlatformMutexLock(&device->producerLock);
    if (AudioVoiceIsActive(device, stream->voice))
    {
        PlatformMutexUnlock(&device->producerLock);
        return AUDIO_VOICE_NONE;
    }
    AudioVoice handle = AUDIO_VOICE_NONE;
    for (uint32_t index = 0u; index < AUDIO_MAX_VOICES; ++index)
    {
        VoiceSlot *slot = &device->voices[index];
        uint32_t state = PlatformAtomicLoadU32Acquire(&slot->state);
        if (state != VOICE_FREE && state != VOICE_FINISHED)
            continue;
        /* Reserve command capacity before changing the decoder's epoch. */
        if ((device->commandWrite + 1u) % AUDIO_COMMAND_CAPACITY ==
            PlatformAtomicLoadU32Acquire(&device->commandRead))
            break;
        uint32_t epoch = 0u;
        if (!AudioStreamPrepare(stream, 0u, resolved.looping, &epoch))
            break;
        slot->clipSampleRate = stream->info.sampleRate;
        device->slotClips[index] = NULL;
        slot->generation = device->nextGeneration++;
        if (device->nextGeneration == 0u)
            device->nextGeneration = 1u;
        AudioCommand command = {
            .type = COMMAND_START,
            .slot = index,
            .generation = slot->generation,
            .stream = stream,
            .streamEpoch = epoch,
            .step = ComputeStep(stream->info.sampleRate, device->sampleRate, resolved.speed),
            .gains = ComputeGains(resolved.volume, resolved.pan),
            .looping = resolved.looping,
        };
        PlatformAtomicStoreU32Release(&slot->state, VOICE_PENDING);
        if (!PushCommand(device, &command))
        {
            PlatformAtomicStoreU32Release(&slot->state, VOICE_FREE);
            break;
        }
        handle = MakeVoiceHandle(index, slot->generation);
        stream->voice = handle;
        break;
    }
    PlatformMutexUnlock(&device->producerLock);
    return handle;
}

bool AudioVoicePause(AudioDevice *device, AudioVoice voice, bool paused)
{
    uint32_t index = 0u, generation = 0u;
    if (device == NULL || !SplitVoiceHandle(voice, &index, &generation))
        return false;
    PlatformMutexLock(&device->producerLock);
    AudioCommand command = {
        .type = COMMAND_PAUSE, .slot = index, .generation = generation, .paused = paused};
    bool ok = AudioVoiceIsActive(device, voice) && PushCommand(device, &command);
    PlatformMutexUnlock(&device->producerLock);
    return ok;
}

bool AudioVoiceSeek(AudioDevice *device, AudioVoice voice, double seconds)
{
    uint32_t index = 0u, generation = 0u;
    if (device == NULL || !SplitVoiceHandle(voice, &index, &generation) || !(seconds >= 0.0) ||
        seconds > DBL_MAX)
        return false;
    PlatformMutexLock(&device->producerLock);
    bool ok = false;
    if (AudioVoiceIsActive(device, voice))
    {
        AudioStream *stream = device->streams;
        while (stream != NULL && (stream->retired || stream->voice != voice))
            stream = stream->next;
        const AudioClip *clip = device->slotClips[index];
        uint32_t total =
            stream != NULL ? stream->info.frameCount : (clip != NULL ? clip->frameCount : 0u);
        uint32_t rate = device->voices[index].clipSampleRate;
        double position = seconds * rate;
        if (seconds <= (double)total / rate && position > total)
            position = total;
        if (position <= total && (device->commandWrite + 1u) % AUDIO_COMMAND_CAPACITY !=
                                     PlatformAtomicLoadU32Acquire(&device->commandRead))
        {
            uint32_t epoch = 0u;
            /* API producer owns loop preference; copy it through stream lock. */
            bool looping = false;
            if (stream != NULL)
            {
                PlatformMutexLock(&stream->decoderLock);
                looping = stream->looping;
                PlatformMutexUnlock(&stream->decoderLock);
            }
            ok = stream == NULL || AudioStreamPrepare(stream, (uint32_t)position, looping, &epoch);
            if (ok)
            {
                AudioCommand command = {.type = COMMAND_SEEK,
                                        .slot = index,
                                        .generation = generation,
                                        .position = (double)(uint32_t)position,
                                        .streamEpoch = epoch};
                ok = PushCommand(device, &command);
            }
        }
    }
    PlatformMutexUnlock(&device->producerLock);
    return ok;
}

bool AudioVoiceGetPosition(const AudioDevice *device, AudioVoice voice, double *outSeconds)
{
    uint32_t index = 0u, generation = 0u;
    if (device == NULL || outSeconds == NULL || !SplitVoiceHandle(voice, &index, &generation))
        return false;
    PlatformMutexLock((PlatformMutex *)&device->producerLock);
    const VoiceSlot *slot = &device->voices[index];
    bool valid =
        slot->generation == generation && PlatformAtomicLoadU32Acquire(&slot->state) != VOICE_FREE;
    if (valid)
    {
        if (PlatformAtomicLoadU32Acquire(&slot->state) == VOICE_PENDING)
            *outSeconds = 0.0;
        else
        {
            uint32_t before, after, low, high;
            do
            {
                before = PlatformAtomicLoadU32Acquire(&slot->positionSequence);
                low = PlatformAtomicLoadU32Acquire(&slot->positionLow);
                high = PlatformAtomicLoadU32Acquire(&slot->positionHigh);
                after = PlatformAtomicLoadU32Acquire(&slot->positionSequence);
            } while ((before & 1u) != 0u || before != after);
            union
            {
                double value;
                uint64_t bits;
            } position;
            position.bits = ((uint64_t)high << 32) | low;
            *outSeconds = position.value / slot->clipSampleRate;
        }
    }
    PlatformMutexUnlock((PlatformMutex *)&device->producerLock);
    return valid;
}
