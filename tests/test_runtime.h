#pragma once

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <stdio.h>
#include <stdlib.h>
#endif

// Общий минимальный harness: Windows-тесты сохраняют no-CRT entry point,
// а POSIX-тесты используют обычный main и системный C runtime.
static void LaiueTestRuntimeWrite(const char* text)
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
#endif
}

static void LaiueTestRuntimeExit(int code)
{
#if defined(_WIN32)
    ExitProcess((UINT)code);
#else
    exit(code);
#endif
}

#if defined(_WIN32)
#define LAIUE_TEST_ENTRY(name) void name(void)
#define LAIUE_TEST_SUCCESS() LaiueTestRuntimeExit(0)
#else
#define LAIUE_TEST_ENTRY(name) int main(void)
#define LAIUE_TEST_SUCCESS() return 0
#endif
