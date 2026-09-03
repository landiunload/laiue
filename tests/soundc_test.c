// Конвертер WAV в `.la`: разбор входных форматов, отказ на том, что
// разобрать нельзя, и круговой проход кодировщик — декодер движка.
//
// Проверяется ровно тот код, который выполняет инструмент: тест линкует
// его ядро, а не повторяет логику. Круг замыкается настоящим декодером
// из laiue_audio — так ошибка в кодировщике не может спрятаться за
// симметричной ошибкой в проверке.

#include "la_encode.h"
#include "wave_decode.h"

#include "platform/system.h"
#include "test_runtime.h"

#if defined(LAIUE_SOUNDC_TEST_WITH_AUDIO)
#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "audio/audio_pack.h"
#endif

#include <stdbool.h>
#include <stdint.h>

#define TEST_FRAMES 512u
#define TEST_SAMPLE_RATE 48000u
#define WAVE_CAPACITY 16384u

// Панорама равной мощности сохраняет суммарную мощность, а не амплитуду
// канала: голос по центру звучит в каждом канале с усилением 1/sqrt(2).
#define CENTRE_GAIN 0.70710678f

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("soundc test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

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
    for (uint32_t index = 0; index < 4u; ++index) bytes[index] = (uint8_t)tag[index];
}

static int32_t AbsoluteDifference(int32_t left, int32_t right)
{
    int32_t difference = left - right;
    return difference < 0 ? -difference : difference;
}

// Треугольная волна: у неё предсказуемый спектр и она хорошо ловит
// ошибку шага ADPCM, в отличие от постоянного уровня.
static void FillTriangle(int16_t *samples, uint32_t frameCount)
{
    int32_t value = 0;
    int32_t direction = 512;
    for (uint32_t index = 0; index < frameCount; ++index)
    {
        samples[index] = (int16_t)value;
        value += direction;
        if (value >= 8192 || value <= -8192) direction = -direction;
    }
}

// Собирает WAV в буфер. Ведущий чанк намеренно нечётной длины: по
// стандарту он дополняется до чётной границы, и разбор обязан это учесть.
static uint32_t BuildWave(uint8_t *out, uint32_t formatTag, uint32_t subFormatTag,
                          uint32_t channelCount, uint32_t sampleRate, uint32_t bitsPerSample,
                          const void *data, uint32_t dataBytes, uint32_t leadingChunkBytes)
{
    PutTag(out, "RIFF");
    PutTag(out + 8, "WAVE");
    uint32_t cursor = 12u;

    if (leadingChunkBytes != 0u)
    {
        PutTag(out + cursor, "LIST");
        PutU32(out + cursor + 4, leadingChunkBytes);
        for (uint32_t index = 0; index < leadingChunkBytes; ++index)
        {
            out[cursor + 8u + index] = (uint8_t)index;
        }
        if ((leadingChunkBytes & 1u) != 0u) out[cursor + 8u + leadingChunkBytes] = 0u;
        cursor += 8u + leadingChunkBytes + (leadingChunkBytes & 1u);
    }

    uint32_t formatBytes = formatTag == 0xFFFEu ? 40u : 16u;
    PutTag(out + cursor, "fmt ");
    PutU32(out + cursor + 4, formatBytes);
    uint8_t *format = out + cursor + 8u;
    uint32_t blockAlign = channelCount * (bitsPerSample / 8u);
    PutU16(format, formatTag);
    PutU16(format + 2, channelCount);
    PutU32(format + 4, sampleRate);
    PutU32(format + 8, sampleRate * blockAlign);
    PutU16(format + 12, blockAlign);
    PutU16(format + 14, bitsPerSample);
    if (formatBytes == 40u)
    {
        PutU16(format + 16, 22u);              // cbSize
        PutU16(format + 18, bitsPerSample);    // validBitsPerSample
        PutU32(format + 20, 0u);               // channelMask
        PutU16(format + 24, subFormatTag);     // первые два байта SubFormat GUID
        for (uint32_t index = 26u; index < 40u; ++index) format[index] = 0u;
    }
    cursor += 8u + formatBytes;

    PutTag(out + cursor, "data");
    PutU32(out + cursor + 4, dataBytes);
    const uint8_t *source = (const uint8_t *)data;
    for (uint32_t index = 0; index < dataBytes; ++index)
    {
        out[cursor + 8u + index] = source[index];
    }
    cursor += 8u + dataBytes;

    PutU32(out + 4, cursor - 8u);
    return cursor;
}

static WaveStatus InspectStatus(const uint8_t *bytes, uint32_t sizeBytes)
{
    WaveInfo info;
    return WaveInspect(bytes, sizeBytes, &info);
}

typedef struct TestBuffers
{
    int16_t reference[TEST_FRAMES];
    int16_t decoded[TEST_FRAMES * 2u];
    int16_t stereo[TEST_FRAMES * 2u];
    uint8_t wave[WAVE_CAPACITY];
    uint8_t payload[TEST_FRAMES * 2u * 8u];
    uint8_t encoded[LA_HEADER_BYTES + TEST_FRAMES * 2u * 2u];
} TestBuffers;

LAIUE_TEST_ENTRY(SoundcTestEntryPoint)
{
    // Буферы живут в куче: вместе они переполнили бы страницу стека, а в
    // сборке без CRT нет __chkstk, чтобы её нарастить.
    TestBuffers *buffers = PlatformAllocate(sizeof(*buffers), true);
    Expect(buffers != NULL, "test buffers could not be allocated");
    FillTriangle(buffers->reference, TEST_FRAMES);

    // === PCM16 моно читается точно ===
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        PutU16(buffers->payload + index * 2u, (uint32_t)(uint16_t)buffers->reference[index]);
    }
    uint32_t waveBytes = BuildWave(buffers->wave, 1u, 0u, 1u, TEST_SAMPLE_RATE, 16u,
                                   buffers->payload, TEST_FRAMES * 2u, 5u);
    Expect(waveBytes <= WAVE_CAPACITY, "the wave buffer is too small for the test");

    WaveInfo info;
    Expect(WaveInspect(buffers->wave, waveBytes, &info) == WAVE_OK, "pcm16 mono must be accepted");
    Expect(info.frameCount == TEST_FRAMES && info.channelCount == 1u &&
               info.sampleRate == TEST_SAMPLE_RATE && info.bitsPerSample == 16u && !info.isFloat,
           "the pcm16 header must be read exactly");
    Expect(WaveDecodeSamples(buffers->wave, waveBytes, &info, buffers->decoded, TEST_FRAMES) ==
               WAVE_OK,
           "pcm16 decoding must succeed");
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        Expect(buffers->decoded[index] == buffers->reference[index],
               "pcm16 must survive the round trip unchanged");
    }

    // Усечённый хвост data берётся как есть: так пишут те, кто не дописал
    // заголовок после потоковой записи, и терять из-за этого весь файл
    // незачем.
    Expect(WaveInspect(buffers->wave, waveBytes - 100u, &info) == WAVE_OK,
           "a truncated data tail must still decode");
    Expect(info.frameCount == TEST_FRAMES - 50u, "the truncated tail must shorten the sound");

    // === 8 бит стерео: беззнаковый источник и правильные каналы ===
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        int32_t level = buffers->reference[index] / 256;
        buffers->payload[index * 2u] = (uint8_t)(level + 128);
        buffers->payload[index * 2u + 1u] = (uint8_t)(-level + 128);
    }
    waveBytes = BuildWave(buffers->wave, 1u, 0u, 2u, TEST_SAMPLE_RATE, 8u, buffers->payload,
                          TEST_FRAMES * 2u, 0u);
    Expect(WaveInspect(buffers->wave, waveBytes, &info) == WAVE_OK, "pcm8 stereo must be accepted");
    Expect(info.frameCount == TEST_FRAMES && info.channelCount == 2u && info.bitsPerSample == 8u,
           "the pcm8 header must be read exactly");
    Expect(WaveDecodeSamples(buffers->wave, waveBytes, &info, buffers->decoded, TEST_FRAMES * 2u) ==
               WAVE_OK,
           "pcm8 decoding must succeed");
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        int32_t level = buffers->reference[index] / 256;
        Expect(buffers->decoded[index * 2u] == (int16_t)(level * 256),
               "pcm8 must be shifted from unsigned to signed");
        Expect(buffers->decoded[index * 2u + 1u] == (int16_t)(-level * 256),
               "pcm8 channels must not be swapped");
    }

    // === 24 бита: старшие 16 берутся с округлением ===
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        int32_t value = (int32_t)buffers->reference[index] * 256;
        buffers->payload[index * 3u] = (uint8_t)(value & 0xFF);
        buffers->payload[index * 3u + 1u] = (uint8_t)((value >> 8) & 0xFF);
        buffers->payload[index * 3u + 2u] = (uint8_t)((value >> 16) & 0xFF);
    }
    waveBytes = BuildWave(buffers->wave, 1u, 0u, 1u, TEST_SAMPLE_RATE, 24u, buffers->payload,
                          TEST_FRAMES * 3u, 0u);
    Expect(WaveInspect(buffers->wave, waveBytes, &info) == WAVE_OK, "pcm24 must be accepted");
    Expect(WaveDecodeSamples(buffers->wave, waveBytes, &info, buffers->decoded, TEST_FRAMES) ==
               WAVE_OK,
           "pcm24 decoding must succeed");
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        Expect(buffers->decoded[index] == buffers->reference[index],
               "pcm24 must scale down to the original signal");
    }

    // === float 32: диапазон [-1, 1] ===
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        union
        {
            float value;
            uint32_t bits;
        } cast;
        cast.value = (float)buffers->reference[index] / 32767.0f;
        PutU32(buffers->payload + index * 4u, cast.bits);
    }
    waveBytes = BuildWave(buffers->wave, 3u, 0u, 1u, TEST_SAMPLE_RATE, 32u, buffers->payload,
                          TEST_FRAMES * 4u, 0u);
    Expect(WaveInspect(buffers->wave, waveBytes, &info) == WAVE_OK, "float32 must be accepted");
    Expect(info.isFloat, "the float format must be recognised as such");
    Expect(WaveDecodeSamples(buffers->wave, waveBytes, &info, buffers->decoded, TEST_FRAMES) ==
               WAVE_OK,
           "float32 decoding must succeed");
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        Expect(AbsoluteDifference(buffers->decoded[index], buffers->reference[index]) <= 1,
               "float32 must round back to the original signal");
    }

    // === float 64 ===
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        union
        {
            double value;
            uint64_t bits;
        } cast;
        cast.value = (double)buffers->reference[index] / 32767.0;
        PutU32(buffers->payload + index * 8u, (uint32_t)(cast.bits & 0xFFFFFFFFu));
        PutU32(buffers->payload + index * 8u + 4u, (uint32_t)(cast.bits >> 32));
    }
    waveBytes = BuildWave(buffers->wave, 3u, 0u, 1u, TEST_SAMPLE_RATE, 64u, buffers->payload,
                          TEST_FRAMES * 8u, 0u);
    Expect(WaveInspect(buffers->wave, waveBytes, &info) == WAVE_OK, "float64 must be accepted");
    Expect(WaveDecodeSamples(buffers->wave, waveBytes, &info, buffers->decoded, TEST_FRAMES) ==
               WAVE_OK,
           "float64 decoding must succeed");
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        Expect(AbsoluteDifference(buffers->decoded[index], buffers->reference[index]) <= 1,
               "float64 must round back to the original signal");
    }

    // === WAVE_FORMAT_EXTENSIBLE: настоящий формат лежит в GUID ===
    waveBytes = BuildWave(buffers->wave, 0xFFFEu, 1u, 2u, TEST_SAMPLE_RATE, 16u, buffers->payload,
                          TEST_FRAMES * 4u, 0u);
    Expect(WaveInspect(buffers->wave, waveBytes, &info) == WAVE_OK,
           "extensible pcm must be accepted");
    Expect(info.channelCount == 2u && info.frameCount == TEST_FRAMES,
           "the extensible header must be read exactly");
    waveBytes = BuildWave(buffers->wave, 0xFFFEu, 2u, 2u, TEST_SAMPLE_RATE, 16u, buffers->payload,
                          TEST_FRAMES * 4u, 0u);
    Expect(InspectStatus(buffers->wave, waveBytes) == WAVE_UNSUPPORTED_FORMAT,
           "a compressed subformat must be named, not guessed");

    // === Отказы ===
    waveBytes = BuildWave(buffers->wave, 1u, 0u, 1u, TEST_SAMPLE_RATE, 16u, buffers->payload,
                          TEST_FRAMES * 2u, 0u);
    PutTag(buffers->wave, "RIFX");
    Expect(InspectStatus(buffers->wave, waveBytes) == WAVE_NOT_RIFF,
           "a foreign container must be rejected");

    waveBytes = BuildWave(buffers->wave, 2u, 0u, 1u, TEST_SAMPLE_RATE, 16u, buffers->payload,
                          TEST_FRAMES * 2u, 0u);
    Expect(InspectStatus(buffers->wave, waveBytes) == WAVE_UNSUPPORTED_FORMAT,
           "microsoft adpcm must be rejected with a clear status");

    waveBytes = BuildWave(buffers->wave, 1u, 0u, 3u, TEST_SAMPLE_RATE, 16u, buffers->payload,
                          TEST_FRAMES * 2u, 0u);
    Expect(InspectStatus(buffers->wave, waveBytes) == WAVE_UNSUPPORTED_CHANNELS,
           "more than two channels must be rejected");

    waveBytes = BuildWave(buffers->wave, 1u, 0u, 1u, TEST_SAMPLE_RATE, 12u, buffers->payload,
                          TEST_FRAMES * 2u, 0u);
    Expect(InspectStatus(buffers->wave, waveBytes) == WAVE_UNSUPPORTED_DEPTH,
           "an exotic depth must be rejected");

    waveBytes = BuildWave(buffers->wave, 1u, 0u, 1u, 100u, 16u, buffers->payload, TEST_FRAMES * 2u,
                          0u);
    Expect(InspectStatus(buffers->wave, waveBytes) == WAVE_UNSUPPORTED_RATE,
           "an impossible sample rate must be rejected");

    waveBytes = BuildWave(buffers->wave, 1u, 0u, 1u, TEST_SAMPLE_RATE, 16u, buffers->payload, 0u,
                          0u);
    Expect(InspectStatus(buffers->wave, waveBytes) == WAVE_MISSING_DATA,
           "a file without frames must be rejected");

    // Обрезанный не-data чанк — повреждение, а не потоковая запись.
    waveBytes = BuildWave(buffers->wave, 1u, 0u, 1u, TEST_SAMPLE_RATE, 16u, buffers->payload,
                          TEST_FRAMES * 2u, 64u);
    Expect(InspectStatus(buffers->wave, 12u + 8u + 32u) == WAVE_TRUNCATED,
           "a chunk cut in half must be rejected");

    // === Размеры контейнера ===
    uint32_t encodedBytes = 0u;
    Expect(LaEncodedBytes(LA_ENCODING_PCM16, TEST_FRAMES, 1u, &encodedBytes) == LA_OK &&
               encodedBytes == LA_HEADER_BYTES + TEST_FRAMES * 2u,
           "the pcm16 size must follow from the frame count");
    Expect(LaEncodedBytes(LA_ENCODING_ADPCM, TEST_FRAMES, 1u, &encodedBytes) == LA_OK &&
               encodedBytes == LA_HEADER_BYTES + 4u + TEST_FRAMES / 2u,
           "the adpcm size must follow from the frame count");
    Expect(LaEncodedBytes(LA_ENCODING_ADPCM, TEST_FRAMES, 2u, &encodedBytes) == LA_OK &&
               encodedBytes == LA_HEADER_BYTES + 2u * (4u + TEST_FRAMES / 2u),
           "each adpcm channel carries its own state");
    Expect(LaEncodedBytes(LA_ENCODING_PCM16, 0u, 1u, &encodedBytes) == LA_INVALID_ARGUMENT,
           "an empty sound must be rejected");
    Expect(LaEncode(buffers->reference, TEST_FRAMES, 1u, 100u, LA_ENCODING_PCM16, buffers->encoded,
                    sizeof(buffers->encoded), NULL) == LA_INVALID_ARGUMENT,
           "an impossible sample rate must be rejected by the encoder too");
    Expect(LaEncode(buffers->reference, TEST_FRAMES, 1u, TEST_SAMPLE_RATE, LA_ENCODING_PCM16,
                    buffers->encoded, LA_HEADER_BYTES, NULL) == LA_BUFFER_TOO_SMALL,
           "a short buffer must be reported, not overrun");

#if defined(LAIUE_SOUNDC_TEST_WITH_AUDIO)
    // === Круг замыкается декодером движка ===
    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = TEST_SAMPLE_RATE,
        .frameCountHint = TEST_FRAMES,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
           "offscreen device could not be created");

    float *frames = PlatformAllocate(TEST_FRAMES * 2u * sizeof(float), true);
    Expect(frames != NULL, "mix buffer could not be allocated");

    uint32_t written = 0u;
    Expect(LaEncode(buffers->reference, TEST_FRAMES, 1u, TEST_SAMPLE_RATE, LA_ENCODING_PCM16,
                    buffers->encoded, sizeof(buffers->encoded), &written) == LA_OK,
           "pcm16 encoding must succeed");

    AudioPackLoadStatus status = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    AudioClip *clip = AudioClipLoadMemory(device, buffers->encoded, written, &status);
    Expect(clip != NULL && status == AUDIO_PACK_LOAD_OK, "the engine must accept the written file");
    Expect(AudioVoicePlay(device, clip, NULL) != AUDIO_VOICE_NONE, "the clip must be playable");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        int32_t decoded = (int32_t)(frames[index * 2u] / CENTRE_GAIN * 32768.0f);
        Expect(AbsoluteDifference(decoded, buffers->reference[index]) <= 2,
               "pcm16 must reach the mixer unchanged");
    }
    AudioClipDestroy(clip);
    AudioDeviceStopAllVoices(device);
    AudioDeviceRenderFrames(device, frames, TEST_FRAMES);

    // ADPCM стерео: каналы кодируются по отдельности, поэтому проверяется
    // и то, что они не перепутались.
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        buffers->stereo[index * 2u] = buffers->reference[index];
        buffers->stereo[index * 2u + 1u] = (int16_t)(-buffers->reference[index]);
    }
    Expect(LaEncode(buffers->stereo, TEST_FRAMES, 2u, TEST_SAMPLE_RATE, LA_ENCODING_ADPCM,
                    buffers->encoded, sizeof(buffers->encoded), &written) == LA_OK,
           "adpcm encoding must succeed");
    Expect(written * 4u < LA_HEADER_BYTES + TEST_FRAMES * 4u + 64u,
           "adpcm must be about four times smaller than pcm16");

    clip = AudioClipLoadMemory(device, buffers->encoded, written, &status);
    Expect(clip != NULL && status == AUDIO_PACK_LOAD_OK, "the engine must accept the adpcm file");
    Expect(AudioVoicePlay(device, clip, NULL) != AUDIO_VOICE_NONE, "the adpcm clip must be playable");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");

    int32_t worstError = 0;
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        int32_t left = (int32_t)(frames[index * 2u] / CENTRE_GAIN * 32768.0f);
        int32_t right = (int32_t)(frames[index * 2u + 1u] / CENTRE_GAIN * 32768.0f);
        int32_t error = AbsoluteDifference(left, buffers->reference[index]);
        if (error > worstError) worstError = error;
        error = AbsoluteDifference(right, -(int32_t)buffers->reference[index]);
        if (error > worstError) worstError = error;
    }
    // На этом сигнале кодек даёт около 70 единиц из 32768. Порог взят с
    // запасом вчетверо: он ловит расхождение с декодером или потерянный
    // начальный шаг, но не срабатывает от разницы округления.
    Expect(worstError < 300, "the adpcm round trip must stay close to the original signal");
    Expect(worstError > 0, "a lossy codec that reproduces the input exactly is suspicious");

    AudioClipDestroy(clip);
    PlatformFree(frames);
    AudioDeviceDestroy(device);
#endif

    PlatformFree(buffers);
    LaiueTestRuntimeWrite("soundc test passed\n");
    LAIUE_TEST_SUCCESS();
}
