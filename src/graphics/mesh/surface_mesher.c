#include "mesh/surface_mesher.h"

#include "math/scalar.h"

#include <stdbool.h>
#include <stddef.h>

#define NO_VERTEX UINT32_MAX

typedef struct Grid
{
    uint32_t count[3];
    size_t sampleStride[3];
    size_t cellStride[3];
    const float *values;
    const uint8_t *materials;
} Grid;

/* Corner c of a cell sits at offset ((c >> 0) & 1, (c >> 1) & 1, (c >> 2) & 1). */
static const uint8_t cellEdges[12][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static size_t SampleIndex(const Grid *grid, const uint32_t point[3])
{
    return point[0] * grid->sampleStride[0] + point[1] * grid->sampleStride[1] +
           point[2] * grid->sampleStride[2];
}

static bool Inside(float value)
{
    return value > 0.0f;
}

/* The vertex of a cell, created on first use: the mean of the points where the
 * surface crosses the cell's edges, with the normal from the field gradient. */
static uint32_t CellVertex(LaiueSurfaceMeshRequestV1 *request, const Grid *grid,
                           const uint32_t cell[3], bool *outOfCapacity)
{
    const size_t cellIndex = cell[0] * grid->cellStride[0] + cell[1] * grid->cellStride[1] +
                             cell[2] * grid->cellStride[2];
    if (request->cellScratch[cellIndex] != NO_VERTEX)
    {
        return request->cellScratch[cellIndex];
    }
    if (request->vertexCount == request->vertexCapacity)
    {
        *outOfCapacity = true;
        return NO_VERTEX;
    }

    float corners[8];
    uint32_t material = 0u;
    float mostSolid = 0.0f;
    for (uint32_t corner = 0u; corner < 8u; ++corner)
    {
        const uint32_t point[3] = {cell[0] + (corner & 1u), cell[1] + ((corner >> 1) & 1u),
                                   cell[2] + ((corner >> 2) & 1u)};
        const size_t sample = SampleIndex(grid, point);
        corners[corner] = grid->values[sample];
        if (grid->materials != NULL && corners[corner] > mostSolid)
        {
            mostSolid = corners[corner];
            material = grid->materials[sample];
        }
    }

    float sum[3] = {0.0f, 0.0f, 0.0f};
    uint32_t crossings = 0u;
    for (uint32_t edge = 0u; edge < 12u; ++edge)
    {
        const uint32_t from = cellEdges[edge][0];
        const uint32_t to = cellEdges[edge][1];
        const float a = corners[from];
        const float b = corners[to];
        if (Inside(a) == Inside(b))
        {
            continue;
        }
        float t = a / (a - b);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const float start = (float)((from >> axis) & 1u);
            const float end = (float)((to >> axis) & 1u);
            sum[axis] += start + (end - start) * t;
        }
        ++crossings;
    }

    /* Trilinear gradient at the cell centre: the mean difference across each
     * axis. Matter is positive, so the outward normal points down the slope. */
    const float gradient[3] = {
        (corners[1] - corners[0]) + (corners[3] - corners[2]) + (corners[5] - corners[4]) +
            (corners[7] - corners[6]),
        (corners[2] - corners[0]) + (corners[3] - corners[1]) + (corners[6] - corners[4]) +
            (corners[7] - corners[5]),
        (corners[4] - corners[0]) + (corners[5] - corners[1]) + (corners[6] - corners[2]) +
            (corners[7] - corners[3]),
    };
    const float length = ScalarSqrt(gradient[0] * gradient[0] + gradient[1] * gradient[1] +
                                    gradient[2] * gradient[2]);

    LaiueSurfaceVertexV1 *vertex = &request->vertices[request->vertexCount];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const float offset = crossings == 0u ? 0.5f : sum[axis] / (float)crossings;
        vertex->position[axis] = (float)cell[axis] + offset;
        vertex->normal[axis] =
            length > 0.0f ? -gradient[axis] / length : (axis == 2u ? 1.0f : 0.0f);
    }
    vertex->material = material;
    request->cellScratch[cellIndex] = request->vertexCount;
    return request->vertexCount++;
}

static bool RequestIsValid(const LaiueSurfaceMeshRequestV1 *request, uint32_t emitMin[3],
                           uint32_t emitMax[3])
{
    if (request == NULL || request->structSize < sizeof(*request) || request->values == NULL ||
        request->cellScratch == NULL ||
        (request->vertexCapacity != 0u && request->vertices == NULL) ||
        (request->indexCapacity != 0u && request->indices == NULL))
    {
        return false;
    }
    const uint32_t counts[3] = {request->countX, request->countY, request->countZ};
    uint64_t samples = 1u;
    uint64_t cells = 1u;
    bool emitAll = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (counts[axis] < 2u || counts[axis] > LAIUE_SURFACE_MESH_MAX_SAMPLES_PER_AXIS)
        {
            return false;
        }
        samples *= counts[axis];
        cells *= counts[axis] - 1u;
        emitAll = emitAll && request->emitMin[axis] == 0u && request->emitMax[axis] == 0u;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        emitMin[axis] = emitAll ? 0u : request->emitMin[axis];
        emitMax[axis] = emitAll ? counts[axis] : request->emitMax[axis];
        if (emitMin[axis] > emitMax[axis] || emitMax[axis] > counts[axis])
        {
            return false;
        }
    }
    return samples <= LAIUE_SURFACE_MESH_MAX_SAMPLES && request->cellScratchCapacity >= cells;
}

uint32_t LaiueSurfaceMeshBuildV1(LaiueSurfaceMeshRequestV1 *request)
{
    uint32_t emitMin[3];
    uint32_t emitMax[3];
    if (!RequestIsValid(request, emitMin, emitMax))
    {
        if (request != NULL)
        {
            request->vertexCount = 0u;
            request->indexCount = 0u;
        }
        return LAIUE_SURFACE_MESH_INVALID;
    }
    request->vertexCount = 0u;
    request->indexCount = 0u;

    const Grid grid = {
        .count = {request->countX, request->countY, request->countZ},
        .sampleStride = {1u, request->countX, (size_t)request->countX * request->countY},
        .cellStride = {1u, request->countX - 1u,
                       (size_t)(request->countX - 1u) * (request->countY - 1u)},
        .values = request->values,
        .materials = request->materials,
    };
    const size_t cells = (size_t)(grid.count[0] - 1u) * (grid.count[1] - 1u) * (grid.count[2] - 1u);
    for (size_t cell = 0u; cell < cells; ++cell)
    {
        request->cellScratch[cell] = NO_VERTEX;
    }

    uint32_t point[3];
    for (point[2] = emitMin[2]; point[2] < emitMax[2]; ++point[2])
    {
        for (point[1] = emitMin[1]; point[1] < emitMax[1]; ++point[1])
        {
            for (point[0] = emitMin[0]; point[0] < emitMax[0]; ++point[0])
            {
                const bool startInside = Inside(grid.values[SampleIndex(&grid, point)]);
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                {
                    const uint32_t first = (axis + 1u) % 3u;
                    const uint32_t second = (axis + 2u) % 3u;
                    /* The edge needs its end sample and all four cells around
                     * it: both neighbours across each perpendicular axis. */
                    if (point[axis] + 1u >= grid.count[axis] || point[first] == 0u ||
                        point[second] == 0u || point[first] + 1u >= grid.count[first] ||
                        point[second] + 1u >= grid.count[second])
                    {
                        continue;
                    }
                    uint32_t end[3] = {point[0], point[1], point[2]};
                    ++end[axis];
                    if (startInside == Inside(grid.values[SampleIndex(&grid, end)]))
                    {
                        continue;
                    }

                    uint32_t corner[4][3];
                    for (uint32_t index = 0u; index < 4u; ++index)
                    {
                        corner[index][0] = point[0];
                        corner[index][1] = point[1];
                        corner[index][2] = point[2];
                    }
                    /* Around the edge: (-,-), (+,-), (+,+), (-,+) in the plane of
                     * the two other axes, counter-clockwise seen along the edge. */
                    --corner[0][first];
                    --corner[0][second];
                    --corner[1][second];
                    --corner[3][first];
                    bool outOfCapacity = false;
                    uint32_t vertices[4];
                    for (uint32_t index = 0u; index < 4u; ++index)
                    {
                        vertices[index] = CellVertex(request, &grid, corner[index], &outOfCapacity);
                    }
                    if (outOfCapacity || request->indexCapacity - request->indexCount < 6u)
                    {
                        return LAIUE_SURFACE_MESH_CAPACITY;
                    }
                    /* Matter at the start faces the quad toward +axis. */
                    const uint32_t order[2][6] = {{0u, 1u, 2u, 0u, 2u, 3u},
                                                  {0u, 2u, 1u, 0u, 3u, 2u}};
                    const uint32_t *triangles = order[startInside ? 0 : 1];
                    for (uint32_t index = 0u; index < 6u; ++index)
                    {
                        request->indices[request->indexCount++] = vertices[triangles[index]];
                    }
                }
            }
        }
    }
    return LAIUE_SURFACE_MESH_OK;
}
