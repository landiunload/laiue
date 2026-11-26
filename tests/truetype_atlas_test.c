#include "ui/truetype_atlas.h"
#include "test_runtime.h"

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

LAIUE_TEST_ENTRY(TrueTypeAtlasTestEntryPoint)
{
    static const TrueTypeAtlasGlyphSize sizes[] = {{3u, 2u}, {2u, 4u}, {0u, 100u}, {4u, 1u}};
    TrueTypeAtlasPlacement placements[5] = {{0u, 0u}, {0u, 0u}, {0u, 0u}, {0u, 0u}, {45u, 56u}};
    TrueTypeAtlasSummary summary = {0};
    TrueTypeAtlasLimits limits = {8u, 8u, 64u};
    Expect(TrueTypeAtlasPlan(sizes, 4u, limits, placements, 5u, &summary), "exact fit");
    Expect(summary.width == 8u && summary.height == 8u && summary.bytes == 64u &&
               summary.glyphCount == 4u,
           "atlas bounds preserve a final row and column of padding");
    Expect(placements[0].x == 1u && placements[0].y == 1u && placements[1].x == 5u &&
               placements[1].y == 1u && placements[2].x == 0u && placements[2].y == 0u &&
               placements[3].x == 1u && placements[3].y == 6u && placements[4].x == 45u &&
               placements[4].y == 56u,
           "the tallest glyph sets the next shelf and the scratch tail is unchanged");

    --limits.byteBudget;
    Expect(!TrueTypeAtlasPlan(sizes, 4u, limits, placements, 4u, &summary) &&
               summary.bytes == 64u && summary.height == 8u && summary.glyphCount == 4u,
           "one short byte rejects the atlas without publishing a new summary");
    limits = (TrueTypeAtlasLimits){4u, 8u, 64u};
    Expect(!TrueTypeAtlasPlan(sizes, 4u, limits, placements, 4u, &summary),
           "a wide glyph cannot overrun the row");
    limits = (TrueTypeAtlasLimits){8u, 8u, 64u};
    Expect(!TrueTypeAtlasPlan(sizes, 4u, limits, placements, 3u, &summary) &&
               !TrueTypeAtlasPlan(sizes, UINT32_MAX, limits, placements, UINT32_MAX, &summary),
           "invalid counts are rejected before reading any glyphs");
    Expect(TrueTypeAtlasPlan(NULL, 0u, limits, NULL, 0u, &summary) && summary.height == 2u &&
               summary.bytes == 16u,
           "a glyph-free atlas contains only padding");
    limits = (TrueTypeAtlasLimits){UINT32_MAX, UINT32_MAX, 16u};
    Expect(!TrueTypeAtlasPlan(NULL, 0u, limits, NULL, 0u, &summary) && summary.bytes == 16u,
           "limits reject the byte product before multiplying");
    LAIUE_TEST_SUCCESS();
}
