// Резервные реализации memset/memcpy для сборки без CRT (/NODEFAULTLIB).
//
// Оптимизатор любой версии MSVC или clang имеет право синтезировать
// вызовы этих функций (инициализаторы структур, копирование, циклы),
// поэтому каждый модуль обязан носить свои определения — иначе линковка
// сломается при смене версии компилятора.
//
// Реализация — интринсики rep stosb / rep movsb: на современных CPU
// (ERMSB) это аппаратная скорость, а рекурсия невозможна — компилятор
// выпускает одну инструкцию, а не вызов memset/memcpy.

#include <stddef.h>
#include <string.h>
#include <intrin.h>

#if defined(_MSC_VER) && !defined(__clang__)
// Иначе cl считает функции интринсиками и не даёт их определить.
#pragma function(memset)
#pragma function(memcpy)
#endif

// Без CRT компилятор при использовании float ищет _fltused.
// Достаточно самого факта существования символа.
int _fltused = 0;

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
