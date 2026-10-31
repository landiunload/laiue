#pragma once

#include <stdint.h>

/* The character an X11 keysym types, or 0 when it types none. Covers Latin-1,
 * the Unicode keysyms, the legacy Cyrillic and currency keysyms and the
 * keypad with Num Lock. Portable, so every platform's tests check it. */
uint32_t LaiueKeysymToCodepoint(uint32_t keysym);
