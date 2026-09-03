#include "la_encode.h"

#include <stddef.h>

#define LA_MAGIC 0x3153414Cu   // L, A, S, 1 little-endian
#define LA_VERSION 1u

#define LA_MIN_SAMPLE_RATE 1000u
#define LA_MAX_SAMPLE_RATE 384000u
#define LA_MAX_FILE_BYTES 0x20000000u

// === IMA ADPCM ===
//
// Таблицы совпадают с декодерными в src/audio/audio_pack.c и с копией в
// tests/audio_pack_test.c. Это стандартные таблицы IMA, и каждая из трёх
// сторон держит свою намеренно: декодер проверяется независимым
// кодировщиком теста, а этот кодировщик — уже проверенным декодером.
// Общая таблица сделала бы обе проверки слепыми к ошибке в ней самой.

static const int32_t ADPCM_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int32_t ADPCM_STEP_TABLE[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,
    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767,
};

typedef struct AdpcmState
{
    int32_t predictor;
    int32_t stepIndex;
} AdpcmState;

static void WriteU16Le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void WriteU32Le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t AdpcmChannelBytes(uint32_t frameCount)
{
    return (frameCount + 1u) / 2u;
}

static uint32_t EncodeNibble(AdpcmState *state, int32_t sample)
{
    int32_t step = ADPCM_STEP_TABLE[state->stepIndex];
    int32_t difference = sample - state->predictor;

    uint32_t nibble = 0u;
    if (difference < 0)
    {
        nibble = 8u;
        difference = -difference;
    }

    int32_t reconstructed = step >> 3;
    if (difference >= step)
    {
        nibble |= 4u;
        difference -= step;
        reconstructed += step;
    }
    if (difference >= (step >> 1))
    {
        nibble |= 2u;
        difference -= step >> 1;
        reconstructed += step >> 1;
    }
    if (difference >= (step >> 2))
    {
        nibble |= 1u;
        reconstructed += step >> 2;
    }

    // Предсказатель обновляется по восстановленному значению, а не по
    // исходному: декодер другого не знает, и расхождение накапливалось бы.
    int32_t predictor = state->predictor + ((nibble & 8u) != 0u ? -reconstructed : reconstructed);
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    state->predictor = predictor;

    int32_t stepIndex = state->stepIndex + ADPCM_INDEX_TABLE[nibble];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    state->stepIndex = stepIndex;
    return nibble;
}

// Начальный шаг выбирается по первым перепадам. Он хранится в файле
// именно затем, чтобы кодировщик мог это сделать: со шага 7 декодер
// догонял бы быстрый сигнал десяток отсчётов, и весь разгон был бы
// слышен как щелчок в начале звука.
static int32_t ChooseInitialStepIndex(const int16_t *samples, uint32_t frameCount, uint32_t stride)
{
    int32_t largestDelta = 0;
    uint32_t inspected = frameCount < 16u ? frameCount : 16u;
    for (uint32_t index = 1; index < inspected; ++index)
    {
        int32_t delta = (int32_t)samples[(size_t)index * stride] -
                        (int32_t)samples[(size_t)(index - 1u) * stride];
        if (delta < 0) delta = -delta;
        if (delta > largestDelta) largestDelta = delta;
    }
    for (int32_t index = 0; index < 88; ++index)
    {
        // Максимум, представимый одним полубайтом, — 1,875 шага.
        if (ADPCM_STEP_TABLE[index] * 15 / 8 >= largestDelta) return index;
    }
    return 88;
}

// Каналы лежат последовательно, а не чередуются: клип распаковывается
// целиком при загрузке, точки синхронизации не нужны, а декодировать так
// проще. Исходные сэмплы при этом чередуются, поэтому канал читается с
// шагом channelCount.
static uint32_t EncodeAdpcmChannel(const int16_t *samples, uint32_t frameCount, uint32_t stride,
                                   uint8_t *payload)
{
    AdpcmState state = {
        .predictor = samples[0],
        .stepIndex = ChooseInitialStepIndex(samples, frameCount, stride),
    };
    WriteU16Le(payload, (uint32_t)(uint16_t)(int16_t)state.predictor);
    WriteU16Le(payload + 2, (uint32_t)state.stepIndex);

    uint8_t *nibbles = payload + 4;
    uint32_t byteCount = AdpcmChannelBytes(frameCount);
    for (uint32_t index = 0; index < byteCount; ++index) nibbles[index] = 0u;
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        uint32_t nibble = EncodeNibble(&state, samples[(size_t)frame * stride]);
        if ((frame & 1u) != 0u) nibbles[frame / 2u] |= (uint8_t)(nibble << 4);
        else nibbles[frame / 2u] |= (uint8_t)nibble;
    }
    return 4u + byteCount;
}

static LaStatus PayloadBytes(LaEncoding encoding, uint32_t frameCount, uint32_t channelCount,
                             uint32_t *outBytes)
{
    if (frameCount == 0u || (channelCount != 1u && channelCount != 2u)) return LA_INVALID_ARGUMENT;
    if (frameCount > LA_MAX_FRAMES) return LA_TOO_LARGE;

    uint64_t total = encoding == LA_ENCODING_PCM16
                         ? (uint64_t)frameCount * channelCount * sizeof(int16_t)
                         : (uint64_t)channelCount * (4u + (uint64_t)AdpcmChannelBytes(frameCount));
    if (total + LA_HEADER_BYTES > LA_MAX_FILE_BYTES) return LA_TOO_LARGE;
    *outBytes = (uint32_t)total;
    return LA_OK;
}

LaStatus LaEncodedBytes(LaEncoding encoding, uint32_t frameCount, uint32_t channelCount,
                        uint32_t *outBytes)
{
    if (outBytes == NULL) return LA_INVALID_ARGUMENT;
    if (encoding != LA_ENCODING_PCM16 && encoding != LA_ENCODING_ADPCM) return LA_INVALID_ARGUMENT;

    uint32_t payload = 0u;
    LaStatus status = PayloadBytes(encoding, frameCount, channelCount, &payload);
    if (status != LA_OK) return status;
    *outBytes = LA_HEADER_BYTES + payload;
    return LA_OK;
}

LaStatus LaEncode(const int16_t *samples, uint32_t frameCount, uint32_t channelCount,
                  uint32_t sampleRate, LaEncoding encoding, void *outBytes,
                  uint32_t capacityBytes, uint32_t *outWritten)
{
    if (samples == NULL || outBytes == NULL) return LA_INVALID_ARGUMENT;
    if (encoding != LA_ENCODING_PCM16 && encoding != LA_ENCODING_ADPCM) return LA_INVALID_ARGUMENT;
    if (sampleRate < LA_MIN_SAMPLE_RATE || sampleRate > LA_MAX_SAMPLE_RATE)
        return LA_INVALID_ARGUMENT;

    uint32_t payloadBytes = 0u;
    LaStatus status = PayloadBytes(encoding, frameCount, channelCount, &payloadBytes);
    if (status != LA_OK) return status;
    if (capacityBytes < LA_HEADER_BYTES + payloadBytes) return LA_BUFFER_TOO_SMALL;

    uint8_t *file = (uint8_t *)outBytes;
    WriteU32Le(file, LA_MAGIC);
    WriteU16Le(file + 4, LA_VERSION);
    WriteU16Le(file + 6, LA_HEADER_BYTES);
    WriteU16Le(file + 8, channelCount);
    WriteU16Le(file + 10, (uint32_t)encoding);
    WriteU32Le(file + 12, sampleRate);
    WriteU32Le(file + 16, frameCount);
    WriteU32Le(file + 20, payloadBytes);

    uint8_t *payload = file + LA_HEADER_BYTES;
    if (encoding == LA_ENCODING_PCM16)
    {
        // Сэмплы пишутся побайтово, чтобы файл не зависел от порядка
        // байтов машины, на которой собран пак.
        for (uint32_t index = 0; index < frameCount * channelCount; ++index)
        {
            WriteU16Le(payload + (size_t)index * 2u, (uint32_t)(uint16_t)samples[index]);
        }
    }
    else
    {
        for (uint32_t channel = 0; channel < channelCount; ++channel)
        {
            payload += EncodeAdpcmChannel(samples + channel, frameCount, channelCount, payload);
        }
    }

    if (outWritten != NULL) *outWritten = LA_HEADER_BYTES + payloadBytes;
    return LA_OK;
}

const char *LaStatusText(LaStatus status)
{
    switch (status)
    {
    case LA_OK: return "ok";
    case LA_INVALID_ARGUMENT: return "invalid argument";
    case LA_TOO_LARGE: return "the sound exceeds the container limits";
    case LA_BUFFER_TOO_SMALL: return "the output buffer is too small";
    }
    return "unknown error";
}
