#include <windows.h>

typedef void (*StartFunction)(void);

void __cdecl __security_init_cookie(void);

// Точка входа без CRT (задаётся линкеру через /ENTRY:EntryPoint).
// ExitProcess вызывается явно: без CRT возврат из точки входа
// завершил бы только главный поток, а не процесс.
void __stdcall EntryPoint(void)
{
    __security_init_cookie();
    HMODULE coreLibrary = LoadLibraryW(L"laiue_core.dll");
    if (coreLibrary != NULL)
    {
        StartFunction start = (StartFunction)GetProcAddress(coreLibrary, "Start");
        if (start != NULL)
        {
            start();
        }

        FreeLibrary(coreLibrary);
    }

    ExitProcess(0);
}
