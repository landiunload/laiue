#pragma once

/* Draws a laiue.mesh_world through the public graphics device.  Every cell
 * near the camera becomes a few vertex buffers, one per material, with the
 * instances' geometry transformed into the cell's frame and the sun and
 * ambient light baked into the vertex colours; the generic graphics
 * pipeline draws them unlit.  A cell is rebuilt only when its revision
 * changes, so a static world costs nothing after the first frames.
 *
 * The adapter knows neither model files nor textures: the application
 * registers each model's geometry (a model pack view has the same vertex
 * layout) and binds each material id to a texture and sampler it created.
 * One renderer is used from one thread; update() must not run between the
 * device's beginFrame and endFrame. */

#include "api.h"
#include "graphics/graphics_device_v2.h"
#include "mesh_world/mesh_world_service.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_MESH_WORLD_RENDER_SERVICE_NAME "laiue.mesh_world_render"
#define LAIUE_MESH_WORLD_RENDER_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueMeshWorldRendererV1 LaiueMeshWorldRendererV1;

/* The 36-byte layout of a model pack vertex (LaiueModelVertexV1 and the
 * VERT section of .lo), so a model view can be registered without copying
 * it into another format first. */
typedef struct LaiueMeshRenderVertexV1
{
    float position[3];
    float normal[3];
    float uv[2];
    uint32_t colorRGBA; /* red in the low byte */
} LaiueMeshRenderVertexV1;

typedef struct LaiueMeshRenderPartV1
{
    uint32_t firstIndex;
    uint32_t indexCount;
    uint32_t material; /* application material id */
} LaiueMeshRenderPartV1;

/* Geometry is copied by registerModel.  partCount 0 draws every index with
 * material 0. */
typedef struct LaiueMeshRenderModelV1
{
    uint32_t structSize;
    const LaiueMeshRenderVertexV1 *vertices;
    uint32_t vertexCount;
    const uint32_t *indices;
    uint32_t indexCount;
    const LaiueMeshRenderPartV1 *parts;
    uint32_t partCount;
} LaiueMeshRenderModelV1;

/* toSun points from the ground towards the sun; it need not be unit
 * length.  Colours are linear multipliers, normally in [0, 1]. */
typedef struct LaiueMeshRenderLightingV1
{
    uint32_t structSize;
    float toSun[3];
    float sunColor[3];
    float ambientColor[3];
} LaiueMeshRenderLightingV1;

typedef struct LaiueMeshWorldRendererConfigV1
{
    uint32_t structSize;
    const LaiueMeshWorldServiceV1 *worldService;
    LaiueMeshWorldV1 *world;
    LaiueGraphicsDeviceV2 *device;
    /* Cached vertex buffers; 0 selects 1024. Cells beyond the budget are
     * left undrawn,
     * farthest first. A transactional rebuild briefly holds
     * its replacement buffers as well,
     * so a failed upload keeps the cache. */
    uint32_t maximumBuffers;
} LaiueMeshWorldRendererConfigV1;

typedef struct LaiueMeshRenderStatsV1
{
    uint32_t structSize;
    uint32_t cells;      /* cells held by the renderer */
    uint32_t buffers;    /* vertex buffers held */
    uint64_t vertices;   /* vertices held */
    uint32_t rebuilt;    /* cells rebuilt by the last update */
    uint32_t pending;    /* rebuild backlog (cell budget/failure), excluding overBudget */
    uint32_t overBudget; /* cells evicted or refused a rebuild by the buffer budget */
    uint32_t drawn;      /* draw items written by the last draws call */
    uint32_t culled;     /* cells the frustum rejected in the last draws call */
} LaiueMeshRenderStatsV1;

typedef struct LaiueMeshWorldRenderServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    uint32_t (*create)(const LaiueMeshWorldRendererConfigV1 *config,
                       LaiueMeshWorldRendererV1 **outRenderer);
    /* Releases every buffer the renderer created on the device. */
    void (*destroy)(LaiueMeshWorldRendererV1 *renderer);
    /* Registering or replacing a model rebuilds the cells progressively. */
    uint32_t (*registerModel)(LaiueMeshWorldRendererV1 *renderer, uint32_t model,
                              const LaiueMeshRenderModelV1 *geometry);
    /* Texture and sampler handles of the device; 0 selects the device's
     * white texture and default sampler.  Takes effect at the next draws. */
    uint32_t (*setMaterial)(LaiueMeshWorldRendererV1 *renderer, uint32_t material,
                            LaiueGraphicsHandle texture, LaiueGraphicsHandle sampler);
    /* New lighting rebakes the cells progressively. */
    uint32_t (*setLighting)(LaiueMeshWorldRendererV1 *renderer,
                            const LaiueMeshRenderLightingV1 *lighting);
    /* Keeps the cells within radius of camera current: rebuilds changed
     * cells nearest first, at most cellBudget per call (0 means no limit),
     * and releases cells that left the radius.  outPending, when not NULL,
     * receives the rebuild backlog; cells excluded by maximumBuffers are
     * counted separately
     * in stats.overBudget. */
    uint32_t (*update)(LaiueMeshWorldRendererV1 *renderer, const LaiueMeshPositionV1 *camera,
                       float radius, uint32_t cellBudget, uint32_t *outPending);
    /* Draw items relative to renderOrigin, the point the camera's
     * view-projection is built around.  viewProjection, when not NULL, culls
     * cells outside the frustum (row-major, clip = position * matrix, depth
     * 0..1, the convention of the scene math module).  outCount receives the
     * full number. Origins must have finite local offsets and cell coordinates
     * within
     * +-LAIUE_MESH_WORLD_MAX_CELL; every supplied matrix value must be
     * finite. Invalid
     * inputs fail with outCount zero. */
    uint32_t (*draws)(LaiueMeshWorldRendererV1 *renderer, const LaiueMeshPositionV1 *renderOrigin,
                      const float viewProjection[16], LaiueGraphicsDrawItemV2 *outItems,
                      uint32_t capacity, uint32_t *outCount);
    uint32_t (*stats)(const LaiueMeshWorldRendererV1 *renderer, LaiueMeshRenderStatsV1 *outStats);
    uintptr_t reserved[8];
} LaiueMeshWorldRenderServiceV1;

LAIUE_MESH_WORLD_RENDER_API const LaiueModuleApiV1 *LaiueMeshWorldRenderGetStaticModuleApiV1(void);
LAIUE_MESH_WORLD_RENDER_API const LaiueMeshWorldRenderServiceV1 *
LaiueMeshWorldRenderGetStaticServiceV1(void);
