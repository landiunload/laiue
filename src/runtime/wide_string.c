// Резервная реализация wcslen для Windows no-CRT сборки.
//
// Файл намеренно не подключает UCRT-заголовки: они объявляют wcslen как
// dllimport и не позволяют дать модулю локальное определение. wchar_t и
// size_t предоставляет стандартный <stddef.h>.

#include <stddef.h>

#if defined(_MSC_VER) && !defined(__clang__)
size_t wcslen(const wchar_t *text);
#pragma function(wcslen)
#endif

// Release-оптимизатор clang-cl распознаёт ручной поиск конца wide-строки и
// заменяет его вызовом wcslen. volatile вместе с optnone не позволяет
// свернуть реализацию в рекурсивный вызов самой себя.
#if defined(__clang__)
__attribute__((optnone))
#endif
size_t wcslen(const wchar_t *text)
{
    const volatile wchar_t *input = (const volatile wchar_t *)text;
    size_t length = 0;
    while (input[length] != L'\0')
    {
        ++length;
    }
    return length;
}
