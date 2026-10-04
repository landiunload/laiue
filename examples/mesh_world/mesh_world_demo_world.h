#pragma once

#include "mesh_world/mesh_world_service.h"

enum
{
    MESH_DEMO_MODEL_GROUND = 1,
    MESH_DEMO_MODEL_TREE = 2,
    MESH_DEMO_CELL_SIZE = 16,
};

typedef struct MeshDemoProvider
{
    /* Absolute cell origin; updated only after the world's successful rebase. */
    LaiueMeshCellV1 origin;
    uint32_t groundModel;
    uint32_t treeModel;
} MeshDemoProvider;

uint32_t MeshDemoPopulate(void *context, const LaiueMeshCellV1 *cell,
                          LaiueMeshInstanceDescV1 *instances, uint32_t capacity,
                          uint32_t *outCount);
uint32_t MeshDemoRebase(const LaiueMeshWorldServiceV1 *service, LaiueMeshWorldV1 *world,
                        MeshDemoProvider *provider, LaiueMeshPositionV1 *player);
void MeshDemoCamera(float yaw, float pitch, float aspect, float matrix[16]);
