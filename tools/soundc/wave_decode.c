#include "wave_decode.h"

#include <stddef.h>

// Поддерживается ровно то, чем пишут звук редакторы: целочисленный PCM
// 8/16/24/32 и IEEE float 32/64, в том числе через WAVE_FORMAT_EXTENSIBLE.
// Сжатые контейнеры внутри WAV (тот же ADPCM от Microsoft) отвергаются с
// внятным кодом, а не разбираются наполовину.

#define WAVE_FORMAT_PCM 1u
#define WAVE_FORMAT_IEEE_FLOAT 3u
#define WAVE_FORMAT_EXTENSIBLE 0xFFFEu

#define WAVE_MIN_SAMPLE_RATE 1000u
#define WAVE_MAX_SAMPLE_RATE 384000u

static uint16_t ReadU16Le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8));
}

static uint32_t ReadU32Le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool TagEquals(const uint8_t *bytes, const char *tag)
{
    return bytes[0] == (uint8_t)tag[0] && bytes[1] == (uint8_t)tag[1] &&
           bytes[2] == (uint8_t)tag[2] && bytes[3] == (uint8_t)tag[3];
}

static int16_t ClampToI16(int32_t value)
{
    if (value > 32767) return (int16_t)32767;
    if (value < -32768) return (int16_t)(-32768);
    return (int16_t)value;
}

// Округление к ближайшему без обращения к libm: смещение прибавляется по
// знаку, поэтому усечение деления даёт тот же результат, что округление.
// Сдвиг вправо здесь не годится — для отрицательных он определён не
// стандартом, а конкретным компилятором.
static int16_t ScaleToI16(int64_t value, int64_t divisor)
{
    int64_t half = divisor / 2;
    int64_t rounded = value >= 0 ? (value + half) / divisor : (value - half) / divisor;
    return ClampToI16((int32_t)rounded);
}

static int16_t FloatToI16(double value)
{
    if (value > 1.0) value = 1.0;
    if (value < -1.0) value = -1.0;
    double scaled = value * 32767.0;
    scaled += scaled >= 0.0 ? 0.5 : -0.5;
    return (int16_t)scaled;
}

static float BitsToFloat(uint32_t bits)
{
    union
    {
        uint32_t bits;
        float value;
    } cast;
    cast.bits = bits;
    return cast.value;
}

static double BitsToDouble(uint64_t bits)
{
    union
    {
        uint64_t bits;
        double value;
    } cast;
    cast.bits = bits;
    return cast.value;
}

WaveStatus WaveInspect(const void *bytes, uint32_t sizeBytes, WaveInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return WAVE_INVALID_ARGUMENT;
    const uint8_t *file = (const uint8_t *)bytes;
    if (sizeBytes < 12u) return WAVE_NOT_RIFF;
    if (!TagEquals(file, "RIFF") || !TagEquals(file + 8, "WAVE")) return WAVE_NOT_RIFF;

    bool haveFormat = false;
    bool haveData = false;
    uint32_t formatTag = 0u;
    uint32_t channelCount = 0u;
    uint32_t sampleRate = 0u;
    uint32_t bitsPerSample = 0u;
    uint32_t dataOffset = 0u;
    uint32_t dataBytes = 0u;

    uint32_t cursor = 12u;
    while (cursor + 8u <= sizeBytes)
    {
        const uint8_t *chunk = file + cursor;
        uint32_t chunkBytes = ReadU32Le(chunk + 4);
        uint32_t payload = cursor + 8u;
        if (chunkBytes > sizeBytes - payload)
        {
            // Завышенный размер data пишут те, кто не дописал заголовок
            // после потоковой записи; берём то, что в файле есть. Любой
            // другой обрезанный чанк — повреждение.
            if (!TagEquals(chunk, "data")) return WAVE_TRUNCATED;
            chunkBytes = sizeBytes - payload;
        }

        if (TagEquals(chunk, "fmt "))
        {
            if (chunkBytes < 16u) return WAVE_TRUNCATED;
            const uint8_t *format = file + payload;
            formatTag = ReadU16Le(format);
            channelCount = ReadU16Le(format + 2);
            sampleRate = ReadU32Le(format + 4);
            bitsPerSample = ReadU16Le(format + 14);
            if (formatTag == WAVE_FORMAT_EXTENSIBLE)
            {
                // Настоящий формат лежит в первых двух байтах SubFormat GUID.
                if (chunkBytes < 40u) return WAVE_TRUNCATED;
                formatTag = ReadU16Le(format + 24);
            }
            haveFormat = true;
        }
        else if (TagEquals(chunk, "data"))
        {
            dataOffset = payload;
            dataBytes = chunkBytes;
            haveData = true;
        }

        // Чанки выровнены по чётной границе, а объявленный размер — нет.
        cursor = payload + chunkBytes + (chunkBytes & 1u);
    }

    if (!haveFormat) return WAVE_MISSING_FORMAT;
    if (formatTag != WAVE_FORMAT_PCM && formatTag != WAVE_FORMAT_IEEE_FLOAT)
        return WAVE_UNSUPPORTED_FORMAT;
    if (channelCount != 1u && channelCount != 2u) return WAVE_UNSUPPORTED_CHANNELS;
    if (sampleRate < WAVE_MIN_SAMPLE_RATE || sampleRate > WAVE_MAX_SAMPLE_RATE)
        return WAVE_UNSUPPORTED_RATE;

    bool isFloat = formatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (isFloat)
    {
        if (bitsPerSample != 32u && bitsPerSample != 64u) return WAVE_UNSUPPORTED_DEPTH;
    }
    else if (bitsPerSample != 8u && bitsPerSample != 16u && bitsPerSample != 24u &&
             bitsPerSample != 32u)
    {
        return WAVE_UNSUPPORTED_DEPTH;
    }

    if (!haveData) return WAVE_MISSING_DATA;
    uint32_t frameBytes = (bitsPerSample / 8u) * channelCount;
    uint32_t frameCount = dataBytes / frameBytes;
    if (frameCount == 0u) return WAVE_MISSING_DATA;
    if (frameCount > WAVE_MAX_FRAMES) return WAVE_TOO_LARGE;

    outInfo->frameCount = frameCount;
    outInfo->channelCount = channelCount;
    outInfo->sampleRate = sampleRate;
    outInfo->bitsPerSample = bitsPerSample;
    outInfo->dataOffset = dataOffset;
    // Хвост, не заполняющий кадр целиком, отбрасывается здесь, а не
    // тянется дальше как половина сэмпла.
    outInfo->dataBytes = frameCount * frameBytes;
    outInfo->isFloat = isFloat;
    return WAVE_OK;
}

WaveStatus WaveDecodeSamples(const void *bytes, uint32_t sizeBytes, const WaveInfo *info,
                             int16_t *outSamples, uint32_t sampleCapacity)
{
    if (bytes == NULL || info == NULL || outSamples == NULL) return WAVE_INVALID_ARGUMENT;
    if (info->frameCount == 0u || info->frameCount > WAVE_MAX_FRAMES) return WAVE_INVALID_ARGUMENT;
    if (info->channelCount != 1u && info->channelCount != 2u) return WAVE_INVALID_ARGUMENT;

    uint32_t sampleCount = info->frameCount * info->channelCount;
    if (sampleCapacity < sampleCount) return WAVE_INVALID_ARGUMENT;

    uint32_t sampleBytes = info->bitsPerSample / 8u;
    if (sampleBytes == 0u) return WAVE_INVALID_ARGUMENT;
    // Описание пришло от вызывающей стороны, поэтому границы проверяются
    // ещё раз: доверять полю смещения, не сверив его с файлом, нельзя.
    if (info->dataOffset > sizeBytes) return WAVE_TRUNCATED;
    if ((uint64_t)sampleCount * sampleBytes > (uint64_t)(sizeBytes - info->dataOffset))
        return WAVE_TRUNCATED;

    const uint8_t *data = (const uint8_t *)bytes + info->dataOffset;
    for (uint32_t index = 0; index < sampleCount; ++index)
    {
        const uint8_t *sample = data + (size_t)index * sampleBytes;
        if (info->isFloat)
        {
            outSamples[index] =
                info->bitsPerSample == 32u
                    ? FloatToI16((double)BitsToFloat(ReadU32Le(sample)))
                    : FloatToI16(BitsToDouble((uint64_t)ReadU32Le(sample) |
                                              ((uint64_t)ReadU32Le(sample + 4) << 32)));
            continue;
        }

        switch (info->bitsPerSample)
        {
        case 8u:
            // Восьмибитный PCM в WAV беззнаковый, с нулём в 128.
            outSamples[index] = (int16_t)(((int32_t)sample[0] - 128) * 256);
            break;
        case 16u:
            outSamples[index] = (int16_t)ReadU16Le(sample);
            break;
        case 24u:
        {
            int32_t raw = (int32_t)((uint32_t)sample[0] | ((uint32_t)sample[1] << 8) |
                                    ((uint32_t)sample[2] << 16));
            if ((raw & 0x00800000) != 0) raw -= 0x01000000;
            outSamples[index] = ScaleToI16(raw, 256);
            break;
        }
        default:
            outSamples[index] = ScaleToI16((int32_t)ReadU32Le(sample), 65536);
            break;
        }
    }
    return WAVE_OK;
}

const char *WaveStatusText(WaveStatus status)
{
    switch (status)
    {
    case WAVE_OK: return "ok";
    case WAVE_NOT_RIFF: return "not a RIFF/WAVE file";
    case WAVE_TRUNCATED: return "the file ends inside a chunk";
    case WAVE_MISSING_FORMAT: return "the file has no fmt chunk";
    case WAVE_MISSING_DATA: return "the file has no audio frames";
    case WAVE_UNSUPPORTED_FORMAT: return "only PCM and IEEE float WAV are supported";
    case WAVE_UNSUPPORTED_DEPTH:
        return "supported depths are 8, 16, 24, 32 bit PCM and 32, 64 bit float";
    case WAVE_UNSUPPORTED_CHANNELS: return "only mono and stereo are supported";
    case WAVE_UNSUPPORTED_RATE: return "the sample rate must be between 1000 and 384000 Hz";
    case WAVE_TOO_LARGE: return "the file holds more frames than the container allows";
    case WAVE_INVALID_ARGUMENT: return "invalid argument";
    }
    return "unknown error";
}
