// Узкий регрессионный тест UI-путей (агент 31-ui, ROUND 2).
//
// Проверяет без тайминга:
//   * UiFontFindGlyph — точное соответствие "кодпоинт -> глиф" для всего
//     запечённого набора, строгий порядок, отказ для отсутствующих
//     кодпоинтов (быстрый O(1)-индекс и запасной поиск дают тот же ответ);
//   * UiFontMeasure/UiTextWidth — совпадение с суммой авансов;
//   * UiTextBuilder/UiFormat* — терминация, усечение по ёмкости, границы
//     значений (0, UINT64_MAX, часы, градусы);
//   * UiText/UiTextCentered/UiRect/UiImage/клип — точная геометрия квадов,
//     порядок, флаги, отсечение.
//
// Тест не измеряет время и не зависит от производительности.

#include "ui/ui.h"
#include "ui/ui_format.h"

#include "platform/system.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t gFailures = 0u;
static uint32_t gChecks = 0u;

static void WriteText(const char* text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
    {
        digits[length++] = '0';
    }
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

static void Fail(const char* message, uint32_t line)
{
    ++gFailures;
    WriteText("FAIL line=");
    WriteUnsigned(line);
    WriteText(" ");
    WriteText(message);
    WriteText("\n");
}

#define CHECK(condition, message) \
    do { ++gChecks; if (!(condition)) { Fail(message, __LINE__); } } while (0)

static bool WideEqual(const wchar_t* left, const wchar_t* right)
{
    while (*left != L'\0' && *right != L'\0')
    {
        if (*left != *right)
        {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == *right;
}

static bool FloatEqual(float left, float right)
{
    float difference = left - right;
    if (difference < 0.0f)
    {
        difference = -difference;
    }
    return difference <= 0.0f;
}

static void TestFontLookup(const UiContext* ui)
{
    const UiFont* font = &ui->font;
    CHECK(font->glyphCount == 162u, "glyph count is 162");
    CHECK(font->glyphs != NULL, "glyph array allocated");

    // Строгий возрастающий порядок без дублей.
    bool sorted = true;
    for (uint32_t index = 1u; index < font->glyphCount; ++index)
    {
        if (font->glyphs[index - 1u].codepoint >= font->glyphs[index].codepoint)
        {
            sorted = false;
        }
    }
    CHECK(sorted, "glyphs sorted and unique");

    // Каждый запечённый кодпоинт находится ровно по своему индексу.
    bool allFound = true;
    for (uint32_t index = 0u; index < font->glyphCount; ++index)
    {
        const UiGlyph* found =
            UiFontFindGlyph(font, font->glyphs[index].codepoint);
        if (found != &font->glyphs[index])
        {
            allFound = false;
        }
    }
    CHECK(allFound, "every glyph found by codepoint");

    // Документированные диапазоны: выборка известных точек.
    const uint16_t present[] = {
        0x0020u, 0x0041u, 0x007Au, 0x00B0u, 0x0401u, 0x0410u, 0x044Fu, 0x0451u
    };
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        CHECK(UiFontFindGlyph(font, present[index]) != NULL,
            "documented codepoint present");
    }

    // Точки вне запечённых диапазонов отсутствуют.
    const uint16_t absent[] = {
        0x001Fu, 0x007Fu, 0x0100u, 0x0400u, 0x0402u, 0x040Fu,
        0x0450u, 0x0452u, 0x4E00u, 0xFFFFu
    };
    bool noneFound = true;
    for (uint32_t index = 0u; index < 10u; ++index)
    {
        if (UiFontFindGlyph(font, absent[index]) != NULL)
        {
            noneFound = false;
        }
    }
    CHECK(noneFound, "absent codepoints return NULL");
}

static void TestMeasure(const UiContext* ui)
{
    const wchar_t* text = L"ABC abc 0123 °Ёё";
    float expected = 0.0f;
    for (const wchar_t* character = text; *character != L'\0'; ++character)
    {
        const UiGlyph* glyph = UiFontFindGlyph(&ui->font, (uint16_t)*character);
        if (glyph != NULL)
        {
            expected += glyph->advance;
        }
    }
    CHECK(UiFontMeasure(&ui->font, text) == expected, "measure equals advance sum");
    CHECK(UiTextWidth(ui, text) == expected, "UiTextWidth equals UiFontMeasure");
    CHECK(UiFontMeasure(&ui->font, L"") == 0.0f, "empty measure is zero");

    // Отсутствующие кодпоинты не добавляют ширины.
    CHECK(UiFontMeasure(&ui->font, L"\x4E00\xFFFF") == 0.0f,
        "unsupported codepoints add no width");
}

static void TestBuilderAndFormat(void)
{
    wchar_t buffer[32];
    UiTextBuilder builder;

    UiTextBuilderInit(&builder, buffer, 4u);
    UiTextBuilderAppend(&builder, L"abcdef");
    CHECK(builder.length == 3u, "append truncates to capacity-1");
    CHECK(WideEqual(buffer, L"abc"), "append terminates at capacity");

    UiTextBuilderInit(&builder, buffer, 4u);
    UiTextBuilderAppendUnsigned(&builder, 12345u);
    CHECK(WideEqual(buffer, L"123"), "unsigned truncates to capacity-1");

    UiTextBuilderInit(&builder, buffer, 32u);
    UiTextBuilderAppendUnsigned(&builder, UINT64_MAX);
    CHECK(WideEqual(buffer, L"18446744073709551615"),
        "UINT64_MAX formats fully");
    CHECK(builder.length == 20u, "UINT64_MAX length is 20");

    UiTextBuilderInit(&builder, buffer, 32u);
    UiTextBuilderAppendUnsigned(&builder, 0u);
    CHECK(WideEqual(buffer, L"0"), "zero formats as 0");

    // Ёмкость 0 и NULL-приёмник не пишут и не падают.
    UiTextBuilderInit(&builder, buffer, 0u);
    UiTextBuilderAppend(&builder, L"ignored");
    UiTextBuilderAppendUnsigned(&builder, 7u);
    CHECK(builder.length == 0u, "zero capacity keeps length zero");
    UiTextBuilderInit(&builder, NULL, 0u);
    UiTextBuilderAppend(&builder, L"ignored");
    UiTextBuilderAppendUnsigned(&builder, 7u);
    CHECK(builder.length == 0u && builder.destination == NULL,
        "NULL destination is a no-op");

    UiFormatUnsigned(buffer, 32u, 0u);
    CHECK(WideEqual(buffer, L"0"), "UiFormatUnsigned zero");
    UiFormatUnsignedSuffix(buffer, 32u, 1234u, L" ms");
    CHECK(WideEqual(buffer, L"1234 ms"), "UiFormatUnsignedSuffix");
    UiFormatDegrees(buffer, 32u, 45);
    CHECK(WideEqual(buffer, L"45\x00B0"), "UiFormatDegrees positive");
    UiFormatDegrees(buffer, 32u, 0);
    CHECK(WideEqual(buffer, L"1\x00B0"), "UiFormatDegrees clamps to 1");
    UiFormatClock(buffer, 32u, 0u);
    CHECK(WideEqual(buffer, L"00:00"), "UiFormatClock zero");
    UiFormatClock(buffer, 32u, 90u);
    CHECK(WideEqual(buffer, L"01:30"), "UiFormatClock 01:30");
    UiFormatClock(buffer, 32u, 1515u);
    CHECK(WideEqual(buffer, L"01:15"), "UiFormatClock wraps hours");
    UiFormatUnsignedSuffix(buffer, 32u, 9u, NULL);
    CHECK(WideEqual(buffer, L"9"), "NULL suffix appends nothing");
}

static void TestQuads(UiContext* ui)
{
    if (!UiBegin(ui, 640, 480, 0.0f, 0.0f, false, false, 0.0f, 0.0f))
    {
        CHECK(false, "UiBegin bakes font");
        return;
    }
    UiClearClip(ui);

    // Один глиф: точная геометрия, UV, цвет и флаг.
    UiText(ui, 100.0f, 50.0f, 0xFFAABBCCu, L"A");
    CHECK(ui->quadCount == 1u, "UiText A pushes one quad");
    const UiGlyph* glyph = UiFontFindGlyph(&ui->font, (uint16_t)L'A');
    if (glyph != NULL && ui->quadCount == 1u)
    {
        const RendererUiQuad* quad = &ui->quads[0];
        float glyphX = 100.0f + (float)glyph->offsetX;
        float baseline = 50.0f + ui->font.ascent;
        float glyphY = baseline - (float)glyph->offsetY;
        CHECK(FloatEqual(quad->rect[0], glyphX), "glyph rect x0");
        CHECK(FloatEqual(quad->rect[1], glyphY), "glyph rect y0");
        CHECK(FloatEqual(quad->rect[2], glyphX + (float)glyph->width),
            "glyph rect x1");
        CHECK(FloatEqual(quad->rect[3], glyphY + (float)glyph->height),
            "glyph rect y1");
        CHECK(quad->uv[0] == glyph->u0 && quad->uv[1] == glyph->v0
            && quad->uv[2] == glyph->u1 && quad->uv[3] == glyph->v1,
            "glyph UV copied");
        CHECK(quad->colorRGBA == 0xFFAABBCCu, "glyph color copied");
        CHECK(quad->flags == RENDERER_UI_QUAD_TEXT, "glyph flag is text");
    }

    // Пробел не даёт квада; две буквы дают два.
    if (UiBegin(ui, 640, 480, 0.0f, 0.0f, false, false, 0.0f, 0.0f))
    {
        UiClearClip(ui);
        UiText(ui, 10.0f, 10.0f, 0xFF000000u, L"A B");
        CHECK(ui->quadCount == 2u, "space emits no quad");
    }

    // Клип: полностью снаружи — ноль квадов; внутри — один.
    if (UiBegin(ui, 640, 480, 0.0f, 0.0f, false, false, 0.0f, 0.0f))
    {
        UiSetClip(ui, 500.0f, 400.0f, 100.0f, 100.0f);
        UiText(ui, 0.0f, 0.0f, 0xFF000000u, L"A");
        CHECK(ui->quadCount == 0u, "text outside clip is dropped");
        UiRect(ui, 520.0f, 420.0f, 10.0f, 10.0f, 0.0f, 0xFF112233u);
        CHECK(ui->quadCount == 1u, "rect inside clip kept");
        UiRect(ui, 0.0f, 0.0f, 10.0f, 10.0f, 0.0f, 0xFF112233u);
        CHECK(ui->quadCount == 1u, "rect outside clip dropped");
    }

    // UiImage: флаг и UV.
    if (UiBegin(ui, 640, 480, 0.0f, 0.0f, false, false, 0.0f, 0.0f))
    {
        UiClearClip(ui);
        UiImage(ui, 0.0f, 0.0f, 10.0f, 20.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0xFFFFFFFFu);
        CHECK(ui->quadCount == 1u, "image pushes one quad");
        CHECK(ui->quads[0].flags == RENDERER_UI_QUAD_IMAGE, "image flag");
        CHECK(ui->quads[0].uv[0] == 0.1f && ui->quads[0].uv[3] == 0.4f,
            "image UV copied");
        UiImage(ui, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
        CHECK(ui->quadCount == 1u, "zero-size image dropped");
    }

    // Центрирование: pen сдвинут на половину измеренной ширины.
    if (UiBegin(ui, 640, 480, 0.0f, 0.0f, false, false, 0.0f, 0.0f))
    {
        UiClearClip(ui);
        UiTextCentered(ui, 300.0f, 50.0f, 0xFF445566u, L"A");
        if (glyph != NULL && ui->quadCount == 1u)
        {
            float width = UiFontMeasure(&ui->font, L"A");
            float penX = 300.0f - width * 0.5f;
            CHECK(FloatEqual(ui->quads[0].rect[0],
                penX + (float)glyph->offsetX), "centered pen position");
        }
    }
}

LAIUE_TEST_ENTRY(R2UiTestEntryPoint)
{
    UiContext* ui = (UiContext*)PlatformAllocate(sizeof(UiContext), true);
    if (ui == NULL)
    {
        WriteText("UiContext allocation failed\n");
        LaiueTestRuntimeExit(1);
    }

    if (!UiBegin(ui, 1280, 720, 0.0f, 0.0f, false, false, 0.0f, 0.0f))
    {
        WriteText("font bake failed\n");
        LaiueTestRuntimeExit(1);
    }

    TestFontLookup(ui);
    TestMeasure(ui);
    TestBuilderAndFormat();
    TestQuads(ui);

    WriteText("checks=");
    WriteUnsigned(gChecks);
    WriteText(" failures=");
    WriteUnsigned(gFailures);
    WriteText("\n");

    UiRelease(ui);
    PlatformFree(ui);

    if (gFailures != 0u)
    {
        LaiueTestRuntimeExit(1);
    }
    WriteText("LAIUE_TEST_SUCCESS\n");
    LAIUE_TEST_SUCCESS();
}
