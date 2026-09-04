#pragma once

#include <stdbool.h>
#include <stdint.h>

// MPEG-1 Layer III (ISO/IEC 11172-3) в 16-битные сэмплы. Разбираются
// частоты 44100, 48000 и 32000 Гц, все битрейты, постоянные и
// переменные, стерео, joint stereo с MS и интенсивностным кодированием,
// двойной канал и моно.
//
// MPEG-2 и 2.5 (частоты ниже 32000 Гц), Layer I и II, свободный формат и
// арифметика отвергаются по имени: разобрать их наполовину хуже, чем
// сказать об этом прямо.
//
// Задержка снимается: у декодера она своя (529 отсчётов), у кодировщика
// записана в теге LAME. Без этого короткий звук начинался бы с тишины в
// четверть кадра, а зацикленный щёлкал бы на стыке.
//
// Своей памяти библиотека не выделяет: сначала Mp3Inspect сообщает
// размеры и объём рабочей памяти, потом Mp3DecodeSamples пишет в буферы
// вызывающей стороны. Рабочий буфер обязан быть выровнен под указатель.

// Предел на один звук совпадает с пределом контейнера `.la`.
#define MP3_MAX_FRAMES 0x04000000u

typedef enum Mp3Status
{
    MP3_OK = 0,
    MP3_INVALID_ARGUMENT,
    MP3_NOT_RECOGNISED,
    MP3_TRUNCATED,
    MP3_CORRUPT,
    MP3_UNSUPPORTED_FEATURE,
    MP3_TOO_LARGE,
    MP3_BUFFER_TOO_SMALL,
} Mp3Status;

typedef struct Mp3Info
{
    uint32_t frameCount;
    uint32_t channelCount;
    uint32_t sampleRate;
    uint32_t scratchBytes;

    // Заполняет Mp3Inspect, читает Mp3DecodeSamples. Отдельно от
    // frameCount, потому что декодер выдаёт больше, чем слышно:
    // начало и конец принадлежат задержке.
    uint32_t firstFrameOffset;
    uint32_t decodedFrames;
    uint32_t skipFrames;
} Mp3Info;

// Определяет формат по сигнатуре: тег ID3 или заголовок кадра.
bool Mp3Matches(const void *bytes, uint32_t sizeBytes);

Mp3Status Mp3Inspect(const void *bytes, uint32_t sizeBytes, Mp3Info *outInfo);

// Пишет frameCount * channelCount сэмплов, чередующихся по каналам.
Mp3Status Mp3DecodeSamples(const void *bytes, uint32_t sizeBytes, const Mp3Info *info,
                           int16_t *outSamples, uint32_t sampleCapacity, void *scratch,
                           uint32_t scratchBytes);

const char *Mp3StatusText(Mp3Status status);
