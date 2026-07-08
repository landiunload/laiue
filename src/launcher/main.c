#include <windows.h>

void _RTC_InitBase(void) {}
void _RTC_Shutdown(void) {}
int _RTC_CheckStackVars(void* frame, void* descriptors) { (void)frame; (void)descriptors; return 0; }

void __stdcall mainCRTStartup(void)
{
    HMODULE dll = LoadLibraryW(L"laiue_core.dll");
    if (dll == NULL) return;

    void (__stdcall *start)(void) = (void (__stdcall *)(void))GetProcAddress(dll, "Start");
    if (start != NULL) start();
}
