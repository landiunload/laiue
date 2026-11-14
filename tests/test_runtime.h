#pragma once

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <stdio.h>
#include <stdlib.h>
#endif

// Общий минимальный harness: Windows-тесты сохраняют no-CRT entry point,
// а POSIX-тесты используют обычный main и системный C runtime. Вывод уходит
// сразу, без буфера: при сбое теста в журнале остаётся всё, что он успел
// написать.
static inline void LaiueTestRuntimeWrite(const char* text)
{
#if defined(_WIN32)
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == NULL || output == INVALID_HANDLE_VALUE)
    {
        return;
    }

    uint32_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }

    DWORD written = 0;
    WriteFile(output, text, length, &written, NULL);
#else
    fputs(text, stdout);
    fflush(stdout);
#endif
}

static inline void LaiueTestRuntimeExit(int code)
{
#if defined(_WIN32)
    ExitProcess((UINT)code);
#else
    exit(code);
#endif
}

// Отчёт о сбое (test_runtime.c): необработанное исключение Windows или
// сигнал SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP печатает код,
// адрес и цепочку вызовов, после чего процесс завершается так же, как без
// отчёта. LAIUE_TEST_ENTRY ставит его первым делом.
void LaiueTestRuntimeReportCrashes(void);

// Версия движка для тестовых манифестов берётся из project(VERSION):
// "engine = " LAIUE_TEST_ENGINE_VERSION.
#define LAIUE_TEST_STRINGIFY_(value) #value
#define LAIUE_TEST_STRINGIFY(value) LAIUE_TEST_STRINGIFY_(value)
#define LAIUE_TEST_ENGINE_VERSION                                                                  \
    LAIUE_TEST_STRINGIFY(LAIUE_VERSION_MAJOR) "." LAIUE_TEST_STRINGIFY(LAIUE_VERSION_MINOR)

#if defined(_WIN32)
#define LAIUE_TEST_ENTRY(name)                                                                     \
    static void name##Body(void);                                                                  \
    void name(void)                                                                                \
    {                                                                                              \
        LaiueTestRuntimeReportCrashes();                                                           \
        name##Body();                                                                              \
    }                                                                                              \
    static void name##Body(void)
#define LAIUE_TEST_SUCCESS() LaiueTestRuntimeExit(0)
#else
#define LAIUE_TEST_ENTRY(name)                                                                     \
    static int name##Body(void);                                                                   \
    int main(void)                                                                                 \
    {                                                                                              \
        LaiueTestRuntimeReportCrashes();                                                           \
        return name##Body();                                                                       \
    }                                                                                              \
    static int name##Body(void)
#define LAIUE_TEST_SUCCESS() return 0
#endif
