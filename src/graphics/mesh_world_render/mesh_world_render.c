#include "mesh_world_render/mesh_world_render_service.h"

#include "math/scalar.h"
#include "platform/system.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(LaiueMeshRenderVertexV1) == 36u,
               "render vertex must match the model pack vertex layout");

#define RENDER_DEFAULT_MAXIMUM_BUFFERS 1024u
#define RENDER_MAXIMUM_BUFFERS_LIMIT 65535u
#define RENDER_MAX_MODEL_VERTICES (1u << 22)
#define RENDER_MAX_MODEL_INDICES (3u << 22)
#define RENDER_MAX_CELL_VERTICES (1u << 24)
#define RENDER_MAX_QUERY_RADIUS 65536.0f

typedef struct RenderModel
{
    uint32_t model;
    LaiueMeshRenderVertexV1 *vertices;
    uint32_t vertexCount;
    uint32_t *indices;
    uint32_t indexCount;
    LaiueMeshRenderPartV1 *parts;
    uint32_t partCount;
} RenderModel;

typedef struct RenderMaterial
{
    uint32_t material;
    LaiueGraphicsHandle texture;
    LaiueGraphicsHandle sampler;
} RenderMaterial;

typedef struct RenderBatch
{
    uint32_t material;
    uint32_t vertexCount;
    LaiueGraphicsHandle buffer;
} RenderBatch;

/* Cells are kept sorted by coordinate, the order queryCells answers in, so
 * one merge pass matches the cache against the cells in range. */
typedef struct RenderCell
{
    LaiueMeshCellV1 coord;
    uint64_t revision;
    uint32_t generation;
    uint32_t built;
    RenderBatch *batches;
    uint32_t batchCount;
    float boundsMin[3];
    float boundsMax[3];
} RenderCell;

typedef struct RenderAccumulator
{
    uint32_t material;
    LaiueGraphicsVertexV2 *vertices;
    uint32_t count;
    uint32_t capacity;
} RenderAccumulator;

typedef struct RenderCandidate
{
    uint32_t cell;
    double distanceSquared;
} RenderCandidate;

struct LaiueMeshWorldRendererV1
{
    const LaiueMeshWorldServiceV1 *worldService;
    LaiueMeshWorldV1 *world;
    LaiueGraphicsDeviceV2 *device;
    uint32_t maximumBuffers;
    float cellSize;

    RenderModel *models; /* sorted by model */
    uint32_t modelCount;
    uint32_t modelCapacity;
    RenderMaterial *materials; /* sorted by material */
    uint32_t materialCount;
    uint32_t materialCapacity;
    RenderCell *cells;
    uint32_t cellCount;
    uint32_t cellCapacity;

    /* Bumped by anything that changes baked geometry without changing the
     * world: a model, the lighting. */
    uint32_t generation;
    float toSun[3];
    float sunColor[3];
    float ambientColor[3];

    uint32_t bufferCount;
    uint64_t vertexCount;
    LaiueMeshRenderStatsV1 stats;

    LaiueMeshCellV1 *query;
    uint32_t queryCapacity;
    LaiueMeshInstanceInfoV1 *instances;
    uint32_t instanceCapacity;
    RenderAccumulator *accumulators;
    uint32_t accumulatorCount;
    uint32_t accumulatorCapacity;
    RenderCandidate *candidates;
    uint32_t candidateCapacity;
};

/* ---- helpers ------------------------------------------------------------ */

static bool FiniteFloat(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT32_C(0x7F800000)) != UINT32_C(0x7F800000);
}

static bool CellEqual(const LaiueMeshCellV1 *left, const LaiueMeshCellV1 *right)
{
    return left->x == right->x && left->y == right->y && left->z == right->z;
}

static int CompareCells(const LaiueMeshCellV1 *left, const LaiueMeshCellV1 *right)
{
    if (left->x != right->x)
        return left->x < right->x ? -1 : 1;
    if (left->y != right->y)
        return left->y < right->y ? -1 : 1;
    if (left->z != right->z)
        return left->z < right->z ? -1 : 1;
    return 0;
}

/* Returns array grown to hold required elements, or NULL leaving it as it
 * was.  New elements are not zeroed. */
static void *GrowArray(void *array, uint32_t *capacity, uint32_t required, size_t elementSize)
{
    if (required <= *capacity)
        return array;
    uint64_t next = *capacity == 0u ? 8u : (uint64_t)*capacity * 2u;
    while (next < required)
        next *= 2u;
    if (next > UINT32_MAX / 2u || next * elementSize > (uint64_t)SIZE_MAX / 2u)
        return NULL;
    void *grown = PlatformReallocate(array, elementSize * (size_t)next, false);
    if (grown != NULL)
        *capacity = (uint32_t)next;
    return grown;
}

static uint32_t FindModelIndex(const LaiueMeshWorldRendererV1 *renderer, uint32_t model,
                               bool *outFound)
{
    uint32_t low = 0u;
    uint32_t high = renderer->modelCount;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2u;
        if (renderer->models[middle].model < model)
            low = middle + 1u;
        else
            high = middle;
    }
    *outFound = low < renderer->modelCount && renderer->models[low].model == model;
    return low;
}

static uint32_t FindMaterialIndex(const LaiueMeshWorldRendererV1 *renderer, uint32_t material,
                                  bool *outFound)
{
    uint32_t low = 0u;
    uint32_t high = renderer->materialCount;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2u;
        if (renderer->materials[middle].material < material)
            low = middle + 1u;
        else
            high = middle;
    }
    *outFound = low < renderer->materialCount && renderer->materials[low].material == material;
    return low;
}

static void ReleaseBatches(LaiueMeshWorldRendererV1 *renderer, RenderCell *cell)
{
    for (uint32_t i = 0u; i < cell->batchCount; ++i)
    {
        renderer->device->destroyHandle(renderer->device, cell->batches[i].buffer);
        --renderer->bufferCount;
        renderer->vertexCount -= cell->batches[i].vertexCount;
    }
    PlatformFree(cell->batches);
    cell->batches = NULL;
    cell->batchCount = 0u;
}

static void QuaternionMatrix(const float q[4], float m[3][3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    m[0][1] = 2.0f * (x * y - w * z);
    m[0][2] = 2.0f * (x * z + w * y);
    m[1][0] = 2.0f * (x * y + w * z);
    m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    m[1][2] = 2.0f * (y * z - w * x);
    m[2][0] = 2.0f * (x * z - w * y);
    m[2][1] = 2.0f * (y * z + w * x);
    m[2][2] = 1.0f - 2.0f * (x * x + y * y);
}

static uint32_t ShadeChannel(uint32_t packed, uint32_t shift, float light)
{
    const float value = (float)((packed >> shift) & 255u) * light;
    const uint32_t channel =
        value >= 255.0f ? 255u : (value <= 0.0f ? 0u : (uint32_t)(value + 0.5f));
    return channel << shift;
}

/* ---- lifecycle ---------------------------------------------------------- */

static uint32_t RendererCreate(const LaiueMeshWorldRendererConfigV1 *config,
                               LaiueMeshWorldRendererV1 **outRenderer)
{
    if (outRenderer == NULL)
        return 0u;
    *outRenderer = NULL;
    if (config == NULL || config->structSize < sizeof(LaiueMeshWorldRendererConfigV1) ||
        config->worldService == NULL ||
        config->worldService->structSize < sizeof(LaiueMeshWorldServiceV1) ||
        config->world == NULL || config->device == NULL ||
        config->device->structSize < sizeof(LaiueGraphicsDeviceV2) ||
        config->device->createBuffer == NULL || config->device->uploadBuffer == NULL ||
        config->device->destroyHandle == NULL ||
        config->maximumBuffers > RENDER_MAXIMUM_BUFFERS_LIMIT)
        return 0u;
    LaiueMeshWorldRendererV1 *renderer =
        (LaiueMeshWorldRendererV1 *)PlatformAllocate(sizeof(*renderer), true);
    if (renderer == NULL)
        return 0u;
    renderer->worldService = config->worldService;
    renderer->world = config->world;
    renderer->device = config->device;
    renderer->maximumBuffers =
        config->maximumBuffers == 0u ? RENDER_DEFAULT_MAXIMUM_BUFFERS : config->maximumBuffers;
    renderer->cellSize = config->worldService->cellSize(config->world);
    renderer->generation = 1u;
    /* A sun high in the south-east and a soft sky: readable shapes before
     * the application chooses its own light. */
    renderer->toSun[0] = 0.32f;
    renderer->toSun[1] = 0.48f;
    renderer->toSun[2] = 0.82f;
    renderer->sunColor[0] = renderer->sunColor[1] = renderer->sunColor[2] = 0.7f;
    renderer->ambientColor[0] = renderer->ambientColor[1] = renderer->ambientColor[2] = 0.35f;
    renderer->stats.structSize = sizeof(LaiueMeshRenderStatsV1);
    *outRenderer = renderer;
    return 1u;
}

static void RendererDestroy(LaiueMeshWorldRendererV1 *renderer)
{
    if (renderer == NULL)
        return;
    for (uint32_t i = 0u; i < renderer->cellCount; ++i)
        ReleaseBatches(renderer, &renderer->cells[i]);
    for (uint32_t i = 0u; i < renderer->modelCount; ++i)
    {
        PlatformFree(renderer->models[i].vertices);
        PlatformFree(renderer->models[i].indices);
        PlatformFree(renderer->models[i].parts);
    }
    for (uint32_t i = 0u; i < renderer->accumulatorCapacity; ++i)
        PlatformFree(renderer->accumulators[i].vertices);
    PlatformFree(renderer->accumulators);
    PlatformFree(renderer->cells);
    PlatformFree(renderer->models);
    PlatformFree(renderer->materials);
    PlatformFree(renderer->query);
    PlatformFree(renderer->instances);
    PlatformFree(renderer->candidates);
    PlatformFree(renderer);
}

/* ---- models, materials, lighting ---------------------------------------- */

static uint32_t RendererRegisterModel(LaiueMeshWorldRendererV1 *renderer, uint32_t model,
                                      const LaiueMeshRenderModelV1 *geometry)
{
    if (renderer == NULL || geometry == NULL ||
        geometry->structSize < sizeof(LaiueMeshRenderModelV1) || geometry->vertexCount == 0u ||
        geometry->vertices == NULL || geometry->vertexCount > RENDER_MAX_MODEL_VERTICES ||
        geometry->indexCount == 0u || geometry->indices == NULL ||
        geometry->indexCount > RENDER_MAX_MODEL_INDICES || geometry->indexCount % 3u != 0u ||
        (geometry->partCount != 0u && geometry->parts == NULL) ||
        geometry->partCount > geometry->indexCount / 3u)
        return 0u;
    for (uint32_t i = 0u; i < geometry->indexCount; ++i)
        if (geometry->indices[i] >= geometry->vertexCount)
            return 0u;
    for (uint32_t i = 0u; i < geometry->partCount; ++i)
    {
        const LaiueMeshRenderPartV1 *part = &geometry->parts[i];
        if (part->firstIndex > geometry->indexCount ||
            part->indexCount > geometry->indexCount - part->firstIndex ||
            part->indexCount % 3u != 0u || part->firstIndex % 3u != 0u)
            return 0u;
    }
    for (uint32_t i = 0u; i < geometry->vertexCount; ++i)
    {
        const LaiueMeshRenderVertexV1 *vertex = &geometry->vertices[i];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (!FiniteFloat(vertex->position[axis]) || !FiniteFloat(vertex->normal[axis]))
                return 0u;
        if (!FiniteFloat(vertex->uv[0]) || !FiniteFloat(vertex->uv[1]))
            return 0u;
    }

    RenderModel copy;
    memset(&copy, 0, sizeof(copy));
    copy.model = model;
    copy.vertexCount = geometry->vertexCount;
    copy.indexCount = geometry->indexCount;
    copy.partCount = geometry->partCount == 0u ? 1u : geometry->partCount;
    copy.vertices = (LaiueMeshRenderVertexV1 *)PlatformAllocate(
        sizeof(LaiueMeshRenderVertexV1) * copy.vertexCount, false);
    copy.indices = (uint32_t *)PlatformAllocate(sizeof(uint32_t) * copy.indexCount, false);
    copy.parts = (LaiueMeshRenderPartV1 *)PlatformAllocate(
        sizeof(LaiueMeshRenderPartV1) * copy.partCount, false);
    bool found = false;
    const uint32_t index = FindModelIndex(renderer, model, &found);
    RenderModel *grown =
        found ? renderer->models
              : (RenderModel *)GrowArray(renderer->models, &renderer->modelCapacity,
                                         renderer->modelCount + 1u, sizeof(RenderModel));
    if (copy.vertices == NULL || copy.indices == NULL || copy.parts == NULL || grown == NULL)
    {
        PlatformFree(copy.vertices);
        PlatformFree(copy.indices);
        PlatformFree(copy.parts);
        return 0u;
    }
    renderer->models = grown;
    memcpy(copy.vertices, geometry->vertices, sizeof(LaiueMeshRenderVertexV1) * copy.vertexCount);
    memcpy(copy.indices, geometry->indices, sizeof(uint32_t) * copy.indexCount);
    if (geometry->partCount != 0u)
    {
        memcpy(copy.parts, geometry->parts, sizeof(LaiueMeshRenderPartV1) * copy.partCount);
    }
    else
    {
        copy.parts[0].firstIndex = 0u;
        copy.parts[0].indexCount = copy.indexCount;
        copy.parts[0].material = 0u;
    }
    if (found)
    {
        PlatformFree(renderer->models[index].vertices);
        PlatformFree(renderer->models[index].indices);
        PlatformFree(renderer->models[index].parts);
    }
    else
    {
        for (uint32_t i = renderer->modelCount; i > index; --i)
            renderer->models[i] = renderer->models[i - 1u];
        ++renderer->modelCount;
    }
    renderer->models[index] = copy;
    ++renderer->generation;
    return 1u;
}

static uint32_t RendererSetMaterial(LaiueMeshWorldRendererV1 *renderer, uint32_t material,
                                    LaiueGraphicsHandle texture, LaiueGraphicsHandle sampler)
{
    if (renderer == NULL)
        return 0u;
    bool found = false;
    const uint32_t index = FindMaterialIndex(renderer, material, &found);
    if (!found)
    {
        RenderMaterial *grown =
            (RenderMaterial *)GrowArray(renderer->materials, &renderer->materialCapacity,
                                        renderer->materialCount + 1u, sizeof(RenderMaterial));
        if (grown == NULL)
            return 0u;
        renderer->materials = grown;
        for (uint32_t i = renderer->materialCount; i > index; --i)
            renderer->materials[i] = renderer->materials[i - 1u];
        ++renderer->materialCount;
    }
    renderer->materials[index].material = material;
    renderer->materials[index].texture = texture;
    renderer->materials[index].sampler = sampler;
    return 1u;
}

static uint32_t RendererSetLighting(LaiueMeshWorldRendererV1 *renderer,
                                    const LaiueMeshRenderLightingV1 *lighting)
{
    if (renderer == NULL || lighting == NULL ||
        lighting->structSize < sizeof(LaiueMeshRenderLightingV1))
        return 0u;
    float lengthSquared = 0.0f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!FiniteFloat(lighting->toSun[axis]) || !FiniteFloat(lighting->sunColor[axis]) ||
            !FiniteFloat(lighting->ambientColor[axis]) || lighting->sunColor[axis] < 0.0f ||
            lighting->ambientColor[axis] < 0.0f)
            return 0u;
        lengthSquared += lighting->toSun[axis] * lighting->toSun[axis];
    }
    if (!(lengthSquared > 1.0e-12f))
        return 0u;
    const float inverse = 1.0f / ScalarSqrt(lengthSquared);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        renderer->toSun[axis] = lighting->toSun[axis] * inverse;
        renderer->sunColor[axis] = lighting->sunColor[axis];
        renderer->ambientColor[axis] = lighting->ambientColor[axis];
    }
    ++renderer->generation;
    return 1u;
}

/* ---- cell building ------------------------------------------------------ */

static RenderAccumulator *Accumulator(LaiueMeshWorldRendererV1 *renderer, uint32_t material)
{
    for (uint32_t i = 0u; i < renderer->accumulatorCount; ++i)
        if (renderer->accumulators[i].material == material)
            return &renderer->accumulators[i];
    if (renderer->accumulatorCount == renderer->accumulatorCapacity)
    {
        const uint32_t previous = renderer->accumulatorCapacity;
        RenderAccumulator *grown = (RenderAccumulator *)GrowArray(
            renderer->accumulators, &renderer->accumulatorCapacity, renderer->accumulatorCount + 1u,
            sizeof(RenderAccumulator));
        if (grown == NULL)
            return NULL;
        renderer->accumulators = grown;
        memset(&renderer->accumulators[previous], 0,
               sizeof(RenderAccumulator) * (renderer->accumulatorCapacity - previous));
    }
    RenderAccumulator *accumulator = &renderer->accumulators[renderer->accumulatorCount++];
    accumulator->material = material;
    accumulator->count = 0u;
    return accumulator;
}

static bool AppendInstance(LaiueMeshWorldRendererV1 *renderer, const RenderModel *model,
                           const LaiueMeshTransformV1 *transform, float boundsMin[3],
                           float boundsMax[3], bool *firstVertex)
{
    float rotation[3][3];
    QuaternionMatrix(transform->rotation, rotation);
    const float *scale = transform->scale;
    const float inverseScale[3] = {1.0f / scale[0], 1.0f / scale[1], 1.0f / scale[2]};
    for (uint32_t p = 0u; p < model->partCount; ++p)
    {
        const LaiueMeshRenderPartV1 *part = &model->parts[p];
        if (part->indexCount == 0u)
            continue;
        RenderAccumulator *accumulator = Accumulator(renderer, part->material);
        if (accumulator == NULL || part->indexCount > RENDER_MAX_CELL_VERTICES - accumulator->count)
            return false;
        LaiueGraphicsVertexV2 *vertices = (LaiueGraphicsVertexV2 *)GrowArray(
            accumulator->vertices, &accumulator->capacity, accumulator->count + part->indexCount,
            sizeof(LaiueGraphicsVertexV2));
        if (vertices == NULL)
            return false;
        accumulator->vertices = vertices;
        for (uint32_t i = 0u; i < part->indexCount; ++i)
        {
            const LaiueMeshRenderVertexV1 *source =
                &model->vertices[model->indices[part->firstIndex + i]];
            LaiueGraphicsVertexV2 *out = &vertices[accumulator->count + i];
            const float scaled[3] = {source->position[0] * scale[0], source->position[1] * scale[1],
                                     source->position[2] * scale[2]};
            /* Normals take the inverse-transpose: divide by the scale. */
            const float bent[3] = {source->normal[0] * inverseScale[0],
                                   source->normal[1] * inverseScale[1],
                                   source->normal[2] * inverseScale[2]};
            float normal[3];
            for (uint32_t r = 0u; r < 3u; ++r)
            {
                out->position[r] = transform->position.local[r] + rotation[r][0] * scaled[0] +
                                   rotation[r][1] * scaled[1] + rotation[r][2] * scaled[2];
                normal[r] =
                    rotation[r][0] * bent[0] + rotation[r][1] * bent[1] + rotation[r][2] * bent[2];
                if (*firstVertex || out->position[r] < boundsMin[r])
                    boundsMin[r] = out->position[r];
                if (*firstVertex || out->position[r] > boundsMax[r])
                    boundsMax[r] = out->position[r];
            }
            *firstVertex = false;
            const float lengthSquared =
                normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
            float facing = 1.0f;
            if (lengthSquared > 1.0e-12f)
                facing = (normal[0] * renderer->toSun[0] + normal[1] * renderer->toSun[1] +
                          normal[2] * renderer->toSun[2]) /
                         ScalarSqrt(lengthSquared);
            if (facing < 0.0f)
                facing = 0.0f;
            out->uv[0] = source->uv[0];
            out->uv[1] = source->uv[1];
            const uint32_t color = source->colorRGBA;
            out->colorRGBA =
                ShadeChannel(color, 0u,
                             renderer->ambientColor[0] + renderer->sunColor[0] * facing) |
                ShadeChannel(color, 8u,
                             renderer->ambientColor[1] + renderer->sunColor[1] * facing) |
                ShadeChannel(color, 16u,
                             renderer->ambientColor[2] + renderer->sunColor[2] * facing) |
                (color & UINT32_C(0xFF000000));
        }
        accumulator->count += part->indexCount;
    }
    return true;
}

typedef enum RenderBuildResult
{
    RENDER_BUILD_OK,
    RENDER_BUILD_OVER_BUDGET,
    RENDER_BUILD_FAILED,
} RenderBuildResult;

static RenderBuildResult BuildCell(LaiueMeshWorldRendererV1 *renderer, RenderCell *cell)
{
    const LaiueMeshWorldServiceV1 *world = renderer->worldService;
    /* Read the revision first: a change after it only causes one more
     * rebuild, never a stale cell that claims to be current. */
    const uint64_t revision = world->cellRevision(renderer->world, &cell->coord);
    uint32_t count = 0u;
    if (world->cellInstances(renderer->world, &cell->coord, NULL, 0u, &count) == 0u)
        return RENDER_BUILD_FAILED;
    LaiueMeshInstanceInfoV1 *instances = (LaiueMeshInstanceInfoV1 *)GrowArray(
        renderer->instances, &renderer->instanceCapacity, count, sizeof(LaiueMeshInstanceInfoV1));
    if (count != 0u && instances == NULL)
        return RENDER_BUILD_FAILED;
    renderer->instances = instances;
    uint32_t listed = 0u;
    if (count != 0u &&
        (world->cellInstances(renderer->world, &cell->coord, instances, count, &listed) == 0u))
        return RENDER_BUILD_FAILED;
    if (listed < count)
        count = listed;

    renderer->accumulatorCount = 0u;
    float boundsMin[3] = {0.0f, 0.0f, 0.0f};
    float boundsMax[3] = {0.0f, 0.0f, 0.0f};
    bool firstVertex = true;
    for (uint32_t i = 0u; i < count; ++i)
    {
        const LaiueMeshInstanceInfoV1 *instance = &instances[i];
        if ((instance->flags & LAIUE_MESH_INSTANCE_VISIBLE) == 0u)
            continue;
        bool found = false;
        const uint32_t modelIndex = FindModelIndex(renderer, instance->model, &found);
        if (!found)
            continue;
        if (!AppendInstance(renderer, &renderer->models[modelIndex], &instance->transform,
                            boundsMin, boundsMax, &firstVertex))
            return RENDER_BUILD_FAILED;
    }

    uint32_t needed = 0u;
    for (uint32_t i = 0u; i < renderer->accumulatorCount; ++i)
        needed += renderer->accumulators[i].count != 0u ? 1u : 0u;
    if (renderer->bufferCount - cell->batchCount + needed > renderer->maximumBuffers)
        return RENDER_BUILD_OVER_BUDGET;

    RenderBatch *batches = NULL;
    if (needed != 0u)
    {
        batches = (RenderBatch *)PlatformAllocate(sizeof(RenderBatch) * needed, false);
        if (batches == NULL)
            return RENDER_BUILD_FAILED;
    }
    uint32_t created = 0u;
    for (uint32_t i = 0u; i < renderer->accumulatorCount; ++i)
    {
        const RenderAccumulator *accumulator = &renderer->accumulators[i];
        if (accumulator->count == 0u)
            continue;
        const uint64_t bytes = (uint64_t)accumulator->count * sizeof(LaiueGraphicsVertexV2);
        LaiueGraphicsBufferDescV1 desc = {sizeof(desc), LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX, bytes};
        LaiueGraphicsHandle buffer = 0u;
        LaiueGraphicsBufferUploadV1 upload = {sizeof(upload), 0u, 0u, accumulator->vertices, bytes};
        bool ok =
            renderer->device->createBuffer(renderer->device, &desc, &buffer) != 0u && buffer != 0u;
        if (ok)
        {
            upload.buffer = buffer;
            ok = renderer->device->uploadBuffer(renderer->device, &upload) != 0u;
            if (!ok)
                renderer->device->destroyHandle(renderer->device, buffer);
        }
        if (!ok)
        {
            for (uint32_t k = 0u; k < created; ++k)
                renderer->device->destroyHandle(renderer->device, batches[k].buffer);
            PlatformFree(batches);
            return RENDER_BUILD_FAILED;
        }
        batches[created].material = accumulator->material;
        batches[created].vertexCount = accumulator->count;
        batches[created].buffer = buffer;
        ++created;
    }
    ReleaseBatches(renderer, cell);
    cell->batches = batches;
    cell->batchCount = created;
    for (uint32_t i = 0u; i < created; ++i)
        renderer->vertexCount += batches[i].vertexCount;
    renderer->bufferCount += created;
    cell->revision = revision;
    cell->generation = renderer->generation;
    cell->built = 1u;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        cell->boundsMin[axis] = boundsMin[axis];
        cell->boundsMax[axis] = boundsMax[axis];
    }
    return RENDER_BUILD_OK;
}

static void SortCandidates(RenderCandidate *candidates, uint32_t count)
{
    /* Heap sort by distance, then by cell index for a deterministic order. */
    if (count < 2u)
        return;
    for (uint32_t start = count / 2u; start-- > 0u;)
    {
        uint32_t root = start;
        for (;;)
        {
            uint32_t child = root * 2u + 1u;
            if (child >= count)
                break;
            if (child + 1u < count &&
                (candidates[child].distanceSquared < candidates[child + 1u].distanceSquared ||
                 (candidates[child].distanceSquared == candidates[child + 1u].distanceSquared &&
                  candidates[child].cell < candidates[child + 1u].cell)))
                ++child;
            if (candidates[root].distanceSquared > candidates[child].distanceSquared ||
                (candidates[root].distanceSquared == candidates[child].distanceSquared &&
                 candidates[root].cell >= candidates[child].cell))
                break;
            const RenderCandidate swap = candidates[root];
            candidates[root] = candidates[child];
            candidates[child] = swap;
            root = child;
        }
    }
    for (uint32_t end = count - 1u; end > 0u; --end)
    {
        const RenderCandidate swap = candidates[0];
        candidates[0] = candidates[end];
        candidates[end] = swap;
        uint32_t root = 0u;
        for (;;)
        {
            uint32_t child = root * 2u + 1u;
            if (child >= end)
                break;
            if (child + 1u < end &&
                (candidates[child].distanceSquared < candidates[child + 1u].distanceSquared ||
                 (candidates[child].distanceSquared == candidates[child + 1u].distanceSquared &&
                  candidates[child].cell < candidates[child + 1u].cell)))
                ++child;
            if (candidates[root].distanceSquared > candidates[child].distanceSquared ||
                (candidates[root].distanceSquared == candidates[child].distanceSquared &&
                 candidates[root].cell >= candidates[child].cell))
                break;
            const RenderCandidate swap2 = candidates[root];
            candidates[root] = candidates[child];
            candidates[child] = swap2;
            root = child;
        }
    }
}

/* ---- update and draws --------------------------------------------------- */

static uint32_t RendererUpdate(LaiueMeshWorldRendererV1 *renderer,
                               const LaiueMeshPositionV1 *camera, float radius, uint32_t cellBudget,
                               uint32_t *outPending)
{
    if (outPending != NULL)
        *outPending = 0u;
    if (renderer == NULL || camera == NULL || !FiniteFloat(radius) || radius < 0.0f ||
        radius > RENDER_MAX_QUERY_RADIUS)
        return 0u;
    const LaiueMeshWorldServiceV1 *world = renderer->worldService;
    uint32_t count = 0u;
    if (world->queryCells(renderer->world, camera, radius, renderer->query, renderer->queryCapacity,
                          &count) == 0u)
        return 0u;
    if (count > renderer->queryCapacity)
    {
        LaiueMeshCellV1 *grown = (LaiueMeshCellV1 *)GrowArray(
            renderer->query, &renderer->queryCapacity, count, sizeof(LaiueMeshCellV1));
        if (grown == NULL)
            return 0u;
        renderer->query = grown;
        if (world->queryCells(renderer->world, camera, radius, renderer->query,
                              renderer->queryCapacity, &count) == 0u ||
            count > renderer->queryCapacity)
            return 0u;
    }

    /* Merge the sorted cache with the sorted answer: cells that left the
     * radius release their buffers, new ones join unbuilt. */
    RenderCell *next = (RenderCell *)PlatformAllocate(sizeof(RenderCell) * (count + 1u), false);
    RenderCandidate *candidates = (RenderCandidate *)GrowArray(
        renderer->candidates, &renderer->candidateCapacity, count + 1u, sizeof(RenderCandidate));
    if (next == NULL || candidates == NULL)
    {
        PlatformFree(next);
        return 0u;
    }
    renderer->candidates = candidates;
    uint32_t old = 0u;
    for (uint32_t i = 0u; i < count; ++i)
    {
        while (old < renderer->cellCount &&
               CompareCells(&renderer->cells[old].coord, &renderer->query[i]) < 0)
            ReleaseBatches(renderer, &renderer->cells[old++]);
        if (old < renderer->cellCount &&
            CellEqual(&renderer->cells[old].coord, &renderer->query[i]))
        {
            next[i] = renderer->cells[old++];
        }
        else
        {
            memset(&next[i], 0, sizeof(RenderCell));
            next[i].coord = renderer->query[i];
        }
    }
    while (old < renderer->cellCount)
        ReleaseBatches(renderer, &renderer->cells[old++]);
    PlatformFree(renderer->cells);
    renderer->cells = next;
    renderer->cellCount = count;
    renderer->cellCapacity = count + 1u;

    uint32_t candidateCount = 0u;
    LaiueMeshPositionV1 center = *camera;
    for (uint32_t i = 0u; i < count; ++i)
    {
        RenderCell *cell = &renderer->cells[i];
        if (cell->built != 0u && cell->generation == renderer->generation &&
            cell->revision == world->cellRevision(renderer->world, &cell->coord))
            continue;
        double distanceSquared = 0.0;
        const int64_t offsets[3] = {cell->coord.x - center.cell.x, cell->coord.y - center.cell.y,
                                    cell->coord.z - center.cell.z};
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const double metres = ((double)offsets[axis] + 0.5) * (double)renderer->cellSize -
                                  (double)center.local[axis];
            distanceSquared += metres * metres;
        }
        candidates[candidateCount].cell = i;
        candidates[candidateCount].distanceSquared = distanceSquared;
        ++candidateCount;
    }
    SortCandidates(candidates, candidateCount);

    uint32_t rebuilt = 0u;
    uint32_t overBudget = 0u;
    uint32_t pending = 0u;
    uint32_t result = 1u;
    for (uint32_t i = 0u; i < candidateCount; ++i)
    {
        if (cellBudget != 0u && rebuilt >= cellBudget)
        {
            pending = candidateCount - i;
            break;
        }
        const RenderBuildResult built = BuildCell(renderer, &renderer->cells[candidates[i].cell]);
        if (built == RENDER_BUILD_FAILED)
        {
            pending = candidateCount - i;
            result = 0u;
            break;
        }
        if (built == RENDER_BUILD_OVER_BUDGET)
            ++overBudget;
        else
            ++rebuilt;
    }
    renderer->stats.cells = renderer->cellCount;
    renderer->stats.buffers = renderer->bufferCount;
    renderer->stats.vertices = renderer->vertexCount;
    renderer->stats.rebuilt = rebuilt;
    renderer->stats.pending = pending;
    renderer->stats.overBudget = overBudget;
    if (outPending != NULL)
        *outPending = pending;
    return result;
}

static void FrustumPlanes(const float m[16], float planes[6][4])
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        const uint32_t sign = i % 2u;
        const uint32_t column = i / 2u;
        const float s = sign == 0u ? 1.0f : -1.0f;
        planes[i][0] = m[3] + s * m[column];
        planes[i][1] = m[7] + s * m[4u + column];
        planes[i][2] = m[11] + s * m[8u + column];
        planes[i][3] = m[15] + s * m[12u + column];
    }
    planes[4][0] = m[2];
    planes[4][1] = m[6];
    planes[4][2] = m[10];
    planes[4][3] = m[14];
    planes[5][0] = m[3] - m[2];
    planes[5][1] = m[7] - m[6];
    planes[5][2] = m[11] - m[10];
    planes[5][3] = m[15] - m[14];
}

static bool BoxVisible(float planes[6][4], const float minimum[3], const float maximum[3])
{
    for (uint32_t i = 0u; i < 6u; ++i)
    {
        const float x = planes[i][0] >= 0.0f ? maximum[0] : minimum[0];
        const float y = planes[i][1] >= 0.0f ? maximum[1] : minimum[1];
        const float z = planes[i][2] >= 0.0f ? maximum[2] : minimum[2];
        if (planes[i][0] * x + planes[i][1] * y + planes[i][2] * z + planes[i][3] < 0.0f)
            return false;
    }
    return true;
}

static uint32_t RendererDraws(LaiueMeshWorldRendererV1 *renderer,
                              const LaiueMeshPositionV1 *renderOrigin,
                              const float viewProjection[16], LaiueGraphicsDrawItemV2 *outItems,
                              uint32_t capacity, uint32_t *outCount)
{
    if (outCount != NULL)
        *outCount = 0u;
    if (renderer == NULL || renderOrigin == NULL || outCount == NULL ||
        (capacity != 0u && outItems == NULL))
        return 0u;
    float planes[6][4] = {{0.0f}};
    if (viewProjection != NULL)
        FrustumPlanes(viewProjection, planes);
    uint32_t written = 0u;
    uint32_t culled = 0u;
    for (uint32_t i = 0u; i < renderer->cellCount; ++i)
    {
        const RenderCell *cell = &renderer->cells[i];
        if (cell->batchCount == 0u)
            continue;
        float offset[3];
        const int64_t offsets[3] = {cell->coord.x - renderOrigin->cell.x,
                                    cell->coord.y - renderOrigin->cell.y,
                                    cell->coord.z - renderOrigin->cell.z};
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            offset[axis] = (float)((double)offsets[axis] * (double)renderer->cellSize -
                                   (double)renderOrigin->local[axis]);
        if (viewProjection != NULL)
        {
            const float minimum[3] = {offset[0] + cell->boundsMin[0],
                                      offset[1] + cell->boundsMin[1],
                                      offset[2] + cell->boundsMin[2]};
            const float maximum[3] = {offset[0] + cell->boundsMax[0],
                                      offset[1] + cell->boundsMax[1],
                                      offset[2] + cell->boundsMax[2]};
            if (!BoxVisible(planes, minimum, maximum))
            {
                ++culled;
                continue;
            }
        }
        for (uint32_t b = 0u; b < cell->batchCount; ++b)
        {
            if (written < capacity)
            {
                const RenderBatch *batch = &cell->batches[b];
                LaiueGraphicsDrawItemV2 *item = &outItems[written];
                memset(item, 0, sizeof(*item));
                item->structSize = sizeof(LaiueGraphicsDrawItemV2);
                item->vertexBuffer = batch->buffer;
                item->indexCount = batch->vertexCount;
                item->originRelative[0] = offset[0];
                item->originRelative[1] = offset[1];
                item->originRelative[2] = offset[2];
                item->scale = 1.0f;
                bool found = false;
                const uint32_t material = FindMaterialIndex(renderer, batch->material, &found);
                if (found)
                {
                    item->texture = renderer->materials[material].texture;
                    item->sampler = renderer->materials[material].sampler;
                }
            }
            ++written;
        }
    }
    renderer->stats.drawn = written < capacity ? written : capacity;
    renderer->stats.culled = culled;
    *outCount = written;
    return 1u;
}

static uint32_t RendererStats(const LaiueMeshWorldRendererV1 *renderer,
                              LaiueMeshRenderStatsV1 *outStats)
{
    if (renderer == NULL || outStats == NULL ||
        outStats->structSize < sizeof(LaiueMeshRenderStatsV1))
        return 0u;
    *outStats = renderer->stats;
    return 1u;
}

static const LaiueMeshWorldRenderServiceV1 g_service = {
    .structSize = sizeof(LaiueMeshWorldRenderServiceV1),
    .abiVersion = LAIUE_MESH_WORLD_RENDER_SERVICE_ABI_VERSION_1,
    .create = RendererCreate,
    .destroy = RendererDestroy,
    .registerModel = RendererRegisterModel,
    .setMaterial = RendererSetMaterial,
    .setLighting = RendererSetLighting,
    .update = RendererUpdate,
    .draws = RendererDraws,
    .stats = RendererStats,
};

const LaiueMeshWorldRenderServiceV1 *LaiueMeshWorldRenderGetStaticServiceV1(void)
{
    return &g_service;
}
