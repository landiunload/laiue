#pragma once

#include "media/image.h"

#include <stdbool.h>
#include <stdint.h>

// DEFLATE (RFC 1951) в обёртке zlib (RFC 1950). Нужен PNG и больше
// никому: собственная реализация избавляет инструмент от zlib, а движок
// от неё и так не зависит.
//
// Вход задаётся списком отрезков, потому что в PNG поток сжатых данных
// разрезан на чанки IDAT: собирать их в один буфер значило бы копировать
// десятки мегабайт ради удобства читателя битов.

typedef struct InflateSegment
{
    const uint8_t *bytes;
    uint32_t size;
} InflateSegment;

// Таблицы прямого поиска по старшим битам кода. Девяти бит хватает
// почти всем кодам DEFLATE: более длинные разбираются каноническим
// циклом. Таблица лежит в рабочей памяти вызывающего, а не на стеке:
// её размер не помещается в бюджет кадра без CRT.
#define INFLATE_MAX_CODE_BITS 15u
#define INFLATE_FAST_BITS 9u
#define INFLATE_FAST_SIZE (1u << INFLATE_FAST_BITS)
#define INFLATE_LITERAL_SYMBOLS 288u

typedef struct InflateHuffmanTable
{
    // count[n] — сколько кодов длины n; symbol — символы в порядке
    // возрастания кода. Этого достаточно для канонического разбора.
    int16_t count[INFLATE_MAX_CODE_BITS + 1u];
    int16_t symbol[INFLATE_LITERAL_SYMBOLS];
    // fast[(length << 9) | symbol]; length == 0 означает, что код длиннее
    // INFLATE_FAST_BITS и его следует читать по биту.
    uint16_t fast[INFLATE_FAST_SIZE];
} InflateHuffmanTable;

typedef struct InflateWork
{
    InflateHuffmanTable literals;
    InflateHuffmanTable distances;
    InflateHuffmanTable codeLengths;
    // Фиксированные таблицы постоянны: строятся один раз на поток, а не
    // на каждый фиксированный блок, иначе их быстрый поиск обходился бы
    // дороже, чем экономил.
    InflateHuffmanTable fixedLiterals;
    InflateHuffmanTable fixedDistances;
    bool fixedReady;
} InflateWork;

// Пишет ровно в выходной буфер вызывающей стороны; расширять его
// декодер не умеет и не должен: размер распакованных данных PNG знает
// заранее из заголовка, а неожиданный избыток — признак повреждения.
//
// work/workBytes — участок под InflateWork: его размер PngInspect
// добавляет к scratchBytes.
//
// Контрольная сумма Adler-32 в конце потока проверяется: она ловит
// повреждение, которого не заметил бы разбор кодов Хаффмана.
ImageStatus InflateZlib(const InflateSegment *segments, uint32_t segmentCount, void *output,
                        uint32_t outputBytes, void *work, uint32_t workBytes,
                        uint32_t *outWritten);
