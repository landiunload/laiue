// Резервные реализации memset/memcpy для сборки без CRT (/NODEFAULTLIB).
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
#endif

// Без CRT компилятор при использовании float ищет _fltused.
// Достаточно самого факта существования символа.
int _fltused = 0;

// Заглушка stack probe: BigCoord (4160 байт) триггерит __chkstk на x64,
// но без CRT его неоткуда взять. Поскольку guard-страницы стека у нас
// не настроены, probe можно оставить пустым.
void __chkstk(void) { }

#if defined(__clang__)

// ВНИМАНИЕ: под clang memset нельзя реализовывать через __stosb. clang
// понижает __stosb до @llvm.memset, а тот в CodeGen — до вызова memset,
// т.е. самой этой функции: `memset` компилировался в `jmp memset`
// (бесконечный цикл) или `call memset` (рекурсия -> переполнение стека).
// Ни -fno-builtin, ни optnone это не снимают — понижение идёт в бэкенде.
//
// Решение: цикл через volatile-указатель. LoopIdiomRecognize не сворачивает
// volatile-доступы обратно в memset, поэтому самовызов не возникает.
// Пословный проход держит скорость близко к rep stosb (упор в полосу памяти).
void* memset(void* destination, int value, size_t count)
{
    volatile unsigned char* bytes = (volatile unsigned char*)destination;
    unsigned char single = (unsigned char)value;

    // Голова до 8-байтового выравнивания.
    while (count != 0 && ((size_t)bytes & 7u) != 0)
    {
        *bytes++ = single;
        --count;
    }

    // Тело пословно.
    unsigned long long word = 0x0101010101010101ull * (unsigned long long)single;
    volatile unsigned long long* words = (volatile unsigned long long*)bytes;
    while (count >= 8)
    {
        *words++ = word;
        count -= 8;
    }

    // Хвост.
    bytes = (volatile unsigned char*)words;
    while (count != 0)
    {
        *bytes++ = single;
        --count;
    }

    return destination;
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
