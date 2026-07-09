// Резервные реализации memset/memcpy для сборки без CRT (/NODEFAULTLIB).
//
// Оптимизатор любой версии MSVC или clang имеет право синтезировать
// вызовы этих функций (инициализаторы структур, копирование, циклы),
// поэтому каждый модуль обязан носить свои определения — иначе линковка
// сломается при смене версии компилятора.
//
// На скорость это не влияет: на местах вызова компилятор по-прежнему
// инлайнит интринсики (/Oi); эти функции — только страховка для случаев,
// когда он решает позвать библиотечную версию.

#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
// Иначе cl считает функции интринсиками и не даёт их определить.
#pragma function(memset)
#pragma function(memcpy)
#endif

// volatile не даёт оптимизатору распознать в цикле идиому
// и свернуть тело обратно в вызов memset/memcpy (бесконечная рекурсия).

void* memset(void* destination, int value, size_t count)
{
    volatile unsigned char* destinationBytes = destination;
    while (count-- > 0)
    {
        *destinationBytes++ = (unsigned char)value;
    }

    return destination;
}

void* memcpy(void* destination, const void* source, size_t count)
{
    volatile unsigned char* destinationBytes = destination;
    const unsigned char* sourceBytes = source;
    while (count-- > 0)
    {
        *destinationBytes++ = *sourceBytes++;
    }

    return destination;
}
