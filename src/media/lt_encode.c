#include "media/lt_encode.h"

#include <stddef.h>

#define LT_MAGIC 0x3153544Cu   // L, T, S, 1 little-endian
#define LT_FORMAT_RGBA8 1u
#define LT_FORMAT_RGBA8_NORMALS 2u
#define LT_MAX_FILE_BYTES 0x20000000u

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

static LtStatus PayloadBytes(uint32_t width, uint32_t height, uint32_t frameCount,
                             bool withNormals, uint32_t *outBytes)
{
    if (width == 0u || height == 0u || width > LT_MAX_SIZE || height > LT_MAX_SIZE)
        return LT_BAD_SIZE;
    if (frameCount == 0u || frameCount > LT_MAX_FRAMES) return LT_TOO_MANY_FRAMES;

    uint64_t frameBytes = (uint64_t)width * height * 4u;
    uint64_t total = frameBytes * frameCount * (withNormals ? 2u : 1u);
    if (total + LT_HEADER_BYTES > LT_MAX_FILE_BYTES) return LT_BAD_SIZE;
    *outBytes = (uint32_t)total;
    return LT_OK;
}

LtStatus LtEncodedBytes(uint32_t width, uint32_t height, uint32_t frameCount, bool withNormals,
                        uint32_t *outBytes)
{
    if (outBytes == NULL) return LT_INVALID_ARGUMENT;

    uint32_t payload = 0u;
    LtStatus status = PayloadBytes(width, height, frameCount, withNormals, &payload);
    if (status != LT_OK) return status;
    // Таблица длительностей идёт между заголовком и пикселями: по два
    // байта на кадр.
    *outBytes = LT_HEADER_BYTES + frameCount * 2u + payload;
    return LT_OK;
}

LtStatus LtEncode(const LtTexture *texture, void *outBytes, uint32_t capacityBytes,
                  uint32_t *outWritten)
{
    if (texture == NULL || texture->albedoFrames == NULL || outBytes == NULL)
        return LT_INVALID_ARGUMENT;

    uint32_t frameCount = texture->frameCount;
    const uint16_t *durations = texture->frameMilliseconds;
    // Кадр без длительности остановил бы анимацию молча, поэтому такой
    // файл лучше не писать вовсе.
    if (frameCount > 1u)
    {
        if (durations == NULL) return LT_INVALID_ARGUMENT;
        for (uint32_t index = 0; index < frameCount; ++index)
        {
            if (durations[index] == 0u) return LT_INVALID_ARGUMENT;
        }
    }

    uint32_t payloadBytes = 0u;
    LtStatus status = PayloadBytes(texture->width, texture->height, frameCount,
                                   texture->normalFrames != NULL, &payloadBytes);
    if (status != LT_OK) return status;

    uint32_t tableBytes = frameCount * 2u;
    uint32_t required = LT_HEADER_BYTES + tableBytes + payloadBytes;
    if (capacityBytes < required) return LT_BUFFER_TOO_SMALL;

    uint8_t *file = (uint8_t *)outBytes;
    WriteU32Le(file, LT_MAGIC);
    WriteU16Le(file + 4, LT_VERSION);
    WriteU16Le(file + 6, LT_HEADER_BYTES);
    WriteU16Le(file + 8, texture->width);
    WriteU16Le(file + 10, texture->height);
    WriteU16Le(file + 12, frameCount);
    // В заголовке остаётся длительность первого кадра: по ней видно
    // расписание, не читая таблицу.
    WriteU16Le(file + 14, frameCount > 1u && durations != NULL ? durations[0] : 0u);
    WriteU16Le(file + 16, texture->normalFrames != NULL ? LT_FORMAT_RGBA8_NORMALS : LT_FORMAT_RGBA8);
    WriteU16Le(file + 18, 0u);
    WriteU32Le(file + 20, payloadBytes);
    // Отпечаток исходника: по нему движок узнаёт, не устарел ли файл, не
    // читая сам исходник.
    WriteU32Le(file + 24, (uint32_t)(texture->sourceModifiedTime & 0xFFFFFFFFu));
    WriteU32Le(file + 28, (uint32_t)(texture->sourceModifiedTime >> 32));
    WriteU32Le(file + 32, texture->sourceSizeBytes);
    WriteU32Le(file + 36, 0u);

    uint8_t *table = file + LT_HEADER_BYTES;
    for (uint32_t index = 0; index < frameCount; ++index)
    {
        uint32_t value = frameCount > 1u && durations != NULL ? durations[index] : 0u;
        WriteU16Le(table + index * 2u, value);
    }

    uint32_t albedoBytes = texture->width * texture->height * 4u * frameCount;
    uint8_t *payload = file + LT_HEADER_BYTES + tableBytes;
    for (uint32_t index = 0; index < albedoBytes; ++index)
    {
        payload[index] = texture->albedoFrames[index];
    }
    if (texture->normalFrames != NULL)
    {
        for (uint32_t index = 0; index < albedoBytes; ++index)
        {
            payload[albedoBytes + index] = texture->normalFrames[index];
        }
    }

    if (outWritten != NULL) *outWritten = required;
    return LT_OK;
}

const char *LtStatusText(LtStatus status)
{
    switch (status)
    {
    case LT_OK: return "ok";
    case LT_INVALID_ARGUMENT: return "invalid argument";
    case LT_BAD_SIZE: return "the texture side must be between 1 and 4096";
    case LT_TOO_MANY_FRAMES: return "a texture holds between 1 and 256 frames";
    case LT_BUFFER_TOO_SMALL: return "the output buffer is too small";
    }
    return "unknown error";
}
