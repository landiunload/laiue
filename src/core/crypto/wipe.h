#pragma once

#include <stddef.h>
#include <string.h>

/* memset через volatile-указатель: компилятор не знает, какую функцию
 * вызывает, и не может выбросить обнуление как мёртвую запись. Побайтовый
 * volatile-цикл тоже не выбрасывается, но пишет по байту за такт. */
static void *(*const volatile cryptoWipeMemset)(void *, int, size_t) = memset;

/* Стирает секрет так, чтобы компилятор не выбросил запись. */
static inline void CryptoWipe(void *bytes, size_t size)
{
    if (size != 0u)
        (void)cryptoWipeMemset(bytes, 0, size);
}
