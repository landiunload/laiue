#include "mesh/surface_mesher.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COUNT 24u
#define SAMPLES (COUNT * COUNT * COUNT)
#define CELLS ((COUNT - 1u) * (COUNT - 1u) * (COUNT - 1u))
#define CENTER 11.5f
#define RADIUS 8.0f
#define VERTEX_CAPACITY 8192u
#define INDEX_CAPACITY 49152u

static float values[SAMPLES];
static uint8_t materials[SAMPLES];
static uint32_t scratch[CELLS];
static LaiueSurfaceVertexV1 vertices[VERTEX_CAPACITY];
static uint32_t indices[INDEX_CAPACITY];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static LaiueSurfaceMeshRequestV1 Request(void)
{
    return (LaiueSurfaceMeshRequestV1){
        .structSize = sizeof(LaiueSurfaceMeshRequestV1),
        .countX = COUNT,
        .countY = COUNT,
        .countZ = COUNT,
        .values = values,
        .materials = materials,
        .cellScratch = scratch,
        .cellScratchCapacity = CELLS,
        .vertices = vertices,
        .vertexCapacity = VERTEX_CAPACITY,
        .indices = indices,
        .indexCapacity = INDEX_CAPACITY,
    };
}

/* Positive inside a ball: the sign is all the mesher needs. */
static void FillBall(void)
{
    for (uint32_t z = 0u; z < COUNT; ++z)
        for (uint32_t y = 0u; y < COUNT; ++y)
            for (uint32_t x = 0u; x < COUNT; ++x)
            {
                const float dx = (float)x - CENTER;
                const float dy = (float)y - CENTER;
                const float dz = (float)z - CENTER;
                const size_t index = x + (size_t)y * COUNT + (size_t)z * COUNT * COUNT;
                values[index] = RADIUS * RADIUS - (dx * dx + dy * dy + dz * dz);
                materials[index] = (uint8_t)(z < 12u ? 3u : 5u);
            }
}

static void TestBall(void)
{
    FillBall();
    LaiueSurfaceMeshRequestV1 request = Request();
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_OK, "a ball meshes");
    Expect(request.vertexCount > 100u && request.indexCount > 0u && request.indexCount % 3u == 0u,
           "the ball has vertices and whole triangles");

    for (uint32_t index = 0u; index < request.vertexCount; ++index)
    {
        const LaiueSurfaceVertexV1 *vertex = &vertices[index];
        const float offset[3] = {vertex->position[0] - CENTER, vertex->position[1] - CENTER,
                                 vertex->position[2] - CENTER};
        const float squared = offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2];
        Expect(squared > (RADIUS - 1.0f) * (RADIUS - 1.0f) &&
                   squared < (RADIUS + 1.0f) * (RADIUS + 1.0f),
               "vertices lie on the ball surface");
        const float normalLength = vertex->normal[0] * vertex->normal[0] +
                                   vertex->normal[1] * vertex->normal[1] +
                                   vertex->normal[2] * vertex->normal[2];
        Expect(normalLength > 0.99f && normalLength < 1.01f, "normals have unit length");
        Expect(vertex->normal[0] * offset[0] + vertex->normal[1] * offset[1] +
                       vertex->normal[2] * offset[2] >
                   0.0f,
               "normals point out of the ball");
        Expect(vertex->material == 3u || vertex->material == 5u, "vertices carry a material");
    }

    for (uint32_t triangle = 0u; triangle < request.indexCount; triangle += 3u)
    {
        const float *a = vertices[indices[triangle]].position;
        const float *b = vertices[indices[triangle + 1u]].position;
        const float *c = vertices[indices[triangle + 2u]].position;
        const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float normal[3] = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                                 ab[0] * ac[1] - ab[1] * ac[0]};
        const float centroid[3] = {(a[0] + b[0] + c[0]) / 3.0f - CENTER,
                                   (a[1] + b[1] + c[1]) / 3.0f - CENTER,
                                   (a[2] + b[2] + c[2]) / 3.0f - CENTER};
        Expect(normal[0] * centroid[0] + normal[1] * centroid[1] + normal[2] * centroid[2] >= 0.0f,
               "triangles wind counter-clockwise seen from outside");
    }

    /* A closed, consistently wound surface uses every directed edge once and
     * its reverse once. */
    for (uint32_t edge = 0u; edge < request.indexCount; ++edge)
    {
        const uint32_t base = edge - edge % 3u;
        const uint32_t from = indices[edge];
        const uint32_t to = indices[base + (edge - base + 1u) % 3u];
        uint32_t forward = 0u;
        uint32_t backward = 0u;
        for (uint32_t other = 0u; other < request.indexCount; ++other)
        {
            const uint32_t otherBase = other - other % 3u;
            const uint32_t otherFrom = indices[other];
            const uint32_t otherTo = indices[otherBase + (other - otherBase + 1u) % 3u];
            forward += otherFrom == from && otherTo == to;
            backward += otherFrom == to && otherTo == from;
        }
        Expect(forward == 1u && backward == 1u, "the ball surface is closed and consistent");
    }
}

/* Two halves meshed with disjoint emit ranges produce exactly the faces of
 * the whole: no gap and no duplicate at the seam. */
static void TestEmitRanges(void)
{
    FillBall();
    LaiueSurfaceMeshRequestV1 whole = Request();
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        whole.emitMin[axis] = 1u;
        whole.emitMax[axis] = COUNT - 1u;
    }
    Expect(LaiueSurfaceMeshBuildV1(&whole) == LAIUE_SURFACE_MESH_OK, "the owned range meshes");
    const uint32_t wholeIndices = whole.indexCount;

    uint32_t splitIndices = 0u;
    const uint32_t splits[3] = {1u, 12u, COUNT - 1u};
    for (uint32_t part = 0u; part < 2u; ++part)
    {
        LaiueSurfaceMeshRequestV1 half = whole;
        half.emitMin[0] = splits[part];
        half.emitMax[0] = splits[part + 1u];
        Expect(LaiueSurfaceMeshBuildV1(&half) == LAIUE_SURFACE_MESH_OK, "a half meshes");
        splitIndices += half.indexCount;
    }
    Expect(splitIndices == wholeIndices && wholeIndices > 0u,
           "halves split the surface without gaps or duplicates");
}

/* Ground below z = 10.3 with a thin top layer: the surface shows the layer
 * right beneath it, however deep the rest of its cells reach. */
static void TestSurfaceMaterial(void)
{
    for (uint32_t z = 0u; z < COUNT; ++z)
        for (uint32_t y = 0u; y < COUNT; ++y)
            for (uint32_t x = 0u; x < COUNT; ++x)
            {
                const size_t index = x + (size_t)y * COUNT + (size_t)z * COUNT * COUNT;
                values[index] = 10.3f - (float)z;
                materials[index] = (uint8_t)(values[index] < 1.0f ? 7u : 9u);
            }
    LaiueSurfaceMeshRequestV1 request = Request();
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_OK && request.vertexCount > 0u,
           "a flat ground meshes");
    for (uint32_t index = 0u; index < request.vertexCount; ++index)
    {
        Expect(vertices[index].material == 7u, "the surface shows its top layer");
        Expect(vertices[index].normal[2] > 0.99f, "flat ground faces up");
    }
}

static void TestLimits(void)
{
    FillBall();
    LaiueSurfaceMeshRequestV1 request = Request();
    request.vertexCapacity = 4u;
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_CAPACITY &&
               request.vertexCount <= 4u,
           "a short vertex buffer reports capacity");
    request = Request();
    request.indexCapacity = 5u;
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_CAPACITY &&
               request.indexCount == 0u,
           "a short index buffer reports capacity");

    request = Request();
    request.countX = 1u;
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_INVALID,
           "a single sample per axis is rejected");
    request = Request();
    request.cellScratchCapacity = CELLS - 1u;
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_INVALID,
           "a short scratch is rejected");
    request = Request();
    request.emitMax[0] = COUNT + 1u;
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_INVALID,
           "an emit range past the samples is rejected");
    request = Request();
    request.emitMin[1] = 5u;
    request.emitMax[1] = 4u;
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_INVALID,
           "a reversed emit range is rejected");
    request = Request();
    request.structSize = 4u;
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_INVALID &&
               LaiueSurfaceMeshBuildV1(NULL) == LAIUE_SURFACE_MESH_INVALID,
           "an old or missing request is rejected");

    for (uint32_t index = 0u; index < SAMPLES; ++index)
        values[index] = -1.0f;
    request = Request();
    Expect(LaiueSurfaceMeshBuildV1(&request) == LAIUE_SURFACE_MESH_OK &&
               request.vertexCount == 0u && request.indexCount == 0u,
           "empty space has no surface");
}

LAIUE_TEST_ENTRY(SurfaceMesherTestEntryPoint)
{
    TestBall();
    TestEmitRanges();
    TestSurfaceMaterial();
    TestLimits();
    LAIUE_TEST_SUCCESS();
}
