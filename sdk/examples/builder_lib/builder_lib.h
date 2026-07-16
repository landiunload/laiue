/*
 * Builder Lib — публичный интерфейс библиотечного мода.
 *
 * Этот заголовок автор библиотеки распространяет вместе со своим
 * .lmp: потребители включают его и получают таблицу через
 * api->queryInterface(BUILDER_LIB_NAME, BUILDER_LIB_VERSION).
 *
 * Правила версий: version растёт при добавлении функций в хвост;
 * ломающие изменения — новое имя интерфейса (например, ".v2").
 */

#pragma once

#include <stdint.h>

#define BUILDER_LIB_NAME "laiue.builder"
#define BUILDER_LIB_VERSION 1u

typedef struct BuilderLibV1
{
    uint32_t version;   /* фактическая версия таблицы у библиотеки */

    /* Заполняет параллелепипед блоками (границы включительно). */
    void (*fillBox)(int64_t x0, int64_t y0, int64_t z0,
        int64_t x1, int64_t y1, int64_t z1, uint8_t block);

    /* Спираль вокруг вертикальной оси: radius в блоках, height вверх
       от baseZ. Красивый способ проверить, что библиотека работает. */
    void (*buildHelix)(int64_t centerX, int64_t centerY, int64_t baseZ,
        int32_t radius, int32_t height, uint8_t block);
} BuilderLibV1;
