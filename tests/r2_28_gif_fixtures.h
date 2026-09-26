#pragma once

// Генератор входов для стенда и регрессионного теста GIF (агент 28-gif).
//
// Собирает GIF-потоки прямо в памяти: с несжатым LZW (код на пиксель) и с
// настоящим сжатым LZW, с чересстрочностью, прозрачностью, разными disposal
// и частыми clear-кодами. Кодировщик — минимальный, но корректный: темп
// роста ширины кода и добавления в словарь повторяет декодер из
// src/media/gif_decode.c, поэтому выход декодера совпадает с ожидаемой
// картинкой (проверяется отдельно).
//
// Пиксели задаются формулой от (x, y, кадр), а не данными на диске: фикстура
// не зависит от внешнего кодировщика, а размер кадра выбирает вызывающий.

#include "platform/system.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum R2GifPattern
{
    R2_GIF_NOISE = 0,     // псевдослучайный на каждый пиксель
    R2_GIF_BLOCKS = 1,    // постоянные блоки 8x8: длинные цепочки LZW
    R2_GIF_GRADIENT = 2,  // плавный градиент
} R2GifPattern;

typedef struct R2GifParams
{
    uint32_t width;
    uint32_t height;
    uint32_t frames;
    bool interlaced;
    bool compressed;       // true — сжатый LZW; false — литеральный (код на пиксель)
    uint32_t clearPeriod;  // литеральный — clear каждые N пикселей; 0 — только полный словарь
    bool transparent;      // движущийся прямоугольник + прозрачный индекс + disposal 1/2/3
    uint32_t rectWidth;
    uint32_t rectHeight;
    R2GifPattern pattern;
    uint32_t seed;
} R2GifParams;

// Порядок строк чересстрочного GIF — тот же, что у декодера.
static inline uint32_t R2InterlacedRow(uint32_t row, uint32_t height, bool interlaced)
{
    if (!interlaced) return row;
    uint32_t pass1 = (height + 7u) / 8u;
    uint32_t pass2 = (height + 3u) / 8u;
    uint32_t pass3 = (height + 1u) / 4u;

    if (row < pass1) return row * 8u;
    row -= pass1;
    if (row < pass2) return row * 8u + 4u;
    row -= pass2;
    if (row < pass3) return row * 4u + 2u;
    row -= pass3;
    return row * 2u + 1u;
}

// Цвета глобальной палитры (256 записей) — те же, что пишет R2GifBuild.
static inline void R2GifPalette(uint32_t index, uint8_t out[3])
{
    out[0] = (uint8_t)index;
    out[1] = (uint8_t)(255u - index);
    out[2] = (uint8_t)((index * 3u) & 0xFFu);
}

// Индекс палитры в кадре frame в точке (x, y) холста.
static inline uint8_t R2GifValue(const R2GifParams *params, uint32_t frame, uint32_t x,
                                 uint32_t y)
{
    switch (params->pattern)
    {
        case R2_GIF_BLOCKS:
        {
            uint32_t blockX = x / 8u;
            uint32_t blockY = y / 8u;
            return (uint8_t)(((blockX * 7u) ^ (blockY * 13u) ^ (frame * 53u)) & 0xFFu);
        }
        case R2_GIF_GRADIENT:
            return (uint8_t)((x + y * 2u + frame * 17u) & 0xFFu);
        default:
        {
            uint32_t value = (x * 73856093u) ^ (y * 19349663u) ^ (frame * 83492791u) ^
                             (params->seed * 2654435761u);
            value ^= value >> 13;
            value *= 1274126177u;
            value ^= value >> 16;
            return (uint8_t)(value & 0xFFu);
        }
    }
}

typedef struct R2GifWriter
{
    uint8_t *out;
    uint32_t capacity;
    uint32_t size;
    bool overflow;
} R2GifWriter;

static inline void R2GifByte(R2GifWriter *writer, uint8_t value)
{
    if (writer->size < writer->capacity)
    {
        writer->out[writer->size++] = value;
    }
    else
    {
        writer->overflow = true;
    }
}

static inline void R2GifU16(R2GifWriter *writer, uint32_t value)
{
    R2GifByte(writer, (uint8_t)(value & 0xFFu));
    R2GifByte(writer, (uint8_t)((value >> 8) & 0xFFu));
}

typedef struct R2GifLzw
{
    R2GifWriter *writer;
    uint8_t block[255];
    uint32_t blockCount;
    uint32_t accumulator;
    uint32_t bitCount;
} R2GifLzw;

static inline void R2GifLzwFlushBlock(R2GifLzw *lzw)
{
    if (lzw->blockCount == 0u) return;
    R2GifByte(lzw->writer, (uint8_t)lzw->blockCount);
    for (uint32_t index = 0u; index < lzw->blockCount; ++index)
    {
        R2GifByte(lzw->writer, lzw->block[index]);
    }
    lzw->blockCount = 0u;
}

static inline void R2GifLzwRawByte(R2GifLzw *lzw, uint8_t value)
{
    lzw->block[lzw->blockCount++] = value;
    if (lzw->blockCount == 255u) R2GifLzwFlushBlock(lzw);
}

static inline void R2GifLzwCode(R2GifLzw *lzw, uint32_t code, uint32_t bits)
{
    lzw->accumulator |= (code & ((1u << bits) - 1u)) << lzw->bitCount;
    lzw->bitCount += bits;
    while (lzw->bitCount >= 8u)
    {
        R2GifLzwRawByte(lzw, (uint8_t)(lzw->accumulator & 0xFFu));
        lzw->accumulator >>= 8u;
        lzw->bitCount -= 8u;
    }
}

static inline void R2GifLzwFinish(R2GifLzw *lzw)
{
    if (lzw->bitCount != 0u)
    {
        R2GifLzwRawByte(lzw, (uint8_t)(lzw->accumulator & 0xFFu));
        lzw->accumulator = 0u;
        lzw->bitCount = 0u;
    }
    R2GifLzwFlushBlock(lzw);
    R2GifByte(lzw->writer, 0u);   // терминатор цепочки подблоков
}

#define R2_GIF_DICT_SLOTS 16384u

typedef struct R2GifSlot
{
    uint32_t key;   // 0xFFFFFFFF — пусто; иначе (префикс << 8) | символ
    uint32_t code;
} R2GifSlot;

static R2GifSlot s_r2GifSlots[R2_GIF_DICT_SLOTS];

static inline R2GifSlot *R2GifFindSlot(uint32_t key)
{
    uint32_t index = (key * 2654435761u) >> 18;   // старшие 14 бит
    for (;;)
    {
        R2GifSlot *slot = &s_r2GifSlots[index];
        if (slot->key == key || slot->key == 0xFFFFFFFFu) return slot;
        index = (index + 1u) & (R2_GIF_DICT_SLOTS - 1u);
    }
}

static inline void R2GifResetSlots(void)
{
    for (uint32_t index = 0u; index < R2_GIF_DICT_SLOTS; ++index)
    {
        s_r2GifSlots[index].key = 0xFFFFFFFFu;
    }
}

// Сжатый LZW. Кодировщик держит словарь на одну запись впереди декодера,
// поэтому ширина кода увеличивается, когда nextCode становится больше
// 1 << codeBits (а не равен): тогда следующий код декодер прочитает той же
// шириной, какой он был записан. Полный словарь сбрасывается clear-кодом.
static inline void R2GifEncodeCompressed(R2GifLzw *lzw, const uint8_t *pixels, uint32_t count,
                                         uint32_t minimumCodeBits, uint32_t forcedClear)
{
    uint32_t clearCode = 1u << minimumCodeBits;
    uint32_t endCode = clearCode + 1u;
    uint32_t nextCode = clearCode + 2u;
    uint32_t codeBits = minimumCodeBits + 1u;

    R2GifResetSlots();
    R2GifLzwCode(lzw, clearCode, codeBits);
    if (count == 0u)
    {
        R2GifLzwCode(lzw, endCode, codeBits);
        return;
    }

    uint32_t previous = pixels[0];
    uint32_t sinceClear = 0u;
    for (uint32_t index = 1u; index < count; ++index)
    {
        uint32_t symbol = pixels[index];
        uint32_t key = (previous << 8) | symbol;
        R2GifSlot *slot = R2GifFindSlot(key);
        if (slot->key == key)
        {
            previous = slot->code;
            continue;
        }

        R2GifLzwCode(lzw, previous, codeBits);
        slot->key = key;
        slot->code = nextCode;
        ++nextCode;
        previous = symbol;

        bool cleared = false;
        if (nextCode == 4096u)
        {
            R2GifLzwCode(lzw, clearCode, codeBits);
            cleared = true;
        }
        else if (nextCode > (1u << codeBits) && codeBits < 12u)
        {
            ++codeBits;
        }
        if (!cleared && forcedClear != 0u && ++sinceClear >= forcedClear)
        {
            R2GifLzwCode(lzw, clearCode, codeBits);
            cleared = true;
        }
        if (cleared)
        {
            R2GifResetSlots();
            nextCode = clearCode + 2u;
            codeBits = minimumCodeBits + 1u;
            sinceClear = 0u;
        }
    }

    R2GifLzwCode(lzw, previous, codeBits);
    R2GifLzwCode(lzw, endCode, codeBits);
}

// Литеральный LZW: каждый пиксель — отдельный код. Словарь растёт как у
// декодера, но записи никогда не переиспользуются, поэтому поток остаётся
// валидным и проверяет цену самого цикла чтения кода.
static inline void R2GifEncodeLiteral(R2GifLzw *lzw, const uint8_t *pixels, uint32_t count,
                                      uint32_t minimumCodeBits, uint32_t clearPeriod)
{
    uint32_t clearCode = 1u << minimumCodeBits;
    uint32_t endCode = clearCode + 1u;
    uint32_t nextCode = clearCode + 2u;
    uint32_t codeBits = minimumCodeBits + 1u;
    bool havePrevious = false;
    uint32_t sinceClear = 0u;

    R2GifLzwCode(lzw, clearCode, codeBits);
    for (uint32_t index = 0u; index < count; ++index)
    {
        R2GifLzwCode(lzw, pixels[index], codeBits);
        if (havePrevious)
        {
            ++nextCode;
            if (nextCode == (1u << codeBits) && codeBits < 12u) ++codeBits;
        }
        else
        {
            havePrevious = true;
        }
        if (clearPeriod != 0u && ++sinceClear >= clearPeriod)
        {
            R2GifLzwCode(lzw, clearCode, codeBits);
            nextCode = clearCode + 2u;
            codeBits = minimumCodeBits + 1u;
            sinceClear = 0u;
            havePrevious = false;
        }
    }
    R2GifLzwCode(lzw, endCode, codeBits);
}

// Возвращает число записанных байт или 0, если буфер мал.
static inline uint32_t R2GifBuild(uint8_t *out, uint32_t capacity, const R2GifParams *params)
{
    R2GifWriter writer = {.out = out, .capacity = capacity};

    static const char signature[6] = {'G', 'I', 'F', '8', '9', 'a'};
    for (uint32_t index = 0u; index < 6u; ++index) R2GifByte(&writer, (uint8_t)signature[index]);
    R2GifU16(&writer, params->width);
    R2GifU16(&writer, params->height);
    R2GifByte(&writer, 0xF7u);   // глобальная палитра 256, глубина цвета 8
    R2GifByte(&writer, 0u);
    R2GifByte(&writer, 0u);
    for (uint32_t index = 0u; index < 256u; ++index)
    {
        uint8_t color[3];
        R2GifPalette(index, color);
        R2GifByte(&writer, color[0]);
        R2GifByte(&writer, color[1]);
        R2GifByte(&writer, color[2]);
    }

    for (uint32_t frame = 0u; frame < params->frames; ++frame)
    {
        uint32_t rectWidth = params->transparent ? params->rectWidth : params->width;
        uint32_t rectHeight = params->transparent ? params->rectHeight : params->height;
        uint32_t left = 0u;
        uint32_t top = 0u;
        if (params->transparent)
        {
            uint32_t spanX = params->width - rectWidth;
            uint32_t spanY = params->height - rectHeight;
            left = spanX != 0u ? (frame * 17u) % spanX : 0u;
            top = spanY != 0u ? (frame * 29u) % spanY : 0u;
        }
        uint32_t disposal = params->transparent ? (1u + (frame % 3u)) : 1u;   // 1,2,3

        R2GifByte(&writer, 0x21u);   // Graphic Control Extension
        R2GifByte(&writer, 0xF9u);
        R2GifByte(&writer, 0x04u);
        R2GifByte(&writer, (uint8_t)((disposal << 2) | (params->transparent ? 1u : 0u)));
        R2GifU16(&writer, 5u);   // задержка 50 мс в сотых
        R2GifByte(&writer, 0u);  // прозрачный индекс 0
        R2GifByte(&writer, 0u);

        R2GifByte(&writer, 0x2Cu);   // Image Descriptor
        R2GifU16(&writer, left);
        R2GifU16(&writer, top);
        R2GifU16(&writer, rectWidth);
        R2GifU16(&writer, rectHeight);
        R2GifByte(&writer, params->interlaced ? 0x40u : 0u);

        const uint32_t minimumCodeBits = 8u;
        R2GifByte(&writer, (uint8_t)minimumCodeBits);

        uint32_t total = rectWidth * rectHeight;
        uint8_t *scan = (uint8_t *)PlatformAllocate(total, false);
        if (scan == NULL)
        {
            writer.overflow = true;
            break;
        }
        uint32_t written = 0u;
        for (uint32_t scanRow = 0u; scanRow < rectHeight; ++scanRow)
        {
            uint32_t displayRow = R2InterlacedRow(scanRow, rectHeight, params->interlaced);
            for (uint32_t column = 0u; column < rectWidth; ++column)
            {
                uint8_t value = 0u;
                bool transparentPixel =
                    params->transparent && (((column + displayRow + frame) & 7u) == 0u);
                if (!transparentPixel)
                {
                    value = R2GifValue(params, frame, left + column, top + displayRow);
                }
                scan[written++] = value;
            }
        }

        R2GifLzw lzw = {.writer = &writer};
        if (params->compressed)
        {
            R2GifEncodeCompressed(&lzw, scan, total, minimumCodeBits, params->clearPeriod);
        }
        else
        {
            R2GifEncodeLiteral(&lzw, scan, total, minimumCodeBits, params->clearPeriod);
        }
        R2GifLzwFinish(&lzw);
        PlatformFree(scan);
    }

    R2GifByte(&writer, 0x3Bu);   // trailer
    return writer.overflow ? 0u : writer.size;
}
