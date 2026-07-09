#include "platform/time.h"

#include <windows.h>

double PlatformTimeSeconds(void)
{
    // Частота таймера неизменна после загрузки системы —
    // повторная инициализация безопасна и идемпотентна.
    static LONGLONG frequency;
    if (frequency == 0)
    {
        LARGE_INTEGER frequencyValue;
        QueryPerformanceFrequency(&frequencyValue);
        frequency = frequencyValue.QuadPart;
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency;
}
