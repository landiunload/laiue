// ROUND2 30-sound: регрессия на запись `.la`.
//
// Проверяется именно то, что изменил ROUND2 в la_encode.c:
//   * PCM16 payload обязан побайтово совпадать с little-endian записью
//     массива сэмплов (ускоренная копия не имеет права менять байты);
//   * упаковка нибблов ADPCM: младший ниббл — чётный кадр, старший —
//     нечётный, а у нечётного числа кадров старший ниббл последнего байта
//     обязан быть нулевым (прежний обнулённый буфер давал ровно это).
//
// ADPCM распаковывается независимым декодером прямо здесь: таблицы IMA
// держит и декодер движка, и этот тест, как и audio_pack_test.c, намеренно.
// Круг «кодировщик — независимый декодер» ловит ошибку упаковки, которую
// симметричная проверка не увидела бы.

#include "media/la_encode.h"

#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEST_MAX_FRAMES 64u
#define TEST_MAX_CHANNELS 2u
#define TEST_SAMPLE_RATE 48000u
#define TEST_CAPACITY (SOUND_LA_HEADER_BYTES + TEST_MAX_FRAMES * TEST_MAX_CHANNELS * 2u)

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

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("r2_30_sound test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint16_t ReadU16Le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8));
}

static uint32_t ReadU32Le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int16_t AdpcmDecodeNibble(AdpcmState *state, uint32_t nibble)
{
    int32_t step = ADPCM_STEP_TABLE[state->stepIndex];

    int32_t difference = step >> 3;
    if (nibble & 4u) difference += step;
    if (nibble & 2u) difference += step >> 1;
    if (nibble & 1u) difference += step >> 2;
    if (nibble & 8u) difference = -difference;

    int32_t predictor = state->predictor + difference;
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    state->predictor = predictor;

    int32_t stepIndex = state->stepIndex + ADPCM_INDEX_TABLE[nibble & 15u];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    state->stepIndex = stepIndex;
    return (int16_t)predictor;
}

static uint32_t AdpcmChannelBytes(uint32_t frameCount)
{
    return (frameCount + 1u) / 2u;
}

static void FillSignal(int16_t *samples, uint32_t frameCount, uint32_t channelCount)
{
    int32_t value = 0;
    int32_t direction = 256;
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        for (uint32_t channel = 0; channel < channelCount; ++channel)
        {
            int16_t sample = (int16_t)(channel == 0u ? value : -value);
            samples[(size_t)frame * channelCount + channel] = sample;
        }
        value += direction;
        if (value >= 8192 || value <= -8192) direction = -direction;
    }
}

static int32_t AbsoluteDifference(int32_t left, int32_t right)
{
    int32_t difference = left - right;
    return difference < 0 ? -difference : difference;
}

static void EncodeClip(const int16_t *samples, uint32_t frameCount, uint32_t channelCount,
                       SoundEncoding encoding, uint8_t *outBytes, uint32_t *outWritten)
{
    SoundClip clip = {
        .samples = samples,
        .frameCount = frameCount,
        .channelCount = channelCount,
        .sampleRate = TEST_SAMPLE_RATE,
        .encoding = encoding,
        .sourceModifiedTime = 0u,
        .sourceSizeBytes = 0u,
    };
    uint32_t expected = 0u;
    Expect(SoundEncodedBytes(encoding, frameCount, channelCount, &expected) == SOUND_OK,
           "the encoded size must be computable");
    Expect(expected <= TEST_CAPACITY, "the test buffer is too small");
    uint32_t written = 0u;
    Expect(SoundEncode(&clip, outBytes, TEST_CAPACITY, &written) == SOUND_OK,
           "encoding must succeed");
    Expect(written == expected, "the written size must match the computed size");
    *outWritten = written;
}

static void CheckHeader(const uint8_t *file, uint32_t channelCount, SoundEncoding encoding,
                        uint32_t frameCount, uint32_t payloadBytes)
{
    Expect(ReadU32Le(file) == 0x3153414Cu, "the magic must be LAS1");
    Expect(ReadU16Le(file + 4) == SOUND_LA_VERSION, "the version must be current");
    Expect(ReadU16Le(file + 6) == SOUND_LA_HEADER_BYTES, "the header size must be current");
    Expect(ReadU16Le(file + 8) == channelCount, "the channel count must round trip");
    Expect(ReadU16Le(file + 10) == (uint16_t)encoding, "the encoding must round trip");
    Expect(ReadU32Le(file + 12) == TEST_SAMPLE_RATE, "the sample rate must round trip");
    Expect(ReadU32Le(file + 16) == frameCount, "the frame count must round trip");
    Expect(ReadU32Le(file + 20) == payloadBytes, "the payload size must round trip");
}

static void CheckPcm16Exact(const uint8_t *file, const int16_t *samples, uint32_t frameCount,
                            uint32_t channelCount)
{
    uint32_t count = frameCount * channelCount;
    CheckHeader(file, channelCount, SOUND_ENCODING_PCM16, frameCount, count * 2u);
    const uint8_t *payload = file + SOUND_LA_HEADER_BYTES;
    for (uint32_t index = 0; index < count; ++index)
    {
        // payload обязан быть little-endian представлением int16.
        uint32_t expected = (uint32_t)(uint16_t)samples[index];
        Expect(payload[index * 2u] == (uint8_t)(expected & 0xFFu) &&
                   payload[index * 2u + 1u] == (uint8_t)(expected >> 8),
               "pcm16 payload must be the exact little-endian samples");
    }
}

static void CheckAdpcmRoundTrip(const uint8_t *file, const int16_t *samples, uint32_t frameCount,
                                uint32_t channelCount)
{
    uint32_t payloadBytes = channelCount * (4u + AdpcmChannelBytes(frameCount));
    CheckHeader(file, channelCount, SOUND_ENCODING_ADPCM, frameCount, payloadBytes);

    const uint8_t *payload = file + SOUND_LA_HEADER_BYTES;
    const uint8_t *cursor = payload;
    int32_t worstError = 0;
    for (uint32_t channel = 0; channel < channelCount; ++channel)
    {
        AdpcmState state = {
            .predictor = (int16_t)ReadU16Le(cursor),
            .stepIndex = (int16_t)ReadU16Le(cursor + 2),
        };
        Expect(state.stepIndex >= 0 && state.stepIndex <= 88, "the step index must be valid");
        cursor += 4u;

        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            uint8_t packed = cursor[frame / 2u];
            uint32_t nibble =
                (frame & 1u) != 0u ? (uint32_t)(packed >> 4) : (uint32_t)(packed & 15u);
            int16_t decoded = AdpcmDecodeNibble(&state, nibble);
            int32_t error =
                AbsoluteDifference(decoded, samples[(size_t)frame * channelCount + channel]);
            if (error > worstError) worstError = error;
        }

        // Нечётный хвост: последний кадр лежит в младшем ниббле, а старший
        // обязан остаться нулевым, иначе в декодер уехал бы выдуманный кадр.
        if ((frameCount & 1u) != 0u)
        {
            Expect((cursor[AdpcmChannelBytes(frameCount) - 1u] & 0xF0u) == 0u,
                   "an odd tail must leave the high nibble zero");
        }
        cursor += AdpcmChannelBytes(frameCount);
    }

    // На треугольной волне кодек даёт заметную, но не катастрофическую
    // ошибку. Порог ловит перепутанный ниббл или потерянный кадр.
    // Единственный кадр воспроизводится точно: шаг ещё нулевой, а
    // предсказатель равен сэмплу, — это не признак ошибки.
    if (frameCount > 1u) Expect(worstError > 0, "a lossy codec that is exact is suspicious");
    Expect(worstError < 600, "the adpcm round trip must stay close to the input");
}

LAIUE_TEST_ENTRY(R2SoundTestEntryPoint)
{
    int16_t *samples = (int16_t *)PlatformAllocate(sizeof(int16_t) * TEST_MAX_FRAMES * TEST_MAX_CHANNELS, true);
    uint8_t *file = (uint8_t *)PlatformAllocate(TEST_CAPACITY, false);
    uint8_t *again = (uint8_t *)PlatformAllocate(TEST_CAPACITY, false);
    Expect(samples != NULL && file != NULL && again != NULL, "buffers could not be allocated");

    // === PCM16: точные байты, включая нечётное общее число сэмплов ===
    for (uint32_t channelCount = 1u; channelCount <= TEST_MAX_CHANNELS; ++channelCount)
    {
        for (uint32_t frameCount = 1u; frameCount <= 9u; ++frameCount)
        {
            FillSignal(samples, frameCount, channelCount);
            uint32_t written = 0u;
            EncodeClip(samples, frameCount, channelCount, SOUND_ENCODING_PCM16, file, &written);
            CheckPcm16Exact(file, samples, frameCount, channelCount);
        }
    }

    // === ADPCM: упаковка нибблов и хвост, mono/stereo, чёт/нечет ===
    static const uint32_t frameCounts[] = {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 17u};
    for (uint32_t channelCount = 1u; channelCount <= TEST_MAX_CHANNELS; ++channelCount)
    {
        for (uint32_t index = 0u; index < sizeof(frameCounts) / sizeof(frameCounts[0]); ++index)
        {
            uint32_t frameCount = frameCounts[index];
            FillSignal(samples, frameCount, channelCount);

            uint32_t written = 0u;
            EncodeClip(samples, frameCount, channelCount, SOUND_ENCODING_ADPCM, file, &written);
            CheckAdpcmRoundTrip(file, samples, frameCount, channelCount);

            // Повторный проход обязан дать те же байты: кодировщик без
            // скрытого состояния между вызовами.
            uint32_t repeated = 0u;
            EncodeClip(samples, frameCount, channelCount, SOUND_ENCODING_ADPCM, again, &repeated);
            Expect(repeated == written, "the encoded size must be reproducible");
            for (uint32_t byte = 0u; byte < written; ++byte)
            {
                Expect(file[byte] == again[byte], "encoding must be deterministic");
            }
        }
    }

    PlatformFree(samples);
    PlatformFree(file);
    PlatformFree(again);
    LaiueTestRuntimeWrite("r2_30_sound test passed\n");
    LAIUE_TEST_SUCCESS();
}
