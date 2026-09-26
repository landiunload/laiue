// ROUND2 harness 30-sound: WAV PCM/float decode, sound dispatch и запись `.la`
// (PCM16 + IMA ADPCM) на синтетических входах.
//
// Стенд собран из двух половин:
//   * decode: WaveInspect + WaveDecodeSamples на готовых WAV-буферах;
//   * dispatch: SoundProbe + SoundInspect + SoundDecodeSamples (путь движка);
//   * encode: SoundEncode для PCM16 и ADPCM.
//
// Каждая нагрузка N раз вызывает целевой код и печатает суммарное время и
// FNV-1a checksum выхода. A/B-скрипт обязан увидеть одинаковый checksum у
// baseline и candidate: иначе измеряется другой сигнал.
//
// Env:
//   LAIUE_R2_SOUND_BENCH_WORKLOAD — имя одной нагрузки (пусто = все);
//   LAIUE_R2_SOUND_BENCH_SCALE    — масштаб числа повторов в процентах (100);
//   LAIUE_R2_SOUND_BENCH_SAMPLES  — число внутренних выборок (1).

#include "media/la_encode.h"
#include "media/sound.h"
#include "media/wave_decode.h"

#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BENCH_FRAMES 1048576u
#define BENCH_CHANNELS 2u
#define BENCH_SAMPLE_RATE 48000u
#define BENCH_MAX_SAMPLES 32u
#define BENCH_MAX_WORKLOADS 24u

#define BENCH_WAVE_CAPACITY (64u + BENCH_FRAMES * BENCH_CHANNELS * 8u)
#define BENCH_DECODED_COUNT (BENCH_FRAMES * BENCH_CHANNELS)
#define BENCH_PCM16_BYTES (SOUND_LA_HEADER_BYTES + BENCH_FRAMES * BENCH_CHANNELS * 2u)

static volatile uint64_t g_sink;

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

static void WriteFixed(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.000");
        return;
    }
    if (value > 1000000000.0) value = 1000000000.0;
    uint64_t whole = (uint64_t)value;
    WriteUnsigned(whole);
    WriteText(".");
    uint64_t fraction = (uint64_t)((value - (double)whole) * 1000.0);
    if (fraction > 999u) fraction = 999u;
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        buffer[2u + index] = digits[(value >> (60u - index * 4u)) & 0xFu];
    }
    buffer[18] = '\0';
    WriteText(buffer);
}

static uint32_t ReadEnvUnsigned(const char *name, uint32_t fallback, uint32_t maximum)
{
    char text[32];
    uint32_t length = PlatformGetEnvironmentUtf8(name, text, (uint32_t)sizeof(text));
    if (length == 0u) return fallback;
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < length; ++index)
    {
        if (text[index] < '0' || text[index] > '9') return fallback;
        value = value * 10u + (uint32_t)(text[index] - '0');
        if (value > maximum) return maximum;
    }
    return value == 0u ? fallback : value;
}

static uint64_t HashBytes(uint64_t hash, const void *data, uint32_t sizeBytes)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (uint32_t index = 0u; index < sizeBytes; ++index)
    {
        hash ^= (uint64_t)bytes[index];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// === Синтетические сэмплы ===

// typical: медленная пила + быстрая рябь + детерминированный шум, без libm.
// adversarial: максимальный размах со сменой знака каждый отсчёт — худший
// случай для ветвления кодека и знаковой коррекции округления.
static int16_t PatternSample(uint32_t index, bool adversarial)
{
    if (adversarial) return (index & 1u) != 0u ? 32767 : (int16_t)(-32768);
    int32_t slow = (int32_t)(index % 512u) - 256;
    int32_t fast = (int32_t)(index % 16u) - 8;
    int32_t noise = (int32_t)((index * 2654435761u) >> 24) - 128;
    int32_t value = slow * 90 + fast * 500 + noise * 40;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (int16_t)value;
}

static void FillSamples(int16_t *out, uint32_t frames, uint32_t channels, bool adversarial)
{
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        int16_t left = PatternSample(frame, adversarial);
        for (uint32_t channel = 0u; channel < channels; ++channel)
        {
            // Правый канал противофазный: каналы не должны путаться.
            out[(size_t)frame * channels + channel] = channel == 0u ? left : (int16_t)(-left);
        }
    }
}

// === Сборка WAV ===

static void PutU16(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void PutU32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void PutTag(uint8_t *bytes, const char *tag)
{
    for (uint32_t index = 0u; index < 4u; ++index) bytes[index] = (uint8_t)tag[index];
}

// Канонический WAV: RIFF/WAVE/fmt /data без ведущих чанков.
static uint32_t BuildWave(uint8_t *out, uint32_t formatTag, bool isFloat, uint32_t channels,
                          uint32_t bitsPerSample, const int16_t *samples, uint32_t frames)
{
    uint32_t sampleBytes = bitsPerSample / 8u;
    uint32_t dataBytes = frames * channels * sampleBytes;
    PutTag(out, "RIFF");
    PutU32(out + 4, 36u + dataBytes);
    PutTag(out + 8, "WAVE");
    PutTag(out + 12, "fmt ");
    PutU32(out + 16, 16u);
    PutU16(out + 20, formatTag);
    PutU16(out + 22, channels);
    PutU32(out + 24, BENCH_SAMPLE_RATE);
    PutU32(out + 28, BENCH_SAMPLE_RATE * channels * sampleBytes);
    PutU16(out + 32, (uint32_t)(channels * sampleBytes));
    PutU16(out + 34, bitsPerSample);
    PutTag(out + 36, "data");
    PutU32(out + 40, dataBytes);

    uint8_t *payload = out + 44u;
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        for (uint32_t channel = 0u; channel < channels; ++channel)
        {
            int16_t sample = samples[(size_t)frame * channels + channel];
            size_t at = ((size_t)frame * channels + channel) * sampleBytes;
            if (isFloat)
            {
                if (bitsPerSample == 32u)
                {
                    union
                    {
                        float value;
                        uint32_t bits;
                    } cast;
                    cast.value = (float)sample / 32768.0f;
                    PutU32(payload + at, cast.bits);
                }
                else
                {
                    union
                    {
                        double value;
                        uint64_t bits;
                    } cast;
                    cast.value = (double)sample / 32768.0;
                    PutU32(payload + at, (uint32_t)(cast.bits & 0xFFFFFFFFu));
                    PutU32(payload + at + 4u, (uint32_t)(cast.bits >> 32));
                }
            }
            else if (bitsPerSample == 8u)
            {
                payload[at] = (uint8_t)((sample >> 8) + 128);
            }
            else if (bitsPerSample == 16u)
            {
                PutU16(payload + at, (uint32_t)(uint16_t)sample);
            }
            else if (bitsPerSample == 24u)
            {
                int32_t value = (int32_t)sample * 256;
                payload[at] = (uint8_t)(value & 0xFF);
                payload[at + 1u] = (uint8_t)((value >> 8) & 0xFF);
                payload[at + 2u] = (uint8_t)((value >> 16) & 0xFF);
            }
            else
            {
                PutU32(payload + at, (uint32_t)((int32_t)sample * 65536));
            }
        }
    }
    return 44u + dataBytes;
}

// === Нагрузки ===

typedef struct WaveJob
{
    const uint8_t *file;
    uint32_t fileBytes;
    WaveInfo info;
    int16_t *out;
} WaveJob;

typedef struct DispatchJob
{
    const uint8_t *file;
    uint32_t fileBytes;
    SoundInfo info;
    int16_t *out;
} DispatchJob;

typedef struct EncodeJob
{
    const SoundClip *clip;
    uint8_t *out;
    uint32_t capacity;
    uint32_t written;
} EncodeJob;

typedef struct Workload
{
    const char *name;
    uint32_t repeats;
    uint64_t checksum;
    uint32_t valueCount;
    double samples[BENCH_MAX_SAMPLES];
    uint32_t sampleCount;
    int kind;   // 0 wave, 1 dispatch, 2 encode
    SoundClip clipStorage;
    union
    {
        WaveJob wave;
        DispatchJob dispatch;
        EncodeJob encode;
    } job;
} Workload;

static Workload g_workloads[BENCH_MAX_WORKLOADS];
static uint32_t g_workloadCount;
static int16_t *g_samples;
static int16_t *g_decoded;
static uint8_t *g_wave;
static uint8_t *g_encoded;

static Workload *AddWorkload(const char *name, uint32_t repeats, int kind, uint32_t valueCount)
{
    if (g_workloadCount >= BENCH_MAX_WORKLOADS) return NULL;
    Workload *workload = &g_workloads[g_workloadCount++];
    workload->name = name;
    workload->repeats = repeats;
    workload->kind = kind;
    workload->valueCount = valueCount;
    workload->checksum = 0u;
    workload->sampleCount = 0u;
    return workload;
}

static double RunWorkload(Workload *workload, uint32_t repeats)
{
    double begin = PlatformMonotonicSeconds();
    uint64_t accumulator = 0u;
    if (workload->kind == 0)
    {
        WaveJob *job = &workload->job.wave;
        for (uint32_t repeat = 0u; repeat < repeats; ++repeat)
        {
            WaveDecodeSamples(job->file, job->fileBytes, &job->info, job->out,
                              workload->valueCount);
            accumulator += (uint16_t)job->out[(repeat * 97u) % workload->valueCount];
        }
    }
    else if (workload->kind == 1)
    {
        DispatchJob *job = &workload->job.dispatch;
        for (uint32_t repeat = 0u; repeat < repeats; ++repeat)
        {
            if (SoundDecodeSamples(job->file, job->fileBytes, &job->info, job->out,
                                   workload->valueCount, NULL, 0u) != SOUND_OK)
            {
                WriteText("r2sound dispatch failed\n");
                LaiueTestRuntimeExit(1);
            }
            accumulator += (uint16_t)job->out[(repeat * 97u) % workload->valueCount];
        }
    }
    else
    {
        EncodeJob *job = &workload->job.encode;
        for (uint32_t repeat = 0u; repeat < repeats; ++repeat)
        {
            uint32_t written = 0u;
            if (SoundEncode(job->clip, job->out, job->capacity, &written) != SOUND_OK)
            {
                WriteText("r2sound encode failed\n");
                LaiueTestRuntimeExit(1);
            }
            accumulator += job->out[(repeat * 97u) % written];
        }
    }
    g_sink += accumulator;
    return PlatformMonotonicSeconds() - begin;
}

static void PrepareWaveWorkload(const char *name, uint32_t repeats, uint32_t formatTag,
                                bool isFloat, uint32_t channels, uint32_t bits,
                                bool adversarial)
{
    FillSamples(g_samples, BENCH_FRAMES, channels, adversarial);
    uint32_t bytes = BuildWave(g_wave, formatTag, isFloat, channels, bits, g_samples, BENCH_FRAMES);
    Workload *workload =
        AddWorkload(name, repeats, 0, BENCH_FRAMES * channels);
    if (workload == NULL)
    {
        WriteText("r2sound too many workloads\n");
        LaiueTestRuntimeExit(1);
    }
    WaveJob *job = &workload->job.wave;
    job->file = g_wave;
    job->fileBytes = bytes;
    job->out = g_decoded;
    if (WaveInspect(g_wave, bytes, &job->info) != WAVE_OK)
    {
        WriteText("r2sound wave inspect failed\n");
        LaiueTestRuntimeExit(1);
    }
    if (WaveDecodeSamples(g_wave, bytes, &job->info, g_decoded, BENCH_FRAMES * channels) != WAVE_OK)
    {
        WriteText("r2sound wave decode failed\n");
        LaiueTestRuntimeExit(1);
    }
    workload->checksum = HashBytes(0xcbf29ce484222325ull, g_decoded,
                                   BENCH_FRAMES * channels * (uint32_t)sizeof(int16_t));
}

static void PrepareDispatchWorkload(const char *name, uint32_t repeats, uint32_t channels,
                                    bool adversarial)
{
    FillSamples(g_samples, BENCH_FRAMES, channels, adversarial);
    uint32_t bytes = BuildWave(g_wave, 1u, false, channels, 16u, g_samples, BENCH_FRAMES);
    Workload *workload = AddWorkload(name, repeats, 1, BENCH_FRAMES * channels);
    if (workload == NULL)
    {
        WriteText("r2sound too many workloads\n");
        LaiueTestRuntimeExit(1);
    }
    DispatchJob *job = &workload->job.dispatch;
    job->file = g_wave;
    job->fileBytes = bytes;
    job->out = g_decoded;
    if (SoundInspect(g_wave, bytes, &job->info) != SOUND_OK)
    {
        WriteText("r2sound sound inspect failed\n");
        LaiueTestRuntimeExit(1);
    }
    if (SoundDecodeSamples(g_wave, bytes, &job->info, g_decoded, BENCH_FRAMES * channels, NULL,
                           0u) != SOUND_OK)
    {
        WriteText("r2sound sound decode failed\n");
        LaiueTestRuntimeExit(1);
    }
    workload->checksum = HashBytes(0xcbf29ce484222325ull, g_decoded,
                                   BENCH_FRAMES * channels * (uint32_t)sizeof(int16_t));
}

static void PrepareEncodeWorkload(const char *name, uint32_t repeats, uint32_t channels,
                                  SoundEncoding encoding, uint32_t frames, bool adversarial)
{
    FillSamples(g_samples, frames, channels, adversarial);
    Workload *workload = AddWorkload(name, repeats, 2, frames * channels);
    if (workload == NULL)
    {
        WriteText("r2sound too many workloads\n");
        LaiueTestRuntimeExit(1);
    }
    SoundClip *clip = &workload->clipStorage;
    clip->samples = g_samples;
    clip->frameCount = frames;
    clip->channelCount = channels;
    clip->sampleRate = BENCH_SAMPLE_RATE;
    clip->encoding = encoding;
    clip->sourceModifiedTime = 0u;
    clip->sourceSizeBytes = 0u;
    EncodeJob *job = &workload->job.encode;
    job->clip = clip;
    job->out = g_encoded;
    job->capacity = BENCH_PCM16_BYTES;
    uint32_t written = 0u;
    if (SoundEncode(clip, g_encoded, BENCH_PCM16_BYTES, &written) != SOUND_OK)
    {
        WriteText("r2sound encode prepare failed\n");
        LaiueTestRuntimeExit(1);
    }
    job->written = written;
    workload->checksum = HashBytes(0xcbf29ce484222325ull, g_encoded, written);
}

static void Report(Workload *workload)
{
    // Сортировка копии для median/min/max.
    double sorted[BENCH_MAX_SAMPLES];
    for (uint32_t index = 0u; index < workload->sampleCount; ++index)
    {
        sorted[index] = workload->samples[index];
    }
    for (uint32_t index = 1u; index < workload->sampleCount; ++index)
    {
        double key = sorted[index];
        uint32_t position = index;
        while (position > 0u && sorted[position - 1u] > key)
        {
            sorted[position] = sorted[position - 1u];
            --position;
        }
        sorted[position] = key;
    }
    double median = sorted[workload->sampleCount / 2u];
    WriteText("r2sound summary workload=");
    WriteText(workload->name);
    WriteText(" samples=");
    WriteUnsigned(workload->sampleCount);
    WriteText(" min_ms=");
    WriteFixed(sorted[0]);
    WriteText(" median_ms=");
    WriteFixed(median);
    WriteText(" max_ms=");
    WriteFixed(sorted[workload->sampleCount - 1u]);
    WriteText(" checksum=");
    WriteHex(workload->checksum);
    WriteText("\n");
}

static bool WantWorkload(const char *filter, const char *name)
{
    if (filter[0] == '\0') return true;
    const char *left = filter;
    while (*left != '\0' && *name != '\0' && *left == *name)
    {
        ++left;
        ++name;
    }
    return *left == '\0' && *name == '\0';
}

static void Fail(const char *message)
{
    WriteText("r2sound: ");
    WriteText(message);
    WriteText("\n");
    LaiueTestRuntimeExit(1);
}

LAIUE_TEST_ENTRY(R2SoundBenchmarkEntryPoint)
{
    char filter[64];
    uint32_t filterLength =
        PlatformGetEnvironmentUtf8("LAIUE_R2_SOUND_BENCH_WORKLOAD", filter, (uint32_t)sizeof(filter));
    if (filterLength >= (uint32_t)sizeof(filter))
    {
        Fail("workload name is too long");
    }
    uint32_t samples = ReadEnvUnsigned("LAIUE_R2_SOUND_BENCH_SAMPLES", 1u, BENCH_MAX_SAMPLES);
    uint32_t scalePercent = ReadEnvUnsigned("LAIUE_R2_SOUND_BENCH_SCALE", 100u, 100000u);
    if (samples == 0u) samples = 1u;

    // Все буферы в куче: сборка без CRT не растит стек через __chkstk.
    g_samples = (int16_t *)PlatformAllocate(BENCH_DECODED_COUNT * sizeof(int16_t), false);
    g_decoded = (int16_t *)PlatformAllocate(BENCH_DECODED_COUNT * sizeof(int16_t), false);
    g_wave = (uint8_t *)PlatformAllocate(BENCH_WAVE_CAPACITY, false);
    g_encoded = (uint8_t *)PlatformAllocate(BENCH_PCM16_BYTES, false);
    if (g_samples == NULL || g_decoded == NULL || g_wave == NULL || g_encoded == NULL)
    {
        Fail("buffers could not be allocated");
    }

#define SCALE(base) ((base) * scalePercent / 100u < 1u ? 1u : ((base) * scalePercent / 100u))

    if (WantWorkload(filter, "wav_pcm16_stereo"))
        PrepareWaveWorkload("wav_pcm16_stereo", SCALE(140u), 1u, false, 2u, 16u, false);
    if (WantWorkload(filter, "wav_pcm16_mono"))
        PrepareWaveWorkload("wav_pcm16_mono", SCALE(300u), 1u, false, 1u, 16u, false);
    if (WantWorkload(filter, "wav_pcm16_adv"))
        PrepareWaveWorkload("wav_pcm16_adv", SCALE(150u), 1u, false, 2u, 16u, true);
    if (WantWorkload(filter, "wav_pcm8_stereo"))
        PrepareWaveWorkload("wav_pcm8_stereo", SCALE(2400u), 1u, false, 2u, 8u, false);
    if (WantWorkload(filter, "wav_pcm24_mono"))
        PrepareWaveWorkload("wav_pcm24_mono", SCALE(75u), 1u, false, 1u, 24u, false);
    if (WantWorkload(filter, "wav_pcm32_mono"))
        PrepareWaveWorkload("wav_pcm32_mono", SCALE(105u), 1u, false, 1u, 32u, false);
    if (WantWorkload(filter, "wav_float32_mono"))
        PrepareWaveWorkload("wav_float32_mono", SCALE(140u), 3u, true, 1u, 32u, false);
    if (WantWorkload(filter, "wav_float32_adv"))
        PrepareWaveWorkload("wav_float32_adv", SCALE(140u), 3u, true, 1u, 32u, true);
    if (WantWorkload(filter, "wav_float64_mono"))
        PrepareWaveWorkload("wav_float64_mono", SCALE(92u), 3u, true, 1u, 64u, false);
    if (WantWorkload(filter, "dispatch_wav_pcm16"))
        PrepareDispatchWorkload("dispatch_wav_pcm16", SCALE(150u), 2u, false);
    if (WantWorkload(filter, "la_pcm16_mono"))
        PrepareEncodeWorkload("la_pcm16_mono", SCALE(300u), 1u, SOUND_ENCODING_PCM16,
                              BENCH_FRAMES, false);
    if (WantWorkload(filter, "la_pcm16_stereo"))
        PrepareEncodeWorkload("la_pcm16_stereo", SCALE(200u), 2u, SOUND_ENCODING_PCM16,
                              BENCH_FRAMES, false);
    if (WantWorkload(filter, "la_adpcm_mono"))
        PrepareEncodeWorkload("la_adpcm_mono", SCALE(32u), 1u, SOUND_ENCODING_ADPCM, BENCH_FRAMES,
                              false);
    if (WantWorkload(filter, "la_adpcm_stereo"))
        PrepareEncodeWorkload("la_adpcm_stereo", SCALE(16u), 2u, SOUND_ENCODING_ADPCM, BENCH_FRAMES,
                              false);
    if (WantWorkload(filter, "la_adpcm_adv"))
        PrepareEncodeWorkload("la_adpcm_adv", SCALE(32u), 1u, SOUND_ENCODING_ADPCM, BENCH_FRAMES,
                              true);
    if (WantWorkload(filter, "la_adpcm_odd"))
        PrepareEncodeWorkload("la_adpcm_odd", SCALE(32u), 1u, SOUND_ENCODING_ADPCM,
                              BENCH_FRAMES - 1u, false);
    if (filter[0] != '\0' && g_workloadCount == 0u) Fail("unknown workload");

    for (uint32_t index = 0u; index < g_workloadCount; ++index)
    {
        Workload *workload = &g_workloads[index];
        // Прогрев вне статистики.
        RunWorkload(workload, workload->repeats);
        for (uint32_t sample = 0u; sample < samples; ++sample)
        {
            double seconds = RunWorkload(workload, workload->repeats);
            if (workload->sampleCount < BENCH_MAX_SAMPLES)
            {
                workload->samples[workload->sampleCount++] = seconds * 1000.0;
            }
            WriteText("r2sound workload=");
            WriteText(workload->name);
            WriteText(" sample=");
            WriteUnsigned(sample);
            WriteText(" ms=");
            WriteFixed(seconds * 1000.0);
            WriteText(" checksum=");
            WriteHex(workload->checksum);
            WriteText(" reps=");
            WriteUnsigned(workload->repeats);
            WriteText("\n");
        }
        Report(workload);
    }

    PlatformFree(g_samples);
    PlatformFree(g_decoded);
    PlatformFree(g_wave);
    PlatformFree(g_encoded);
    WriteText("r2sound done sink=");
    WriteUnsigned(g_sink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
    (void)WantWorkload;
}
