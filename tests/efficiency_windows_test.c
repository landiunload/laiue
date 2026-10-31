#include "platform/efficiency_windows.h"
#include "test_runtime.h"

#include <stdbool.h>

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Efficiency mode check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

/* Windows старше 11 может не отдавать состояние EcoQoS: тогда проверяется
 * только приоритет. */
static bool QueryProcessThrottling(PROCESS_POWER_THROTTLING_STATE *state)
{
    *state = (PROCESS_POWER_THROTTLING_STATE){.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION};
    return GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, state,
                                 sizeof(*state)) != FALSE;
}

LAIUE_TEST_ENTRY(EfficiencyWindowsTestEntryPoint)
{
    HANDLE process = GetCurrentProcess();
    const DWORD speed = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    PROCESS_POWER_THROTTLING_STATE state;
    /* Программа сама выбрала класс приоритета: режим возвращает именно его. */
    Expect(SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS) != FALSE, "custom priority");

    /* Первый круг уходит из эффективности в производительность, второй —
     * сразу к выбору системы. */
    for (int round = 0; round < 2; ++round)
    {
        Expect(PlatformPowerModeSet(PLATFORM_POWER_EFFICIENCY), "EcoQoS is accepted");
        Expect(PlatformPowerModeSet(PLATFORM_POWER_EFFICIENCY), "entering twice");
        Expect(GetPriorityClass(process) == IDLE_PRIORITY_CLASS, "low base priority in the mode");
        if (QueryProcessThrottling(&state))
            Expect((state.ControlMask & speed) != 0u && (state.StateMask & speed) != 0u,
                   "EcoQoS in the mode");

        if (round == 0)
        {
            Expect(PlatformPowerModeSet(PLATFORM_POWER_PERFORMANCE), "HighQoS is accepted");
            Expect(GetPriorityClass(process) == ABOVE_NORMAL_PRIORITY_CLASS,
                   "performance returns the previous priority class");
            if (QueryProcessThrottling(&state))
                Expect((state.ControlMask & speed) != 0u && (state.StateMask & speed) == 0u,
                       "HighQoS in the performance mode");
        }

        Expect(PlatformPowerModeSet(PLATFORM_POWER_DEFAULT), "leaving the mode");
        Expect(PlatformPowerModeSet(PLATFORM_POWER_DEFAULT), "leaving twice");
        Expect(GetPriorityClass(process) == ABOVE_NORMAL_PRIORITY_CLASS,
               "the previous priority class returns");
        if (QueryProcessThrottling(&state))
            Expect((state.ControlMask & speed) == 0u, "the system chooses QoS again");
    }

    /* Процесс с низким приоритетом остаётся низким и после выхода. */
    Expect(SetPriorityClass(process, IDLE_PRIORITY_CLASS) != FALSE, "idle priority");
    Expect(PlatformPowerModeSet(PLATFORM_POWER_EFFICIENCY) &&
               PlatformPowerModeSet(PLATFORM_POWER_DEFAULT),
           "idle round");
    Expect(GetPriorityClass(process) == IDLE_PRIORITY_CLASS, "an idle process stays idle");

    Expect(PlatformEfficiencyExemptCurrentThread(), "the thread asks for high QoS");
    THREAD_POWER_THROTTLING_STATE thread = {.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION};
    if (GetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &thread, sizeof(thread)))
        Expect(thread.ControlMask == THREAD_POWER_THROTTLING_EXECUTION_SPEED &&
                   thread.StateMask == 0u,
               "the exempt thread keeps high QoS");

    Expect(SetPriorityClass(process, NORMAL_PRIORITY_CLASS) != FALSE, "normal priority");
    LaiueTestRuntimeWrite("efficiency mode: passed\n");
    LAIUE_TEST_SUCCESS();
}
