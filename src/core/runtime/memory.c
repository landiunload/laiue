// Резервные реализации memset/memcpy/memcmp для сборки без CRT (/NODEFAULTLIB).
//
// Оптимизатор любой версии MSVC или clang имеет право синтезировать
// вызовы этих функций (инициализаторы структур, копирование, циклы),
// поэтому каждый модуль обязан носить свои определения — иначе линковка
// сломается при смене версии компилятора.

#include <stddef.h>
#include <string.h>
#include <intrin.h>

#if defined(_MSC_VER) && !defined(__clang__)
// Иначе cl считает функции интринсиками и не даёт их определить.
#pragma function(memset)
#pragma function(memcpy)
#pragma function(memcmp)
#pragma function(memmove)
#endif

// Без CRT компилятор при использовании float ищет _fltused.
// Внешнее определение остаётся в обычном объектном файле без LTO, поэтому
// доступно и для ссылок, которые LTO создаёт только во время кодогенерации.
// Само значение не меняется и не должно занимать writable-секцию.
const int _fltused = 0;

#if defined(_M_ARM64) || defined(__aarch64__)

// У ARM64 нет rep-строковых инструкций и интринсиков __stosb/__movsb, поэтому
// резервные реализации — обычные циклы. volatile (и optnone под clang) не даёт
// распознавателю идиом свернуть цикл обратно в вызов той же самой функции.
#if defined(__clang__)
__attribute__((optnone))
#endif
void* memset(void* destination, int value, size_t count)
{
    volatile unsigned char* output = (volatile unsigned char*)destination;

    for (size_t index = 0; index < count; ++index)
    {
        output[index] = (unsigned char)value;
    }

    return destination;
}

#if defined(__clang__)
__attribute__((optnone))
#endif
void* memcpy(void* destination, const void* source, size_t count)
{
    volatile unsigned char* output = (volatile unsigned char*)destination;
    const volatile unsigned char* input = (const volatile unsigned char*)source;

    for (size_t index = 0; index < count; ++index)
    {
        output[index] = input[index];
    }

    return destination;
}

#elif defined(__clang__)

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

// IsEqualIID и сравнение структур раскрываются в memcmp. В Release компилятор
// разворачивает его на месте, а с /Od выпускает настоящий вызов — без CRT
// линковка падала на неразрешённом символе.
//
// Под clang распознавание идиом умеет свернуть такой цикл обратно в вызов
// memcmp, то есть в рекурсию: optnone это исключает. Функция сравнивает
// короткие структуры вроде GUID, скорость здесь роли не играет.
#if defined(__clang__)
__attribute__((optnone))
#endif
int memcmp(const void* first, const void* second, size_t count)
{
    const unsigned char* left = (const unsigned char*)first;
    const unsigned char* right = (const unsigned char*)second;

    for (size_t index = 0; index < count; ++index)
    {
        if (left[index] != right[index])
        {
            return (int)left[index] - (int)right[index];
        }
    }

    return 0;
}

// Оптимизатор может синтезировать memmove для перекрывающихся сдвигов
// (например, при форматировании IPv6 endpoint). volatile не даёт свернуть
// этот перенос обратно во внешний вызов memmove и получить рекурсию.
#if defined(__clang__)
__attribute__((optnone))
#endif
void* memmove(void* destination, const void* source, size_t count)
{
    volatile unsigned char* output = (volatile unsigned char*)destination;
    const volatile unsigned char* input =
        (const volatile unsigned char*)source;
    if (output < input)
    {
        for (size_t i = 0; i < count; ++i) output[i] = input[i];
    }
    else if (output > input)
    {
        for (size_t i = count; i > 0; --i) output[i - 1U] = input[i - 1U];
    }
    return destination;
}
