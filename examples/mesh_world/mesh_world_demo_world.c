#include "mesh_world_demo_world.h"

#include "core/math/scalar.h"

#include <limits.h>
#include <string.h>

static uint32_t AddCell(int64_t left, int64_t right, int64_t *out)
{
    if ((right > 0 && left > INT64_MAX - right) || (right < 0 && left < INT64_MIN - right))
        return 0u;
    *out = left + right;
    return 1u;
}

static uint64_t CellHash(int64_t x, int64_t y)
{
    /* Unsigned arithmetic makes all signs and distant coordinates deterministic. */
    uint64_t value =
        (uint64_t)x * UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)y * UINT64_C(0xd1b54a32d192ed03);
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void Instance(LaiueMeshInstanceDescV1 *instance, uint32_t model)
{
    memset(instance, 0, sizeof(*instance));
    instance->structSize = sizeof(*instance);
    instance->model = model;
    instance->flags = LAIUE_MESH_INSTANCE_VISIBLE | LAIUE_MESH_INSTANCE_COLLIDABLE;
    instance->transform.rotation[3] = 1.0f;
    instance->transform.scale[0] = 1.0f;
    instance->transform.scale[1] = 1.0f;
    instance->transform.scale[2] = 1.0f;
}

uint32_t MeshDemoPopulate(void *context, const LaiueMeshCellV1 *cell,
                          LaiueMeshInstanceDescV1 *instances, uint32_t capacity, uint32_t *outCount)
{
    MeshDemoProvider *provider = (MeshDemoProvider *)context;
    if (provider == NULL || cell == NULL || outCount == NULL)
        return 0u;
    *outCount = 0u;
    int64_t absoluteX = 0, absoluteY = 0, absoluteZ = 0;
    if (!AddCell(provider->origin.x, cell->x, &absoluteX) ||
        !AddCell(provider->origin.y, cell->y, &absoluteY) ||
        !AddCell(provider->origin.z, cell->z, &absoluteZ))
        return 0u;
    /* The application chooses an infinite plane. Empty vertical cells are
     * successful provider results too; no special case depends on local z. */
    if (absoluteZ != 0 || provider->groundModel == 0u)
        return 1u;
    const uint32_t count = provider->treeModel == 0u ? 1u : 3u;
    if (instances == NULL || capacity < count)
        return 0u;
    Instance(&instances[0], provider->groundModel);
    uint64_t hash = CellHash(absoluteX, absoluteY);
    for (uint32_t index = 1u; index < count; ++index)
    {
        Instance(&instances[index], provider->treeModel);
        float *position = instances[index].transform.position.local;
        position[0] = index == 1u ? 4.0f : 11.0f;
        position[1] = 4.0f + (float)(hash & 7u);
        hash >>= 8;
        float angle = (float)(hash & 255u) * (6.28318530718f / 256.0f);
        instances[index].transform.rotation[2] = ScalarSin(angle * 0.5f);
        instances[index].transform.rotation[3] = ScalarCos(angle * 0.5f);
        const float scale = 0.85f + (float)((hash >> 8) & 15u) * 0.02f;
        instances[index].transform.scale[0] = scale;
        instances[index].transform.scale[1] = scale;
        instances[index].transform.scale[2] = scale;
        instances[index].userData = hash;
        hash = hash * UINT64_C(0x9e3779b97f4a7c15) + 1u;
    }
    *outCount = count;
    return 1u;
}

uint32_t MeshDemoRebase(const LaiueMeshWorldServiceV1 *service, LaiueMeshWorldV1 *world,
                        MeshDemoProvider *provider, LaiueMeshPositionV1 *player)
{
    if (service == NULL || world == NULL || provider == NULL || player == NULL)
        return 0u;
    const LaiueMeshCellV1 shift = player->cell;
    LaiueMeshCellV1 next = {0};
    if (!AddCell(provider->origin.x, shift.x, &next.x) ||
        !AddCell(provider->origin.y, shift.y, &next.y) ||
        !AddCell(provider->origin.z, shift.z, &next.z) || !service->rebase(world, &shift))
        return 0u;
    /* Single-threaded demo: streaming and adapter update are stopped here. */
    provider->origin = next;
    player->cell = (LaiueMeshCellV1){0, 0, 0};
    return 1u;
}

void MeshDemoCamera(float yaw, float pitch, float aspect, float matrix[16])
{
    /* Camera at render origin, looking along +Y with Z up at yaw=pitch=0.
     * Row-vector matrix and 0..1 depth match the public device convention. */
    const float sy = ScalarSin(yaw), cy = ScalarCos(yaw);
    const float sp = ScalarSin(pitch), cp = ScalarCos(pitch);
    const float right[3] = {cy, -sy, 0.0f};
    const float forward[3] = {sy * cp, cy * cp, sp};
    const float up[3] = {-sy * sp, -cy * sp, cp};
    const float focal = 1.0f / ScalarTan(0.5f * 1.134464f);
    const float depth = 160.0f / (160.0f - 0.05f);
    memset(matrix, 0, 16u * sizeof(float));
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        matrix[axis * 4u] = right[axis] * focal / aspect;
        matrix[axis * 4u + 1u] = up[axis] * focal;
        matrix[axis * 4u + 2u] = forward[axis] * depth;
        matrix[axis * 4u + 3u] = forward[axis];
    }
    matrix[14] = -0.05f * depth;
}
