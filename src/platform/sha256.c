#include "platform/sha256.h"

#include <stddef.h>
#include <string.h>

typedef struct LaiueSha256Context
{
    uint32_t state[8];
    uint64_t totalBytes;
    uint8_t block[64];
    uint32_t blockBytes;
} LaiueSha256Context;

static uint32_t RotateRight(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static uint32_t ReadBigEndian32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void WriteBigEndian32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void Transform(LaiueSha256Context *context, const uint8_t block[64])
{
    static const uint32_t roundConstants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };
    uint32_t schedule[64];
    for (uint32_t index = 0; index < 16U; ++index)
    {
        schedule[index] = ReadBigEndian32(block + index * 4U);
    }
    for (uint32_t index = 16U; index < 64U; ++index)
    {
        uint32_t previous15 = schedule[index - 15U];
        uint32_t previous2 = schedule[index - 2U];
        uint32_t sigma0 =
            RotateRight(previous15, 7U) ^ RotateRight(previous15, 18U) ^ (previous15 >> 3U);
        uint32_t sigma1 =
            RotateRight(previous2, 17U) ^ RotateRight(previous2, 19U) ^ (previous2 >> 10U);
        schedule[index] = schedule[index - 16U] + sigma0 + schedule[index - 7U] + sigma1;
    }

    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];
    for (uint32_t index = 0; index < 64U; ++index)
    {
        uint32_t sum1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + sum1 + choose + roundConstants[index] + schedule[index];
        uint32_t sum0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void Initialize(LaiueSha256Context *context)
{
    static const uint32_t initialState[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    memcpy(context->state, initialState, sizeof(initialState));
    context->totalBytes = 0U;
    context->blockBytes = 0U;
}

static void Update(LaiueSha256Context *context, const uint8_t *bytes, size_t size)
{
    context->totalBytes += (uint64_t)size;
    while (size > 0U)
    {
        size_t available = sizeof(context->block) - context->blockBytes;
        size_t part = size < available ? size : available;
        memcpy(context->block + context->blockBytes, bytes, part);
        context->blockBytes += (uint32_t)part;
        bytes += part;
        size -= part;
        if (context->blockBytes == sizeof(context->block))
        {
            Transform(context, context->block);
            context->blockBytes = 0U;
        }
    }
}

static void Finish(LaiueSha256Context *context, uint8_t output[32])
{
    uint64_t totalBits = context->totalBytes << 3U;
    context->block[context->blockBytes++] = 0x80U;
    if (context->blockBytes > 56U)
    {
        memset(context->block + context->blockBytes, 0,
               sizeof(context->block) - context->blockBytes);
        Transform(context, context->block);
        context->blockBytes = 0U;
    }
    memset(context->block + context->blockBytes, 0, 56U - context->blockBytes);
    for (uint32_t index = 0; index < 8U; ++index)
    {
        context->block[63U - index] = (uint8_t)(totalBits >> (index * 8U));
    }
    Transform(context, context->block);
    for (uint32_t index = 0; index < 8U; ++index)
    {
        WriteBigEndian32(output + index * 4U, context->state[index]);
    }
}

bool LaiueSha256Compute(const void *bytes, uint64_t size, uint8_t output[32])
{
    if (output == NULL || (bytes == NULL && size != 0U))
    {
        return false;
    }
    LaiueSha256Context context;
    Initialize(&context);
    const uint8_t *input = bytes;
    uint64_t offset = 0U;
    while (offset < size)
    {
        size_t part = size - offset > SIZE_MAX ? SIZE_MAX : (size_t)(size - offset);
        Update(&context, input + offset, part);
        offset += part;
    }
    Finish(&context, output);
    return true;
}
