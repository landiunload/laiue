#pragma once

#include <stdbool.h>
#include <stdint.h>

// Общий контракт декодеров звука. Все они отдают 16-битные сэмплы,
// чередующиеся по каналам.
//
// Формат определяется по сигнатуре, дальше работает нужный декодер: так
// сведения о наборе форматов лежат в одном месте, а не повторяются у
// каждого, кто открывает звук. Отдельные WaveInspect и Mp3Inspect
// остаются для того, кто формат уже знает.
//
// Своей памяти библиотека не выделяет: сначала SoundInspect сообщает
// размеры и объём рабочей памяти, потом SoundDecodeSamples пишет в
// буферы вызывающей стороны. Рабочий буфер обязан быть выровнен под
// указатель.

typedef enum SoundStatus
{
    SOUND_OK = 0,
    SOUND_INVALID_ARGUMENT,
    SOUND_NOT_RECOGNISED,
    SOUND_TRUNCATED,
    SOUND_CORRUPT,
    SOUND_UNSUPPORTED_FEATURE,
    SOUND_TOO_LARGE,
    SOUND_BUFFER_TOO_SMALL,
} SoundStatus;

typedef enum SoundFormat
{
    SOUND_FORMAT_UNKNOWN = 0,
    SOUND_FORMAT_WAVE,
    SOUND_FORMAT_MP3,
} SoundFormat;

typedef struct SoundInfo
{
    uint32_t frameCount;
    uint32_t channelCount;
    uint32_t sampleRate;
    // frameCount * channelCount: столько сэмплов запишет декодер.
    uint32_t sampleCount;
    // Рабочая память декодера. Ноль означает, что она не нужна.
    uint32_t scratchBytes;
} SoundInfo;

SoundFormat SoundProbe(const void *bytes, uint32_t sizeBytes);

SoundStatus SoundInspect(const void *bytes, uint32_t sizeBytes, SoundInfo *outInfo);

SoundStatus SoundDecodeSamples(const void *bytes, uint32_t sizeBytes, const SoundInfo *info,
                               int16_t *outSamples, uint32_t sampleCapacity, void *scratch,
                               uint32_t scratchBytes);

const char *SoundStatusText(SoundStatus status);
const char *SoundFormatName(SoundFormat format);
