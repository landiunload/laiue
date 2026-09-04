#pragma once

#include <stdbool.h>
#include <stdint.h>

// Запись `.lt` — одной текстуры движка. Раскладка описана в
// docs/texturepacks.md; разбор живёт в загрузчике текстурпака
// (src/render/texture_build.c).
//
// Кодировщик общий: им пользуется и офлайн-инструмент, и сам движок —
// прочитав PNG, GIF или JPEG, он кладёт рядом готовый `.lt` и дальше
// берёт уже его. Копии писателя в двух местах быть не должно.
//
// Смысл `.lt` в том, что он уже разобран: загрузка пропускает
// декодирование и берёт готовые пиксели.
//
// Своей памяти библиотека не выделяет: размер файла считается заранее, а
// кодирование идёт в буфер вызывающей стороны.

// Версия 2 добавила расписание кадров и отпечаток исходника; её
// заголовок длиннее прежнего.
#define LT_VERSION 2u
#define LT_HEADER_BYTES 40u
#define LT_HEADER_BYTES_V1 24u
#define LT_MAX_FRAMES 256u
#define LT_MAX_SIZE 4096u

typedef enum LtStatus
{
    LT_OK = 0,
    LT_INVALID_ARGUMENT,
    LT_BAD_SIZE,
    LT_TOO_MANY_FRAMES,
    LT_BUFFER_TOO_SMALL,
} LtStatus;

typedef struct LtTexture
{
    // Кадры лежат подряд, каждый — RGBA8 width x height, строки
    // вплотную. normalFrames может быть NULL: тогда карт нормалей в
    // файле нет.
    const uint8_t *albedoFrames;
    const uint8_t *normalFrames;
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    // Длительность каждого кадра, frameCount значений. При одном кадре
    // допускается NULL. При нескольких каждое значение обязано быть
    // ненулевым: кадр без длительности остановил бы анимацию молча.
    const uint16_t *frameMilliseconds;
    // Отпечаток исходника, из которого файл собран: время изменения и
    // размер. Нулевой размер означает, что файл ни из чего не выведен —
    // его собрали инструментом или положили руками, и устареть он не
    // может по определению.
    uint64_t sourceModifiedTime;
    uint32_t sourceSizeBytes;
} LtTexture;

// Точный размер файла для заданных параметров.
LtStatus LtEncodedBytes(uint32_t width, uint32_t height, uint32_t frameCount, bool withNormals,
                        uint32_t *outBytes);

LtStatus LtEncode(const LtTexture *texture, void *outBytes, uint32_t capacityBytes,
                  uint32_t *outWritten);

const char *LtStatusText(LtStatus status);
