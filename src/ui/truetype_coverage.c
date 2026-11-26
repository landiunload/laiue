#include "truetype_coverage.h"

enum
{
    COVERAGE_SAMPLES = 8,
};

static bool EdgeCoordinatesValid(const TrueTypeEdge *edges, uint32_t edgeCount)
{
    const float limit = (float)TRUE_TYPE_XY_MAX_MAGNITUDE;
    for (uint32_t index = 0u; index < edgeCount; ++index)
    {
        const TrueTypeEdge edge = edges[index];
        if (!(edge.x0 >= -limit && edge.x0 <= limit && edge.y0 >= -limit && edge.y0 <= limit &&
              edge.x1 >= -limit && edge.x1 <= limit && edge.y1 >= -limit && edge.y1 <= limit))
            return false;
    }
    return true;
}

static bool CollectEvents(const TrueTypeEdge *edges, uint32_t edgeCount, double sampleY,
                          double left, TrueTypeCoverageStorage storage, uint32_t *outCount)
{
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < edgeCount; ++index)
    {
        const TrueTypeEdge edge = edges[index];
        const bool ascending = edge.y0 < edge.y1;
        const double lowerY = ascending ? (double)edge.y0 : (double)edge.y1;
        const double upperY = ascending ? (double)edge.y1 : (double)edge.y0;
        if (sampleY < lowerY || sampleY >= upperY)
            continue;
        if (count == storage.eventCapacity)
            return false;

        // Одна формула для обоих направлений даёт одинаковый X у совпавших рёбер.
        const double lowerX = ascending ? (double)edge.x0 : (double)edge.x1;
        const double upperX = ascending ? (double)edge.x1 : (double)edge.x0;
        const double amount = (sampleY - lowerY) / (upperY - lowerY);
        const double x = (lowerX - left) + (upperX - lowerX) * amount;
        storage.events[count++] = (TrueTypeCoverageEvent){x, ascending ? 1 : -1};
    }
    *outCount = count;
    return true;
}

static void SinkEvent(TrueTypeCoverageEvent *events, uint32_t index, uint32_t count)
{
    const TrueTypeCoverageEvent event = events[index];
    while (index < count / 2u)
    {
        uint32_t child = index * 2u + 1u;
        if (child + 1u < count && events[child].x < events[child + 1u].x)
            ++child;
        if (event.x >= events[child].x)
            break;
        events[index] = events[child];
        index = child;
    }
    events[index] = event;
}

// Итеративная heapsort ограничивает худший случай O(n log n) без qsort.
static void SortEvents(TrueTypeCoverageEvent *events, uint32_t count)
{
    for (uint32_t start = count / 2u; start != 0u; --start)
        SinkEvent(events, start - 1u, count);
    while (count > 1u)
    {
        const TrueTypeCoverageEvent largest = events[0];
        --count;
        events[0] = events[count];
        SinkEvent(events, 0u, count);
        events[count] = largest;
    }
}

static void AddSpan(double *coverage, uint32_t width, double left, double right)
{
    if (left < 0.0)
        left = 0.0;
    if (right > (double)width)
        right = (double)width;
    if (left >= right)
        return;

    // После отсечения 0 <= left < right <= width. Целые пиксели
    // между концами получают единицу, пограничные — длину части.
    uint32_t first = (uint32_t)left;
    const uint32_t last = (uint32_t)right;
    if (first == last)
    {
        coverage[first] += right - left;
        return;
    }
    coverage[first] += (double)(first + 1u) - left;
    for (++first; first < last; ++first)
        coverage[first] += 1.0;
    if (last < width)
        coverage[last] += right - (double)last;
}

static bool FillSample(TrueTypeCoverageStorage storage, uint32_t count, uint32_t width)
{
    SortEvents(storage.events, count);
    int32_t winding = 0;
    double start = 0.0;
    uint32_t index = 0u;
    while (index < count)
    {
        const double x = storage.events[index].x;
        const int32_t previous = winding;
        do
        {
            winding += storage.events[index++].direction;
        } while (index < count && storage.events[index].x == x);
        if (previous == 0 && winding != 0)
            start = x;
        else if (previous != 0 && winding == 0)
            AddSpan(storage.coverage, width, start, x);
    }
    return winding == 0;
}

bool TrueTypeCoverageRow(const TrueTypeEdge *edges, uint32_t edgeCount, double left, double top,
                         uint32_t width, TrueTypeCoverageStorage storage, uint32_t *outRowBytes)
{
    const double limit = (double)TRUE_TYPE_XY_MAX_MAGNITUDE + (double)UINT16_MAX;
    if (outRowBytes == NULL || edgeCount > TRUE_TYPE_EDGE_MAX_COUNT || width > UINT16_MAX ||
        !(left >= -limit && left <= limit && top >= -limit && top <= limit) ||
        (edgeCount != 0u && edges == NULL))
        return false;
    if (width == 0u)
    {
        *outRowBytes = 0u;
        return true;
    }
    if (storage.coverage == NULL || storage.coverageCapacity < width || storage.row == NULL ||
        storage.rowCapacity < width || (storage.eventCapacity != 0u && storage.events == NULL) ||
        !EdgeCoordinatesValid(edges, edgeCount))
        return false;

    for (uint32_t index = 0u; index < width; ++index)
        storage.coverage[index] = 0.0;
    for (uint32_t sample = 0u; sample < COVERAGE_SAMPLES; ++sample)
    {
        const double sampleY = top - ((double)sample + 0.5) / (double)COVERAGE_SAMPLES;
        uint32_t count;
        if (!CollectEvents(edges, edgeCount, sampleY, left, storage, &count) ||
            !FillSample(storage, count, width))
            return false;
    }
    for (uint32_t index = 0u; index < width; ++index)
    {
        const double covered = storage.coverage[index];
        const double scaled = covered >= (double)COVERAGE_SAMPLES
                                  ? 255.0
                                  : covered * (255.0 / (double)COVERAGE_SAMPLES);
        const uint32_t whole = (uint32_t)scaled;
        storage.row[index] = (uint8_t)(whole + (uint32_t)(scaled - (double)whole >= 0.5));
    }
    *outRowBytes = width;
    return true;
}
