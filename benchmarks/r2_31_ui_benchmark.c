// Ручной A/B-стенд UI (ROUND 2, агент 31-ui).
//
// Слинкован с настоящей laiue_ui.dll: один и тот же исполняемый файл
// сравнивается с baseline- и candidate-DLL, разложенными в отдельные
// каталоги вместе со всеми зависимостями. Стенд не входит ни в ALL, ни в
// CTest; включается LAIUE_BUILD_BENCHMARKS и запускается A/B-скриптом.
//
// Стадии:
//   menu_latin     - типичное меню из виджетов, латинские подписи;
//   menu_cyrillic  - то же, кириллические подписи (boundary);
//   text_draw      - чистый путь отрисовки длинных строк;
//   text_measure   - чистый путь UiTextWidth;
//   format         - построение строк UiFormat*.
//
// Вывод машинно-читаемый, по одной строке на sample:
//   stage=<name> sample=<i> ns=<общее нс> frames=<n> quads=<last> checksum=<hex>
// затем одна строка mem= и verify=ok. checksum — FNV-1a по байтам квадов
// кадра (или по байтам измеренной ширины/строки) и обязан совпасть у
// baseline и candidate: иначе работа/результат отличаются.

#include "ui/ui.h"
#include "ui/ui_format.h"

#include "platform/system.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

#define UI_WIDTH 1280
#define UI_HEIGHT 720
#define UI_DELTA_SECONDS (1.0f / 60.0f)

// Числа кадров подобраны так, чтобы каждый sample занимал >=100 мс:
// длинный прогон разбит на короткие блоки, общий A/B-прогон укладывается
// в один bench-lock до 2-3 минут.
#define MENU_FRAMES 16000u
#define TEXT_DRAW_FRAMES 1500u
#define TEXT_MEASURE_FRAMES 14000u
#define TEXT_UNSUPPORTED_FRAMES 60000u
#define FORMAT_FRAMES 4000u
#define STAGE_SAMPLES 5u

#define FNV_OFFSET 1469598103934665603ull
#define FNV_PRIME 1099511628211ull

typedef uint32_t (*StageFn)(UiContext* ui, uint64_t* checksum);

static const wchar_t* const LATIN_NAMES[6] = {
    L"Volume", L"Field of view", L"Render distance",
    L"Pointer sensitivity", L"Gamma", L"Shadow quality"
};
static const wchar_t* const LATIN_VALUES[6] = {
    L"75", L"90", L"12 chunks", L"2.50", L"1.00", L"High"
};
static const wchar_t* const CYRILLIC_NAMES[6] = {
    L"Громкость", L"Угол обзора", L"Дальность прорисовки",
    L"Чувствительность мыши", L"Гамма", L"Качество теней"
};
static const wchar_t* const CYRILLIC_VALUES[6] = {
    L"75", L"90°", L"12 чанков", L"2,50", L"1,00", L"Высокое"
};

static const wchar_t* const SEGMENTS_LATIN[4] = {
    L"Low", L"Medium", L"High", L"Ultra"
};
static const wchar_t* const SEGMENTS_CYRILLIC[4] = {
    L"Низко", L"Средне", L"Высоко", L"Ультра"
};
static const wchar_t* const RADIOS_LATIN[4] = {
    L"Windowed", L"Borderless", L"Fullscreen", L"Exclusive fullscreen"
};
static const wchar_t* const RADIOS_CYRILLIC[4] = {
    L"В окне", L"Без рамки", L"Полный экран", L"Эксклюзивный экран"
};

static const wchar_t* const LONG_LATIN =
    L"The quick brown fox jumps over the lazy dog 0123456789 - UI text hot path";
static const wchar_t* const LONG_CYRILLIC =
    L"Съешь же ещё этих мягких французских булок, да выпей чаю 0123456789";

// Неблагоприятный случай: строка с кодпоинтами вне запечённых диапазонов
// (дырки между диапазонами и значения вне набора) вперемешку с
// поддерживаемыми. Такие глифы не рисуются и не дают ширины, но поиск по
// ним не должен стать дороже базового бинарного.
static const wchar_t* const MIXED_UNSUPPORTED =
    L"\u00E9\u0402\u0450\u4E00\uFFFF abc ABC \u00B0\u0401\u0451 foo bar";

static int32_t gSliderValue = 40;
static bool gToggleValue = true;
static int32_t gSegmentIndex = 2;

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

static void WriteHex64(uint64_t value)
{
    static const char hexDigits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        uint32_t shift = (15u - index) * 4u;
        text[index] = hexDigits[(value >> shift) & 0xFu];
    }
    text[16] = '\0';
    WriteText(text);
}

static void HashBytes(uint64_t* checksum, const void* data, size_t count)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint64_t value = *checksum;
    for (size_t index = 0u; index < count; ++index)
    {
        value ^= (uint64_t)bytes[index];
        value *= FNV_PRIME;
    }
    *checksum = value;
}

static void HashWide(uint64_t* checksum, const wchar_t* text)
{
    for (const wchar_t* character = text; *character != L'\0'; ++character)
    {
        HashBytes(checksum, character, sizeof(wchar_t));
    }
}

static void HashQuads(UiContext* ui, uint64_t* checksum)
{
    HashBytes(checksum, ui->quads, (size_t)ui->quadCount * sizeof(RendererUiQuad));
}

static uint64_t NowNanoseconds(void)
{
    double seconds = PlatformMonotonicSeconds();
    return (uint64_t)(seconds * 1000000000.0 + 0.5);
}

static void ReportSample(const char* stage, uint32_t sample, uint64_t nanoseconds,
    uint32_t frames, uint32_t quads, uint64_t checksum)
{
    WriteText("stage=");
    WriteText(stage);
    WriteText(" sample=");
    WriteUnsigned(sample);
    WriteText(" ns=");
    WriteUnsigned(nanoseconds);
    WriteText(" frames=");
    WriteUnsigned(frames);
    WriteText(" quads=");
    WriteUnsigned(quads);
    WriteText(" checksum=");
    WriteHex64(checksum);
    WriteText("\n");
}

static uint32_t RenderMenu(UiContext* ui, bool cyrillic)
{
    const wchar_t* const* names = cyrillic ? CYRILLIC_NAMES : LATIN_NAMES;
    const wchar_t* const* values = cyrillic ? CYRILLIC_VALUES : LATIN_VALUES;
    const wchar_t* const* segments = cyrillic ? SEGMENTS_CYRILLIC : SEGMENTS_LATIN;
    const wchar_t* const* radios = cyrillic ? RADIOS_CYRILLIC : RADIOS_LATIN;

    if (!UiBegin(ui, UI_WIDTH, UI_HEIGHT, 220.0f, 375.0f,
        false, false, 0.0f, UI_DELTA_SECONDS))
    {
        return 0u;
    }

    UiPanel(ui, 60.0f, 40.0f, 520.0f, 400.0f);

    float y = 70.0f;
    for (uint32_t index = 0u; index < 6u; ++index)
    {
        y = UiLabelValueRow(ui, 90.0f, 460.0f, y,
            names[index], values[index], 6.0f);
    }

    UiSegmented(ui, 0x3100u, 90.0f, 205.0f, 460.0f, 30.0f,
        segments, 4, &gSegmentIndex);
    UiToggle(ui, 0x3200u, 90.0f, 250.0f, &gToggleValue);
    UiSliderInt(ui, 0x3300u, 90.0f, 300.0f, 460.0f, 0, 100, &gSliderValue);

    UiButton(ui, 0x3400u, 90.0f, 360.0f, 150.0f, 34.0f, cyrillic ? L"Применить" : L"Apply");
    UiButton(ui, 0x3500u, 250.0f, 360.0f, 150.0f, 34.0f, cyrillic ? L"Сбросить" : L"Reset");
    UiButton(ui, 0x3600u, 410.0f, 360.0f, 150.0f, 34.0f, cyrillic ? L"Выход" : L"Quit");

    for (uint32_t index = 0u; index < 4u; ++index)
    {
        UiRadioRow(ui, 0x3700u + index, 90.0f, 400.0f + (float)index * 20.0f,
            460.0f, 18.0f, radios[index], index == 1u);
    }

    return ui->quadCount;
}

static uint32_t StageMenuLatin(UiContext* ui, uint64_t* checksum)
{
    uint32_t quads = RenderMenu(ui, false);
    HashQuads(ui, checksum);
    return quads;
}

static uint32_t StageMenuCyrillic(UiContext* ui, uint64_t* checksum)
{
    uint32_t quads = RenderMenu(ui, true);
    HashQuads(ui, checksum);
    return quads;
}

static uint32_t StageTextDraw(UiContext* ui, uint64_t* checksum)
{
    UiBegin(ui, UI_WIDTH, UI_HEIGHT, 0.0f, 0.0f,
        false, false, 0.0f, UI_DELTA_SECONDS);
    for (uint32_t line = 0u; line < 30u; ++line)
    {
        UiText(ui, 40.0f, 20.0f + (float)line * 18.0f, UI_COLOR_TEXT, LONG_LATIN);
    }
    HashQuads(ui, checksum);
    return ui->quadCount;
}

static uint32_t StageTextMeasure(UiContext* ui, uint64_t* checksum)
{
    UiBegin(ui, UI_WIDTH, UI_HEIGHT, 0.0f, 0.0f,
        false, false, 0.0f, UI_DELTA_SECONDS);
    float width = 0.0f;
    for (uint32_t index = 0u; index < 40u; ++index)
    {
        width += UiTextWidth(ui, (index & 1u) != 0u ? LONG_CYRILLIC : LONG_LATIN);
    }
    HashBytes(checksum, &width, sizeof(width));
    return ui->quadCount;
}

static uint32_t StageTextUnsupported(UiContext* ui, uint64_t* checksum)
{
    UiBegin(ui, UI_WIDTH, UI_HEIGHT, 0.0f, 0.0f,
        false, false, 0.0f, UI_DELTA_SECONDS);
    float width = 0.0f;
    for (uint32_t line = 0u; line < 40u; ++line)
    {
        UiText(ui, 40.0f, 20.0f + (float)line * 18.0f, UI_COLOR_TEXT,
            MIXED_UNSUPPORTED);
        width += UiTextWidth(ui, MIXED_UNSUPPORTED);
    }
    HashQuads(ui, checksum);
    HashBytes(checksum, &width, sizeof(width));
    return ui->quadCount;
}

static uint32_t StageFormat(UiContext* ui, uint64_t* checksum)
{
    (void)ui;
    wchar_t buffer[64];
    for (uint32_t index = 0u; index < 500u; ++index)
    {
        UiFormatUnsigned(buffer, 64u, (uint64_t)index * 2654435761u);
        HashWide(checksum, buffer);
        UiFormatUnsignedSuffix(buffer, 64u, (uint64_t)index, L" ms");
        HashWide(checksum, buffer);
        UiFormatDegrees(buffer, 64u, (int32_t)(index % 360u));
        HashWide(checksum, buffer);
        UiFormatClock(buffer, 64u, index * 7u);
        HashWide(checksum, buffer);
    }
    return 0u;
}

static void WarmStage(StageFn stage, UiContext* ui, uint32_t frames)
{
    uint64_t checksum = FNV_OFFSET;
    for (uint32_t frame = 0u; frame < frames; ++frame)
    {
        stage(ui, &checksum);
    }
}

static void RunStage(const char* name, StageFn stage, UiContext* ui,
    uint32_t frames)
{
    for (uint32_t sample = 0u; sample < STAGE_SAMPLES; ++sample)
    {
        uint64_t checksum = FNV_OFFSET;
        uint64_t start = NowNanoseconds();
        uint32_t quads = 0u;
        for (uint32_t frame = 0u; frame < frames; ++frame)
        {
            quads = stage(ui, &checksum);
        }
        uint64_t elapsed = NowNanoseconds() - start;
        ReportSample(name, sample, elapsed, frames, quads, checksum);
    }
}

static void ReportFont(const UiContext* ui)
{
    WriteText("font_glyphs=");
    WriteUnsigned(ui->font.glyphCount);
    WriteText(" font_atlas=");
    WriteUnsigned(ui->font.atlasWidth);
    WriteText("x");
    WriteUnsigned(ui->font.atlasHeight);
    WriteText(" font_pixel=");
    WriteUnsigned((uint64_t)ui->font.pixelSize);
    WriteText("\n");
}

#if defined(_WIN32)
static void ReportMemory(void)
{
    PROCESS_MEMORY_COUNTERS counters;
    for (size_t index = 0u; index < sizeof(counters); ++index)
    {
        ((uint8_t*)&counters)[index] = 0u;
    }
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    {
        WriteText("mem peak_wset=");
        WriteUnsigned((uint64_t)counters.PeakWorkingSetSize);
        WriteText(" peak_commit=");
        WriteUnsigned((uint64_t)counters.PeakPagefileUsage);
        WriteText(" wset=");
        WriteUnsigned((uint64_t)counters.WorkingSetSize);
        WriteText(" commit=");
        WriteUnsigned((uint64_t)counters.PagefileUsage);
        WriteText("\n");
    }
}
#endif

LAIUE_TEST_ENTRY(R2UiBenchmarkEntryPoint)
{
    UiContext* ui = (UiContext*)PlatformAllocate(sizeof(UiContext), true);
    if (ui == NULL)
    {
        WriteText("UiContext allocation failed\n");
        LaiueTestRuntimeExit(1);
    }

    if (!UiBegin(ui, UI_WIDTH, UI_HEIGHT, 0.0f, 0.0f,
        false, false, 0.0f, UI_DELTA_SECONDS))
    {
        WriteText("font bake failed\n");
        LaiueTestRuntimeExit(1);
    }
    ReportFont(ui);

    // Прогрев вне статистики: первая печь, первые анимации, кэш команд.
    WarmStage(StageMenuLatin, ui, 500u);
    WarmStage(StageMenuCyrillic, ui, 500u);
    WarmStage(StageTextDraw, ui, 200u);
    WarmStage(StageTextMeasure, ui, 500u);
    WarmStage(StageTextUnsupported, ui, 500u);
    WarmStage(StageFormat, ui, 200u);
    WriteText("warmup=ok\n");

    RunStage("menu_latin", StageMenuLatin, ui, MENU_FRAMES);
    RunStage("menu_cyrillic", StageMenuCyrillic, ui, MENU_FRAMES);
    RunStage("text_draw", StageTextDraw, ui, TEXT_DRAW_FRAMES);
    RunStage("text_measure", StageTextMeasure, ui, TEXT_MEASURE_FRAMES);
    RunStage("text_unsupported", StageTextUnsupported, ui, TEXT_UNSUPPORTED_FRAMES);
    RunStage("format", StageFormat, ui, FORMAT_FRAMES);

#if defined(_WIN32)
    ReportMemory();
#endif

    WriteText("verify=ok\n");
    UiRelease(ui);
    PlatformFree(ui);
    LAIUE_TEST_SUCCESS();
}
