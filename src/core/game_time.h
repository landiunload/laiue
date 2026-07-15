#pragma once

#include <stdint.h>

// Игровое время суток и его влияние на свет и небо (внутренний
// компонент ядра). Время измеряется в часах 0..24: восход в 6:00,
// зенит в 12:00, закат в 18:00; луна ходит противоположно солнцу.

typedef enum TimeSpeedPreset
{
    TIME_SPEED_PAUSED = 0,
    TIME_SPEED_NORMAL,    // сутки за dayLengthMinutes из конфигурации
    TIME_SPEED_FAST,      // сутки за 2 минуты
    TIME_SPEED_REALTIME,  // сутки за 24 часа
    TIME_SPEED_COUNT,
} TimeSpeedPreset;

typedef struct DayLighting
{
    float sunDirection[3];  // единичный, от активного светила к миру
    float sunColor[3];      // солнце днём, луна ночью
    float ambientColor[3];
    float skyColor[3];
} DayLighting;

// Продвигает время суток на deltaSeconds реального времени; результат
// нормирован в [0, 24).
float GameTimeAdvance(float hours, TimeSpeedPreset speed,
    float dayLengthMinutes, float deltaSeconds);

// Свет и небо для момента суток: рассветы, закаты, ночь с луной.
void GameTimeGetLighting(float hours, DayLighting* outLighting);
