#include "core/game_time.h"
#include "core/math.h"

#define PI 3.141592654f

float GameTimeAdvance(float hours, TimeSpeedPreset speed,
    float dayLengthMinutes, float deltaSeconds)
{
    float minutesPerDay;
    switch (speed)
    {
        case TIME_SPEED_PAUSED:   return hours;
        case TIME_SPEED_FAST:     minutesPerDay = 2.0f; break;
        case TIME_SPEED_REALTIME: minutesPerDay = 24.0f * 60.0f; break;
        default:                  minutesPerDay = dayLengthMinutes; break;
    }
    if (minutesPerDay < 0.05f)
    {
        minutesPerDay = 0.05f;
    }

    hours += deltaSeconds * (24.0f / (minutesPerDay * 60.0f));
    while (hours >= 24.0f) hours -= 24.0f;
    while (hours < 0.0f) hours += 24.0f;
    return hours;
}

static void Mix3(const float from[3], const float to[3], float t, float out[3])
{
    out[0] = from[0] + (to[0] - from[0]) * t;
    out[1] = from[1] + (to[1] - from[1]) * t;
    out[2] = from[2] + (to[2] - from[2]) * t;
}

static float SmoothStep01(float value)
{
    float t = ScalarClamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void GameTimeGetLighting(float hours, DayLighting* outLighting)
{
    // Угол солнца: 6:00 -> 0 (восход, +X), 12:00 -> pi/2 (зенит),
    // 18:00 -> pi (закат, -X); ночью солнце под горизонтом.
    float angle = (hours - 6.0f) * (PI / 12.0f);
    float sunX = ScalarCos(angle);
    float sunZ = ScalarSin(angle);   // высота (мир: Z вверх)
    float elevation = sunZ;

    // Плавные переходы день/ночь вокруг горизонта.
    float daylight = SmoothStep01((elevation + 0.10f) / 0.30f);
    float moonlight = SmoothStep01((-elevation - 0.10f) / 0.30f);
    // Тёплое свечение, когда солнце у горизонта (и утром, и вечером).
    float horizonGlow = SmoothStep01(1.0f - ScalarClamp(
        (elevation < 0.0f ? -elevation : elevation) / 0.35f, 0.0f, 1.0f));

    static const float daySun[3] = { 1.04f, 0.99f, 0.92f };
    static const float sunsetSun[3] = { 1.05f, 0.56f, 0.30f };
    static const float moonColor[3] = { 0.10f, 0.12f, 0.19f };
    static const float dayAmbient[3] = { 0.42f, 0.45f, 0.50f };
    static const float nightAmbient[3] = { 0.075f, 0.09f, 0.14f };
    static const float daySky[3] = { 0.40f, 0.62f, 0.92f };
    static const float nightSky[3] = { 0.012f, 0.02f, 0.05f };
    static const float sunsetSky[3] = { 0.88f, 0.48f, 0.30f };

    if (elevation >= 0.0f)
    {
        // День: направление от солнца, у горизонта — тёплый оттенок.
        outLighting->sunDirection[0] = -sunX;
        outLighting->sunDirection[1] = 0.0f;
        outLighting->sunDirection[2] = -sunZ;

        float warm[3];
        Mix3(daySun, sunsetSun, horizonGlow, warm);
        outLighting->sunColor[0] = warm[0] * daylight;
        outLighting->sunColor[1] = warm[1] * daylight;
        outLighting->sunColor[2] = warm[2] * daylight;
    }
    else
    {
        // Ночь: луна напротив солнца, холодный слабый свет.
        outLighting->sunDirection[0] = sunX;
        outLighting->sunDirection[1] = 0.0f;
        outLighting->sunDirection[2] = sunZ;
        outLighting->sunColor[0] = moonColor[0] * moonlight;
        outLighting->sunColor[1] = moonColor[1] * moonlight;
        outLighting->sunColor[2] = moonColor[2] * moonlight;
    }

    Mix3(nightAmbient, dayAmbient, daylight, outLighting->ambientColor);

    float sky[3];
    Mix3(nightSky, daySky, daylight, sky);
    // Заря подмешивается сильнее, пока светло хотя бы частично.
    float glowAmount = horizonGlow * (0.15f + 0.50f * daylight);
    Mix3(sky, sunsetSky, glowAmount, outLighting->skyColor);
}
