#include "platform/efficiency_windows.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Класс приоритета до входа в режим; 0 — процесс не в режиме.
static volatile LONG g_priorityBeforeEfficiency;

bool PlatformEfficiencyModeSet(bool enabled)
{
    HANDLE process = GetCurrentProcess();
    if (enabled)
    {
        // Процесс, который уже работает с низким приоритетом, таким и остаётся.
        const DWORD current = GetPriorityClass(process);
        if (current != 0u && current != IDLE_PRIORITY_CLASS &&
            InterlockedCompareExchange(&g_priorityBeforeEfficiency, (LONG)current, 0) == 0)
            SetPriorityClass(process, IDLE_PRIORITY_CLASS);
    }
    else
    {
        const DWORD previous = (DWORD)InterlockedExchange(&g_priorityBeforeEfficiency, 0);
        if (previous != 0u)
            SetPriorityClass(process, previous);
    }
    // Без режима маска управления пуста: QoS снова выбирает система.
    PROCESS_POWER_THROTTLING_STATE state = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = enabled ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0u,
        .StateMask = enabled ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0u,
    };
    return SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
}

bool PlatformEfficiencyExemptCurrentThread(void)
{
    THREAD_POWER_THROTTLING_STATE state = {
        .Version = THREAD_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED,
        .StateMask = 0u,
    };
    const BOOL applied =
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
    return applied != FALSE;
}
