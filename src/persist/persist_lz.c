#include "persist/persist_lz.h"

#include <stddef.h>
#include <string.h>

#define LZ_MINIMUM_MATCH 4u
#define LZ_MAXIMUM_OFFSET 65535u
#define LZ_HASH_BITS 16u

static uint32_t Read32(const uint8_t *bytes)
{
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t Hash(uint32_t value)
{
    /* Мультипликативное хеширование: старшие биты произведения. */
    return (value * UINT32_C(2654435761)) >> (32u - LZ_HASH_BITS);
}

/* Длина в 4-битном поле и продолжение байтами 255. */
static bool WriteLength(uint8_t **cursor, const uint8_t *end, uint32_t length)
{
    if (length < 15u)
        return true;
    length -= 15u;
    while (length >= 255u)
    {
        if (*cursor >= end)
            return false;
        *(*cursor)++ = 255u;
        length -= 255u;
    }
    if (*cursor >= end)
        return false;
    *(*cursor)++ = (uint8_t)length;
    return true;
}

static bool EmitSequence(uint8_t **cursor, const uint8_t *end, const uint8_t *literals,
                         uint32_t literalCount, uint32_t offset, uint32_t matchLength)
{
    if (*cursor >= end)
        return false;
    const uint32_t matchCode = matchLength == 0u ? 0u : matchLength - LZ_MINIMUM_MATCH;
    uint8_t *token = (*cursor)++;
    *token = (uint8_t)(((literalCount < 15u ? literalCount : 15u) << 4u) |
                       (matchCode < 15u ? matchCode : 15u));
    if (!WriteLength(cursor, end, literalCount) || (uint64_t)(end - *cursor) < literalCount)
        return false;
    if (literalCount != 0u)
        memcpy(*cursor, literals, literalCount);
    *cursor += literalCount;
    if (matchLength == 0u)
        return true;
    if (end - *cursor < 2)
        return false;
    *(*cursor)++ = (uint8_t)offset;
    *(*cursor)++ = (uint8_t)(offset >> 8u);
    return WriteLength(cursor, end, matchCode);
}

uint32_t LaiuePersistLzCompress(const uint8_t *input, uint32_t inputSize, uint8_t *output,
                                uint32_t outputCapacity, uint32_t *scratch)
{
    if (input == NULL || output == NULL || scratch == NULL || inputSize == 0u ||
        inputSize > LAIUE_PERSIST_LZ_BLOCK_SIZE)
        return 0u;
    memset(scratch, 0, LAIUE_PERSIST_LZ_SCRATCH_BYTES);
    uint8_t *cursor = output;
    const uint8_t *const end = output + outputCapacity;
    uint32_t anchor = 0u;
    uint32_t position = 0u;
    while (inputSize >= LZ_MINIMUM_MATCH && position <= inputSize - LZ_MINIMUM_MATCH)
    {
        const uint32_t value = Read32(input + position);
        const uint32_t slot = Hash(value);
        const uint32_t stored = scratch[slot];
        scratch[slot] = position + 1u;
        if (stored != 0u)
        {
            uint32_t candidate = stored - 1u;
            if (position - candidate <= LZ_MAXIMUM_OFFSET && Read32(input + candidate) == value)
            {
                uint32_t start = position;
                uint32_t length = LZ_MINIMUM_MATCH;
                while (start + length < inputSize &&
                       input[start + length] == input[candidate + length])
                    ++length;
                while (start > anchor && candidate > 0u &&
                       input[start - 1u] == input[candidate - 1u])
                {
                    --start;
                    --candidate;
                    ++length;
                }
                if (!EmitSequence(&cursor, end, input + anchor, start - anchor, start - candidate,
                                  length))
                    return 0u;
                position = start + length;
                anchor = position;
                /* Соседняя позиция внутри совпадения помогает следующему поиску. */
                if (position >= 2u && position - 2u <= inputSize - LZ_MINIMUM_MATCH)
                    scratch[Hash(Read32(input + position - 2u))] = position - 1u;
                continue;
            }
        }
        /* Несжимаемые участки проходятся всё более крупным шагом. */
        position += 1u + ((position - anchor) >> 6u);
    }
    if (!EmitSequence(&cursor, end, input + anchor, inputSize - anchor, 0u, 0u))
        return 0u;
    const uint32_t written = (uint32_t)(cursor - output);
    return written < inputSize ? written : 0u;
}

static bool ReadLength(const uint8_t **cursor, const uint8_t *end, uint32_t *length, uint32_t limit)
{
    if (*length < 15u)
        return true;
    if (*length > limit)
        return false;
    for (;;)
    {
        if (*cursor >= end)
            return false;
        const uint8_t byte = *(*cursor)++;
        if (byte > limit - *length)
            return false;
        *length += byte;
        if (byte != 255u)
            return true;
    }
}

bool LaiuePersistLzDecompress(const uint8_t *input, uint32_t inputSize, uint8_t *output,
                              uint32_t outputSize)
{
    if ((inputSize != 0u && input == NULL) || (outputSize != 0u && output == NULL))
        return false;
    const uint8_t *cursor = input;
    const uint8_t *const inputEnd = input + inputSize;
    uint32_t produced = 0u;
    for (;;)
    {
        if (cursor >= inputEnd)
            return false;
        const uint8_t token = *cursor++;
        uint32_t literalCount = token >> 4u;
        if (!ReadLength(&cursor, inputEnd, &literalCount, outputSize) ||
            literalCount > outputSize - produced || (uint64_t)(inputEnd - cursor) < literalCount)
            return false;
        if (literalCount != 0u)
            memcpy(output + produced, cursor, literalCount);
        cursor += literalCount;
        produced += literalCount;
        if (cursor == inputEnd)
            return produced == outputSize && (token & 0x0fu) == 0u;
        if (inputEnd - cursor < 2)
            return false;
        const uint32_t offset = (uint32_t)cursor[0] | ((uint32_t)cursor[1] << 8u);
        cursor += 2;
        if (offset == 0u || offset > produced)
            return false;
        uint32_t matchLength = token & 0x0fu;
        if (!ReadLength(&cursor, inputEnd, &matchLength, outputSize))
            return false;
        matchLength += LZ_MINIMUM_MATCH;
        if (matchLength > outputSize - produced)
            return false;
        uint8_t *destination = output + produced;
        const uint8_t *source = destination - offset;
        if (offset >= matchLength)
            memcpy(destination, source, matchLength);
        else
            for (uint32_t index = 0u; index < matchLength; ++index)
                destination[index] = source[index];
        produced += matchLength;
    }
}
