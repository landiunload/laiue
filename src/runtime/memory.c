// Резервные реализации memset/memcpy для сборки без CRT (/NODEFAULTLIB).
//
// Оптимизатор любой версии MSVC или clang имеет право синтезировать
// вызовы этих функций (инициализаторы структур, копирование, циклы),
// поэтому каждый модуль обязан носить свои определения — иначе линковка
// сломается при смене версии компилятора.

#include <stddef.h>
#include <string.h>
#include <intrin.h>
#include <windows.h>

void __cdecl __security_init_cookie(void);

// Все DLL проекта собираются без CRT и используют эту точку входа. Инициализация
// непредсказуемого /GS-cookie обязательна до первого защищённого вызова.
BOOL WINAPI LaiueDllEntryPoint(
    HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        __security_init_cookie();
        // У модулей нет TLS и thread attach hooks: не будим все DLL на
        // каждом worker thread стриминга или сетевого backend.
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

#if defined(_MSC_VER) && !defined(__clang__)
// Иначе cl считает функции интринсиками и не даёт их определить.
#pragma function(memset)
#pragma function(memcpy)
#endif

// Без CRT компилятор при использовании float ищет _fltused.
// Внешнее определение остаётся в обычном объектном файле без LTO, поэтому
// доступно и для ссылок, которые ThinLTO создаёт только во время кодогенерации.
// Само значение не меняется и не должно занимать writable-секцию.
const int _fltused = 0;

#if defined(__clang__)

// __stosb под clang понижается до @llvm.memset, а затем может стать вызовом
// этой же функции. Прямой GNU inline asm не проходит через LLVM memory
// intrinsics: в объекте гарантированно остаётся инструкция rep stosb.
void* memset(void* destination, int value, size_t count)
{
    void* result = destination;
    unsigned char* output = (unsigned char*)destination;

    __asm__ volatile(
        "rep stosb"
        : "+D"(output), "+c"(count)
        : "a"((unsigned char)value)
        : "memory");

    return result;
}

// __movsb под clang остаётся инструкцией rep movsb (проверено дизассемблером):
// memcpy-идиома его обратно в вызов memcpy не сворачивает.
void* memcpy(void* destination, const void* source, size_t count)
{
    __movsb((unsigned char*)destination, (const unsigned char*)source, count);
    return destination;
}

#else

// Реализация — интринсики rep stosb / rep movsb: на современных CPU
// (ERMSB) это аппаратная скорость, а рекурсия невозможна — компилятор
// выпускает одну инструкцию, а не вызов memset/memcpy.
void* memset(void* destination, int value, size_t count)
{
    __stosb((unsigned char*)destination, (unsigned char)value, count);
    return destination;
}

void* memcpy(void* destination, const void* source, size_t count)
{
    __movsb((unsigned char*)destination, (const unsigned char*)source, count);
    return destination;
}

#endif
