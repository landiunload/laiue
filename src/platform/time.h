#pragma once

#include "api.h"

// Монотонное время в секундах (QueryPerformanceCounter).
// Ядро не трогает WinAPI напрямую — платформенные вызовы живут здесь.
LAIUE_WINDOW_API double PlatformTimeSeconds(void);
