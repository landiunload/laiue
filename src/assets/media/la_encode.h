#pragma once

#include "media/sound.h"

#include <stdbool.h>
#include <stdint.h>

// Запись контейнера `.la`. Раскладка описана в docs/soundpacks.md;
// разбор живёт в загрузчике звукопака (src/audio/audio_pack.c).
//
// Кодировщик общий: им пользуется и офлайн-конвертер, и сам движок —
// прочитав чужой формат, он кладёт рядом готовый `.la` и дальше берёт
// уже его. Копии писателя в двух местах быть не должно.
//
// Своей памяти библиотека не выделяет: размер файла считается заранее,
// а кодирование идёт в буфер вызывающей стороны.

// Версия 2 добавила отпечаток исходника; её заголовок длиннее прежнего.
#define SOUND_LA_VERSION 2u
#define SOUND_LA_HEADER_BYTES 40u
#define SOUND_LA_HEADER_BYTES_V1 24u
#define SOUND_LA_MAX_FRAMES 0x04000000u

typedef enum SoundEncoding
{
    SOUND_ENCODING_PCM16 = 0,
    SOUND_ENCODING_ADPCM = 1,
} SoundEncoding;

typedef struct SoundClip
{
    // Сэмплы 16-битные знаковые, чередующиеся по каналам, как в WAV и в
    // AudioClipDescription.
    const int16_t *samples;
    uint32_t frameCount;
    uint32_t channelCount;
    uint32_t sampleRate;
    SoundEncoding encoding;
    // Отпечаток исходника, из которого файл собран: время изменения и
    // размер. Нулевой размер означает, что файл ни из чего не выведен —
    // его собрали конвертером или положили руками, и устареть он не
    // может по определению.
    uint64_t sourceModifiedTime;
    uint32_t sourceSizeBytes;
} SoundClip;

// Точный размер файла для заданных параметров.
SoundStatus SoundEncodedBytes(SoundEncoding encoding, uint32_t frameCount, uint32_t channelCount,
                              uint32_t *outBytes);

SoundStatus SoundEncode(const SoundClip *clip, void *outBytes, uint32_t capacityBytes,
                        uint32_t *outWritten);
