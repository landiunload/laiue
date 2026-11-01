#include "platform/keysym.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static uint32_t failureCount;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    ++failureCount;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
}

/* The 32 keysyms 0x6c0-0x6df are а to я, each once, and the keysym 0x20
 * above each is its capital. */
static void TestRussianAlphabet(void)
{
    uint32_t seen = 0u;
    bool mirrored = true;
    for (uint32_t keysym = 0x6C0u; keysym <= 0x6DFu; ++keysym)
    {
        const uint32_t small = LaiueKeysymToCodepoint(keysym);
        if (small < 0x0430u || small > 0x044Fu)
        {
            mirrored = false;
            continue;
        }
        seen |= 1u << (small - 0x0430u);
        mirrored = mirrored && LaiueKeysymToCodepoint(keysym + 0x20u) == small - 0x20u;
    }
    Expect(mirrored && seen == UINT32_MAX, "0x6c0-0x6ff hold а to я and А to Я once each");
}

LAIUE_TEST_ENTRY(KeysymTestEntryPoint)
{
    Expect(LaiueKeysymToCodepoint(0x61u) == 0x61u && LaiueKeysymToCodepoint(0xE9u) == 0xE9u,
           "Latin-1 keysyms are their characters");
    Expect(LaiueKeysymToCodepoint(0x6C1u) == 0x0430u && LaiueKeysymToCodepoint(0x6F1u) == 0x042Fu,
           "Cyrillic_a is а and Cyrillic_YA is Я");
    Expect(LaiueKeysymToCodepoint(0x6A3u) == 0x0451u && LaiueKeysymToCodepoint(0x6B3u) == 0x0401u,
           "Cyrillic_io and Cyrillic_IO are ё and Ё");
    Expect(LaiueKeysymToCodepoint(0x6ADu) == 0x0491u && LaiueKeysymToCodepoint(0x6BDu) == 0x0490u,
           "the Ukrainian ghe with upturn is ґ and Ґ");
    Expect(LaiueKeysymToCodepoint(0x6B0u) == 0x2116u, "numerosign is №");
    TestRussianAlphabet();
    Expect(LaiueKeysymToCodepoint(0x20ACu) == 0x20ACu, "EuroSign is €");
    Expect(LaiueKeysymToCodepoint(0x10020ACu) == 0x20ACu,
           "a Unicode keysym carries its code point");
    Expect(LaiueKeysymToCodepoint(0x100D800u) == 0u, "a surrogate is no character");
    Expect(LaiueKeysymToCodepoint(0xFFB5u) == '5' && LaiueKeysymToCodepoint(0xFFAAu) == '*' &&
               LaiueKeysymToCodepoint(0xFFBDu) == '=' && LaiueKeysymToCodepoint(0xFF80u) == ' ',
           "the keypad types digits and signs");
    Expect(LaiueKeysymToCodepoint(0xFF0Du) == 0u && LaiueKeysymToCodepoint(0xFF08u) == 0u &&
               LaiueKeysymToCodepoint(0xFF9Cu) == 0u && LaiueKeysymToCodepoint(0u) == 0u,
           "Return, BackSpace, KP_End and NoSymbol type nothing");
    if (failureCount != 0u)
        LaiueTestRuntimeExit(1);
    LAIUE_TEST_SUCCESS();
}
