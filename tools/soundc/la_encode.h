#pragma once

#include <stdbool.h>
#include <stdint.h>

// Запись контейнера `.la`. Раскладка описана в docs/soundpacks.md;
// декодер живёт в движке (src/audio/audio_pack.c), кодировщик — здесь,
// потому что подготовка содержимого выполняется до запуска игры.
//
// Своей памяти библиотека не выделяет: размер файла считается заранее,
// а кодирование идёт в буфер вызывающей стороны.

#define LA_HEADER_BYTES 24u
#define LA_MAX_FRAMES 0x04000000u

typedef enum LaEncoding
{
    LA_ENCODING_PCM16 = 0,
    LA_ENCODING_ADPCM = 1,
} LaEncoding;

typedef enum LaStatus
{
    LA_OK = 0,
    LA_INVALID_ARGUMENT,
    LA_TOO_LARGE,
    LA_BUFFER_TOO_SMALL,
} LaStatus;

// Точный размер файла для заданных параметров.
LaStatus LaEncodedBytes(LaEncoding encoding, uint32_t frameCount, uint32_t channelCount,
                        uint32_t *outBytes);

// Сэмплы 16-битные знаковые, чередующиеся по каналам, как в WAV и в
// AudioClipDescription.
LaStatus LaEncode(const int16_t *samples, uint32_t frameCount, uint32_t channelCount,
                  uint32_t sampleRate, LaEncoding encoding, void *outBytes,
                  uint32_t capacityBytes, uint32_t *outWritten);

const char *LaStatusText(LaStatus status);
