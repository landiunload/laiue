#include "truetype_bake.h"

#include "platform/system.h"

typedef struct TrueTypeBakeWorkspace
{
    TrueTypeBakeGeometry geometry;
    TrueTypeBakePlan plan;
} TrueTypeBakeWorkspace;

bool UiFontBakeFromSfnt(UiFont *font, int32_t pixelSize, const uint8_t *bytes, size_t size,
                        TrueTypeAtlasLimits limits, uint64_t edgeRowBudget)
{
    TrueTypeFont source;
    if (font == NULL || !TrueTypeReaderInitialize(bytes, size, &source))
        return false;

    TrueTypeBakeWorkspace *work = PlatformAllocate(sizeof(*work), false);
    if (work == NULL)
        return false;
    if (!TrueTypeBakePrepare(&source, pixelSize, limits, edgeRowBudget, &work->geometry,
                             &work->plan))
    {
        PlatformFree(work);
        return false;
    }

    const TrueTypeBakePlan *plan = &work->plan;
    UiFont next = {
        .atlas = PlatformAllocate(plan->atlas.bytes, false),
        .atlasWidth = plan->atlas.width,
        .atlasHeight = plan->atlas.height,
        .glyphs = PlatformAllocate((size_t)plan->atlas.glyphCount * sizeof(UiGlyph), false),
        .glyphCount = plan->atlas.glyphCount,
        .ascent = plan->ascent,
        .lineHeight = plan->lineHeight,
        .pixelSize = pixelSize,
    };
    const size_t eventBytes = (size_t)plan->maxEdges * sizeof(TrueTypeCoverageEvent);
    const size_t rowBytes = (size_t)plan->maxWidth * sizeof(double);
    uint8_t *storage =
        eventBytes + rowBytes != 0u ? PlatformAllocate(eventBytes + rowBytes, false) : NULL;
    const TrueTypeCoverageStorage scratch = {
        .events = (TrueTypeCoverageEvent *)(void *)storage,
        .eventCapacity = plan->maxEdges,
        .coverage = storage != NULL ? (double *)(void *)(storage + eventBytes) : NULL,
        .coverageCapacity = plan->maxWidth,
    };
    const bool baked = next.atlas != NULL && next.glyphs != NULL &&
                       TrueTypeBakePaint(&source, plan, &work->geometry, scratch, next.atlas,
                                         plan->atlas.bytes, next.glyphs, next.glyphCount);
    PlatformFree(storage);
    PlatformFree(work);
    if (!baked)
    {
        PlatformFree(next.atlas);
        PlatformFree(next.glyphs);
        return false;
    }

    UiFontRelease(font);
    *font = next;
    return true;
}
