/* Намеренные сбои для проверки отчёта test_runtime.c. Сценарий
 * run_crash_report_test.cmake запускает harness с видом сбоя в
 * LAIUE_CRASH_REPORT_TEST и сверяет отчёт и то, что процесс по-прежнему
 * завершился сбоем. Без переменной harness ничего не делает. */

#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(_MSC_VER)
#define CRASH_TEST_NOINLINE __declspec(noinline)
#else
#define CRASH_TEST_NOINLINE __attribute__((noinline))
#endif

/* Через volatile компилятор не видит негодных адресов и не заменяет сбой
 * ловушкой. Результаты уходят в sink, чтобы вызовы не стали переходами и
 * каждая функция оставила свой кадр. */
static volatile uintptr_t badAddress = 0x10u;
static void (*volatile badFunction)(void);
static volatile uint8_t firstFrame;
static volatile uint32_t sink;

static CRASH_TEST_NOINLINE uint32_t ReadBadAddress(void)
{
    return *(volatile const uint8_t *)badAddress;
}

static CRASH_TEST_NOINLINE uint32_t ReadThroughCaller(void)
{
    return ReadBadAddress() + 1u;
}

static CRASH_TEST_NOINLINE uint32_t CallBadFunction(void)
{
    badFunction();
    return 1u;
}

/* Адрес кадра уходит в следующий вызов: кадры не сливаются в цикл, и стек
 * растёт до переполнения. Глубина ограничена примерно 70 МиБ: стек без
 * предела (ulimit -s unlimited) иначе съел бы память, а так проверка просто
 * не дождётся сбоя. */
static CRASH_TEST_NOINLINE uint32_t Overflow(volatile uint8_t *previous, uint32_t depth)
{
    volatile uint8_t frame[512];
    frame[0] = (uint8_t)depth;
    if (depth == 1u << 17)
        return previous[0];
    return Overflow(frame, depth + 1u) + previous[0];
}

static void ReadKind(char kind[16])
{
    kind[0] = '\0';
#if defined(_WIN32)
    if (GetEnvironmentVariableA("LAIUE_CRASH_REPORT_TEST", kind, 16u) >= 16u)
        kind[0] = '\0';
#else
    const char *value = getenv("LAIUE_CRASH_REPORT_TEST");
    for (uint32_t index = 0u; value != NULL && value[index] != '\0' && index + 1u < 16u; ++index)
    {
        kind[index] = value[index];
        kind[index + 1u] = '\0';
    }
#endif
}

static bool SameText(const char *first, const char *second)
{
    for (uint32_t index = 0u;; ++index)
    {
        if (first[index] != second[index])
            return false;
        if (first[index] == '\0')
            return true;
    }
}

LAIUE_TEST_ENTRY(CrashReportTestEntryPoint)
{
    char kind[16];
    ReadKind(kind);
    if (kind[0] != '\0')
    {
        if (SameText(kind, "read"))
            sink = ReadThroughCaller();
        else if (SameText(kind, "call"))
            sink = CallBadFunction();
        else if (SameText(kind, "overflow"))
            sink = Overflow(&firstFrame, 0u);
        LaiueTestRuntimeWrite("no crash\n");
        LaiueTestRuntimeExit(1);
    }
    LAIUE_TEST_SUCCESS();
}
