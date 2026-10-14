#pragma once

/* Model packs: `models/<name>.lop` directories with `.lo` models and their
 * Wavefront OBJ sources.  The service only loads geometry; it knows nothing
 * about GPUs, worlds or materials, so a server, a tool or a mesh world can
 * use it without the renderer.  Material names belong to the application,
 * exactly like texture names in a texture pack. */

#include "api.h"
#include "content/content_catalog.h"
#include "mod/module_api.h"

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#define LAIUE_MODEL_SERVICE_NAME "laiue.model"
#define LAIUE_MODEL_SERVICE_ABI_VERSION_1 1u

#define LAIUE_MODEL_LOAD_NOT_ATTEMPTED 0u
#define LAIUE_MODEL_LOAD_OK 1u
#define LAIUE_MODEL_LOAD_NO_ACTIVE_PACK 2u
#define LAIUE_MODEL_LOAD_NOT_FOUND 3u
#define LAIUE_MODEL_LOAD_INVALID_MODEL 4u
#define LAIUE_MODEL_LOAD_IO_ERROR 5u
#define LAIUE_MODEL_LOAD_OUT_OF_MEMORY 6u
#define LAIUE_MODEL_LOAD_NO_CATALOG 7u

#define LAIUE_MODEL_FLAG_NORMALS UINT32_C(1) << 0
#define LAIUE_MODEL_FLAG_UV UINT32_C(1) << 1
#define LAIUE_MODEL_FLAG_COLORS UINT32_C(1) << 2
/* The model is the built-in placeholder returned instead of a missing or
 * broken model; the load status names the reason. */
#define LAIUE_MODEL_FLAG_PLACEHOLDER UINT32_C(1) << 31

/* 36-byte vertex in engine axes: Z up, right-handed, texture origin at the
 * top left.  The record matches the `.lo` VERT section. */
typedef struct LaiueModelVertexV1
{
    float position[3];
    float normal[3];
    float uv[2];
    uint32_t colorRGBA;
} LaiueModelVertexV1;

typedef struct LaiueModelPartV1
{
    uint32_t firstIndex;
    uint32_t indexCount;
    /* UTF-8, NUL-terminated, owned by the model. */
    const char *material;
} LaiueModelPartV1;

/* Oriented collision box in model axes. */
typedef struct LaiueModelBoxV1
{
    float center[3];
    float halfExtent[3];
    float rotation[4]; /* unit quaternion (x, y, z, w) */
} LaiueModelBoxV1;

typedef struct LaiueModelViewV1
{
    uint32_t structSize;
    uint32_t flags;
    const LaiueModelVertexV1 *vertices;
    uint32_t vertexCount;
    uint32_t indexCount;
    const uint32_t *indices;
    const LaiueModelPartV1 *parts;
    uint32_t partCount;
    uint32_t boxCount;
    const LaiueModelBoxV1 *boxes;
    float boundsMin[3];
    float boundsMax[3];
} LaiueModelViewV1;

/* Optional .lo skin binding. Vertices have the same order as getView's
 * static geometry. Joint indices belong to the application's matching
 * animation rig, including indices with zero weights. */
typedef struct LaiueModelSkinVertexV1
{
    uint32_t joints[4];
    float weights[4]; /* nonnegative; sum within 0.001 of one */
} LaiueModelSkinVertexV1;

typedef struct LaiueModelSkinViewV1
{
    uint32_t structSize;
    uint32_t jointCount;
    uint32_t vertexCount;
    const LaiueModelSkinVertexV1 *vertices; /* owned by the model */
} LaiueModelSkinViewV1;

typedef struct LaiueModelV1 LaiueModelV1;

typedef struct LaiueModelNameV1
{
    wchar_t name[LAIUE_CONTENT_NAME_CAPACITY];
    uint32_t active;
} LaiueModelNameV1;

typedef struct LaiueModelListV1
{
    LaiueModelNameV1 *entries;
    uint32_t count;
} LaiueModelListV1;

typedef struct LaiueModelServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    /* Pack selection; the active name lives in models/active.txt. */
    uint32_t (*enumeratePacks)(LaiueContentCatalog *catalog, LaiueModelListV1 *outList);
    uint32_t (*activatePack)(LaiueContentCatalog *catalog, const wchar_t *packName);
    /* Models of the active pack, including subfolders: a name is the path
     * from the pack root without extension. */
    uint32_t (*enumerateModels)(LaiueContentCatalog *catalog, LaiueModelListV1 *outList);
    void (*releaseList)(LaiueModelListV1 *list);
    /* A missing or broken model yields the placeholder cube and a status
     * naming the reason; NULL means a program error (unsafe name) or no
     * memory.  The caller owns the result. */
    LaiueModelV1 *(*loadFrom)(LaiueContentCatalog *catalog, const wchar_t *modelName,
                              uint32_t *outStatus);
    /* Explicit file or memory: `.lo` or OBJ recognised by signature. */
    LaiueModelV1 *(*loadFile)(const wchar_t *path, uint32_t *outStatus);
    LaiueModelV1 *(*loadMemory)(const void *bytes, uint32_t sizeBytes, uint32_t *outStatus);
    uint32_t (*getView)(const LaiueModelV1 *model, LaiueModelViewV1 *outView);
    void (*release)(LaiueModelV1 *model);
    uintptr_t reserved[8];
    /* Optional tail. outView must have the alignment of LaiueModelSkinViewV1.
     * Set outView->structSize to its capacity before the call.
     * Static models succeed with zero counts and NULL vertices.
     * Invalid arguments leave the output untouched. Borrowed data stays
     * valid until release(model). No allocation or provider dependency. */
    uint32_t (*getSkinView)(const LaiueModelV1 *model, LaiueModelSkinViewV1 *outView);
} LaiueModelServiceV1;

#define LAIUE_MODEL_SERVICE_V1_LEGACY_SIZE ((uint32_t)offsetof(LaiueModelServiceV1, getSkinView))
#define LAIUE_MODEL_SERVICE_V1_SKIN_SIZE                                                           \
    ((uint32_t)(offsetof(LaiueModelServiceV1, getSkinView) +                                       \
                sizeof(((LaiueModelServiceV1 *)0)->getSkinView)))
#define LAIUE_MODEL_SERVICE_HAS_SKIN_V1(service)                                                   \
    ((service) != NULL && (service)->structSize >= LAIUE_MODEL_SERVICE_V1_SKIN_SIZE &&             \
     (service)->getSkinView != NULL)

struct LaiueContentServiceV1;

LAIUE_MODEL_API const LaiueModuleApiV1 *LaiueModelGetStaticModuleApiV1(void);
/* Direct static integrators can use the same table without a module host;
 * catalog operations then need the content table installed here.  The
 * module host installs it between create and start and clears it on stop. */
LAIUE_MODEL_API const LaiueModelServiceV1 *LaiueModelGetStaticServiceV1(void);
LAIUE_MODEL_API void LaiueModelSetContentService(const struct LaiueContentServiceV1 *content);
