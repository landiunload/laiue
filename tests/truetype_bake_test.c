#include "ui/truetype_bake.h"
#include "ui/ui_font_internal.h"

#include "test_runtime.h"
#include "truetype_fixture.h"

#include <string.h>

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

/* Глиф A — прямоугольник с левым краем в половине пикселя относительно пера. */
static void BuildRectangleFont(TrueTypeFixture *fixture)
{
    TrueTypeFixtureBuild(fixture, false, true);
    uint8_t *glyph = fixture->bytes + fixture->glyfOffset;
    memmove(glyph + 34u, glyph + 12u, fixture->size - fixture->glyfOffset - 12u);
    fixture->size += 22u;
    fixture->hmtxOffset += 22u;
    TrueTypeFixtureTable(fixture->bytes, 1u, "glyf", fixture->glyfOffset, 58u);
    TrueTypeFixtureTable(fixture->bytes, 4u, "hmtx", fixture->hmtxOffset, 14u);
    static const uint32_t offsets[] = {0u, 0u, 34u, 46u, 46u, 58u};
    for (size_t index = 0u; index < sizeof(offsets) / sizeof(offsets[0]); ++index)
        TrueTypeFixturePut32(fixture->bytes + fixture->locaOffset + index * 4u, offsets[index]);

    static const uint16_t rectangle[] = {1u,      128u, 0u,   512u, 640u,           3u, 0u, 0x0101u,
                                         0x0101u, 128u, 384u, 0u,   (uint16_t)-384, 0u, 0u, 640u,
                                         0u};
    for (size_t index = 0u; index < sizeof(rectangle) / sizeof(rectangle[0]); ++index)
        TrueTypeFixturePut16(glyph + index * 2u, rectangle[index]);
    TrueTypeFixturePut16(fixture->bytes + TRUE_TYPE_FIXTURE_HEAD + 18u, 1024u);
    TrueTypeFixturePut16(fixture->bytes + fixture->hmtxOffset + 4u, 256u);
    TrueTypeFixturePut16(fixture->bytes + fixture->hmtxOffset + 6u, 96u);
    TrueTypeFixtureAddVertical(fixture, 0x00010000u, 2u);
}

LAIUE_TEST_ENTRY(TrueTypeBakeTestEntryPoint)
{
    TrueTypeFixture fixture;
    BuildRectangleFont(&fixture);
    UiFont font = {0};
    const TrueTypeAtlasLimits limits = {32u, 16u, 512u};
    Expect(UiFontBakeFromSfnt(&font, 16, fixture.bytes, fixture.size, limits, 1000u),
           "rectangle font bakes");
    Expect(font.glyphCount == UiGlyphCount() && font.atlasWidth == 32u && font.atlasHeight == 12u &&
               font.ascent == 12.5f && font.lineHeight == 17.1875f,
           "atlas and line metrics");
    const UiGlyph *a = UiFontFindGlyph(&font, 'A');
    Expect(a != NULL && a->codepoint == 'A' && a->offsetX == 1 && a->offsetY == 10 &&
               a->advance == 4.0f && a->width == 7u && a->height == 10u && a->u0 == 0.03125f &&
               a->u1 == 0.25f && a->v0 > 0.0f && a->v1 < 1.0f,
           "UiGlyph uses the pen-relative integer box and scaled advance");
    const UiGlyph *space = UiFontFindGlyph(&font, ' ');
    Expect(space != NULL && space->width == 0u && space->height == 0u && space->advance == 6.25f,
           "an empty glyph keeps its advance");
    for (uint32_t y = 0u; y < font.atlasHeight; ++y)
        for (uint32_t x = 0u; x < font.atlasWidth; ++x)
        {
            const uint8_t expected =
                y == 0u || y == 11u || x == 0u || x > 7u ? 0u : (x == 1u || x == 7u ? 128u : 255u);
            Expect(font.atlas[(size_t)y * font.atlasWidth + x] == expected,
                   "fractional ink and every padding pixel are exact");
        }

    uint8_t *originalAtlas = font.atlas;
    UiGlyph *originalGlyphs = font.glyphs;
    Expect(!UiFontBakeFromSfnt(&font, 16, fixture.bytes, 4u, limits, 1000u),
           "invalid SFNT is rejected");
    Expect(!UiFontBakeFromSfnt(&font, 16, fixture.bytes, fixture.size, limits, 1u),
           "edge work budget is checked before painting");
    Expect(!UiFontBakeFromSfnt(&font, 32, fixture.bytes, fixture.size, limits, 1000u) &&
               font.atlas == originalAtlas && font.glyphs == originalGlyphs &&
               font.pixelSize == 16 && font.atlas[33] == 128u && a->advance == 4.0f,
           "failed replacement preserves the successful font");

    const TrueTypeAtlasLimits bigger = {64u, 32u, 2048u};
    Expect(UiFontBakeFromSfnt(&font, 32, fixture.bytes, fixture.size, bigger, 1000u) &&
               font.pixelSize == 32 && font.atlasWidth == 64u && font.atlasHeight == 22u,
           "successful replacement publishes new glyphs and atlas together");
    UiFontRelease(&font);
    Expect(font.atlas == NULL && font.glyphs == NULL, "the existing release owns both allocations");
    LAIUE_TEST_SUCCESS();
}
