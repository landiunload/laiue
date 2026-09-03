#pragma once

#include <stdbool.h>
#include <stdint.h>

// Разбор WAV (RIFF/WAVE) в 16-битные сэмплы. Живёт в офлайн-инструменте,
// а не в движке: рантайм читает только готовый `.la` и ничего не
// декодирует сверх него, ровно как текстурпак не декодирует PNG.
//
// Своей памяти библиотека не выделяет: разбор идёт в два шага — сначала
// WaveInspect сообщает размеры, потом WaveDecodeSamples пишет в буфер
// вызывающей стороны. Так у неё нет ни своего мнения об аллокаторе, ни
// скрытого владения.

// Предел на один звук совпадает с пределом контейнера `.la`: он
// гарантирует, что произведение кадров, каналов и размера сэмпла не
// переполняет 32 бита.
#define WAVE_MAX_FRAMES 0x04000000u

typedef enum WaveStatus
{
    WAVE_OK = 0,
    WAVE_NOT_RIFF,
    WAVE_TRUNCATED,
    WAVE_MISSING_FORMAT,
    WAVE_MISSING_DATA,
    WAVE_UNSUPPORTED_FORMAT,
    WAVE_UNSUPPORTED_DEPTH,
    WAVE_UNSUPPORTED_CHANNELS,
    WAVE_UNSUPPORTED_RATE,
    WAVE_TOO_LARGE,
    WAVE_INVALID_ARGUMENT,
} WaveStatus;

typedef struct WaveInfo
{
    uint32_t frameCount;
    uint32_t channelCount;
    uint32_t sampleRate;
    uint32_t bitsPerSample;
    // Полезные данные внутри файла. Заполняются WaveInspect и передаются
    // в WaveDecodeSamples без изменений.
    uint32_t dataOffset;
    uint32_t dataBytes;
    bool isFloat;
} WaveInfo;

// Читает заголовок и находит данные, ничего не декодируя.
WaveStatus WaveInspect(const void *bytes, uint32_t sizeBytes, WaveInfo *outInfo);

// Пишет frameCount * channelCount сэмплов, чередующихся по каналам.
// Требуемая ёмкость известна из WaveInfo и проверяется здесь же.
WaveStatus WaveDecodeSamples(const void *bytes, uint32_t sizeBytes, const WaveInfo *info,
                             int16_t *outSamples, uint32_t sampleCapacity);

const char *WaveStatusText(WaveStatus status);
