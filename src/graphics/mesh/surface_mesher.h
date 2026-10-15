#pragma once

#include "api.h"

#include <stdint.h>

/* Surface meshing of a sampled density field (naive surface nets).
 *
 * Volumetric terrain, clouds, metaballs or any field that is positive inside
 * matter and not positive outside become a smooth triangle mesh: one vertex
 * per grid cell the surface passes through, placed at the mean of the edge
 * crossings, and one quad per sample edge the surface cuts. The function knows
 * nothing about worlds, materials or rendering; the caller owns every buffer,
 * so it allocates nothing and may run on any number of threads at once.
 *
 * Blocks of a larger field mesh without gaps or duplicate faces when they
 * overlap by sample layers and each emits only the edges it owns: sample a
 * block of N cells as N + 2 samples per axis starting one sample before it,
 * and emit edges starting in [1, N + 1). The neighbouring blocks then compute
 * identical vertices in the shared cells. */

#define LAIUE_SURFACE_MESH_OK 0u
#define LAIUE_SURFACE_MESH_INVALID 1u
#define LAIUE_SURFACE_MESH_CAPACITY 2u

#define LAIUE_SURFACE_MESH_MAX_SAMPLES_PER_AXIS 1024u
#define LAIUE_SURFACE_MESH_MAX_SAMPLES (1u << 24)

typedef struct LaiueSurfaceVertexV1
{
    /* In sample units: sample (i, j, k) sits at (i, j, k). */
    float position[3];
    /* Unit length, pointing out of the matter. */
    float normal[3];
    /* Material of the solid corner of the vertex's cell nearest the surface
     * (the smallest positive value): what lies right beneath it. 0 without a
     * material array. */
    uint32_t material;
} LaiueSurfaceVertexV1;

typedef struct LaiueSurfaceMeshRequestV1
{
    uint32_t structSize;
    /* Samples per axis, 2..1024 each and at most 2^24 in total. */
    uint32_t countX;
    uint32_t countY;
    uint32_t countZ;
    /* countX * countY * countZ values, X fastest, then Y, then Z. Positive is
     * inside matter. */
    const float *values;
    /* Optional, same layout. */
    const uint8_t *materials;
    /* Faces are emitted only for sample edges whose start lies in
     * [emitMin, emitMax) on every axis. All zero emits every edge. */
    uint32_t emitMin[3];
    uint32_t emitMax[3];
    /* One entry per cell: (countX - 1) * (countY - 1) * (countZ - 1). */
    uint32_t *cellScratch;
    uint32_t cellScratchCapacity;
    LaiueSurfaceVertexV1 *vertices;
    uint32_t vertexCapacity;
    /* Triangles, counter-clockwise seen from outside the matter. */
    uint32_t *indices;
    uint32_t indexCapacity;
    /* Outputs. On CAPACITY they hold what fit before the buffers ran out. */
    uint32_t vertexCount;
    uint32_t indexCount;
} LaiueSurfaceMeshRequestV1;

/* LAIUE_SURFACE_MESH_OK, INVALID for a malformed request, or CAPACITY when a
 * vertex or index buffer was too small. */
LAIUE_MESHER_API uint32_t LaiueSurfaceMeshBuildV1(LaiueSurfaceMeshRequestV1 *request);
