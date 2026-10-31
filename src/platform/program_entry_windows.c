// Начало программы на laiue под Windows. Движок собирается без CRT, и её
// стартовый код заменяет эта функция: обычная main программы получает
// аргументы в UTF-8, как на остальных системах, а её код становится кодом
// выхода процесса.
#include <windows.h>

#include <shellapi.h>

#include "platform/system.h"

#include <stddef.h>

int main(int argumentCount, char **arguments);

void LaiueProgramEntry(void)
{
    int count = 0;
    // Кавычки разбирает система: свой разбор расходился бы с ней ровно в
    // тех путях, которые пользователь и берёт в кавычки.
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == NULL || count < 0)
        ExitProcess(2u);
    size_t bytes = ((size_t)count + 1u) * sizeof(char *);
    for (int index = 0; index < count; ++index)
        bytes += (size_t)WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, NULL, 0, NULL, NULL);
    char **arguments = (char **)PlatformAllocate(bytes, false);
    if (arguments == NULL)
    {
        LocalFree((HLOCAL)wide);
        ExitProcess(2u);
    }
    char *text = (char *)(arguments + count + 1);
    const char *const end = (const char *)arguments + bytes;
    for (int index = 0; index < count; ++index)
    {
        const int written =
            WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, text, (int)(end - text), NULL, NULL);
        if (written <= 0)
        {
            PlatformFree((void *)arguments);
            LocalFree((HLOCAL)wide);
            ExitProcess(2u);
        }
        arguments[index] = text;
        text += written;
    }
    arguments[count] = NULL;
    LocalFree((HLOCAL)wide);
    const int code = main(count, arguments);
    PlatformFree((void *)arguments);
    ExitProcess((UINT)code);
}
