// Мир на мешах через таблицу `laiue.mesh_world`: нормализация позиций,
// хэндлы с поколением, ревизии ячеек, запросы ячеек, sweep, скольжение,
// луч, блочный мост для физики, выборка коллайдеров, rebase и то, что
// результат не зависит от удалённости от начала координат.

#include "mesh_world/mesh_world_service.h"
#include "mod/module_host.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static const LaiueMeshWorldServiceV1 *g_service;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Mesh world check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool Near(float value, float expected, float tolerance)
{
    const float difference = value - expected;
    return difference <= tolerance && difference >= -tolerance;
}

enum
{
    MODEL_CUBE = 1u,
    MODEL_FLOOR = 2u,
    MODEL_TILTED = 3u,
    MODEL_EDGE = 4u,
    MODEL_LATE = 9u,
};

static LaiueMeshPositionV1 Position(int64_t x, int64_t y, int64_t z, float lx, float ly, float lz)
{
    LaiueMeshPositionV1 position = {{x, y, z}, {lx, ly, lz}};
    return position;
}

static LaiueMeshInstanceDescV1 Desc(uint32_t model, LaiueMeshPositionV1 position, float scale)
{
    LaiueMeshInstanceDescV1 desc = {0};
    desc.structSize = sizeof(desc);
    desc.model = model;
    desc.transform.position = position;
    desc.transform.rotation[3] = 1.0f;
    desc.transform.scale[0] = scale;
    desc.transform.scale[1] = scale;
    desc.transform.scale[2] = scale;
    desc.flags = LAIUE_MESH_INSTANCE_VISIBLE | LAIUE_MESH_INSTANCE_COLLIDABLE;
    return desc;
}

static LaiueMeshInstanceV1 Add(LaiueMeshWorldV1 *world, uint32_t model,
                               LaiueMeshPositionV1 position, float scale)
{
    const LaiueMeshInstanceDescV1 desc = Desc(model, position, scale);
    LaiueMeshInstanceV1 instance = 0u;
    Expect(g_service->add(world, &desc, &instance) != 0u && instance != 0u, "instance is added");
    return instance;
}

static void RegisterShapes(LaiueMeshWorldV1 *world)
{
    LaiueMeshShapeV1 cube = {0};
    cube.structSize = sizeof(cube);
    cube.flags = LAIUE_MESH_SHAPE_COLLIDE_BOUNDS;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        cube.boundsMin[axis] = -0.5f;
        cube.boundsMax[axis] = 0.5f;
    }
    Expect(g_service->registerShape(world, MODEL_CUBE, &cube) != 0u, "cube shape");

    // Пол 16×16 м, верх на z = 0: рисуемый габарит и одна коробка.
    static const LaiueMeshBoxV1 floorBox = {
        {0.0f, 0.0f, -0.25f}, {8.0f, 8.0f, 0.25f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    LaiueMeshShapeV1 floor = {0};
    floor.structSize = sizeof(floor);
    floor.boundsMin[0] = -8.0f;
    floor.boundsMin[1] = -8.0f;
    floor.boundsMin[2] = -0.5f;
    floor.boundsMax[0] = 8.0f;
    floor.boundsMax[1] = 8.0f;
    floor.boxes = &floorBox;
    floor.boxCount = 1u;
    Expect(g_service->registerShape(world, MODEL_FLOOR, &floor) != 0u, "floor shape");

    // Куб 2×2×2, повёрнутый на 45° вокруг Z: ребро смотрит на движущийся
    // ящик, и контакт находит только тест по наклонным осям.
    static const LaiueMeshBoxV1 tiltedBox = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.38268343f, 0.92387953f}};
    LaiueMeshShapeV1 tilted = {0};
    tilted.structSize = sizeof(tilted);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        tilted.boundsMin[axis] = -1.5f;
        tilted.boundsMax[axis] = 1.5f;
    }
    tilted.boxes = &tiltedBox;
    tilted.boxCount = 1u;
    Expect(g_service->registerShape(world, MODEL_TILTED, &tilted) != 0u, "tilted shape");

    // Куб, повёрнутый вокруг X и затем Z: рядом с блоком его разделяет
    // только ось из векторного произведения рёбер, а не грани.
    static const LaiueMeshBoxV1 edgeBox = {{0.0f, 0.0f, 0.0f},
                                           {0.5f, 0.5f, 0.5f},
                                           {0.35355339f, 0.14644661f, 0.35355339f, 0.85355339f}};
    LaiueMeshShapeV1 edge = {0};
    edge.structSize = sizeof(edge);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        edge.boundsMin[axis] = -0.9f;
        edge.boundsMax[axis] = 0.9f;
    }
    edge.boxes = &edgeBox;
    edge.boxCount = 1u;
    Expect(g_service->registerShape(world, MODEL_EDGE, &edge) != 0u, "edge shape");

    LaiueMeshShapeV1 invalid = cube;
    invalid.boundsMin[0] = 1.0f;
    Expect(g_service->registerShape(world, 77u, &invalid) == 0u, "inverted bounds are refused");
    invalid = floor;
    invalid.boxCount = LAIUE_MESH_WORLD_MAX_SHAPE_BOXES + 1u;
    Expect(g_service->registerShape(world, 77u, &invalid) == 0u, "too many boxes are refused");
}

static void CheckCreate(void)
{
    LaiueMeshWorldConfigV1 config = {sizeof(config), 0.5f, 0u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(g_service->create(&config, &world) == 0u && world == NULL, "tiny cells are refused");
    config.cellSize = 8192.0f;
    Expect(g_service->create(&config, &world) == 0u, "huge cells are refused");
    config.cellSize = 16.0f;
    config.structSize = 4u;
    Expect(g_service->create(&config, &world) == 0u, "truncated config is refused");
}

static void CheckPlacement(LaiueMeshWorldV1 *world)
{
    // Смещение вне ячейки нормализуется в [0, cellSize).
    const LaiueMeshInstanceV1 a =
        Add(world, MODEL_CUBE, Position(0, 0, 0, 20.0f, -1.0f, 3.0f), 1.0f);
    LaiueMeshInstanceInfoV1 info;
    Expect(g_service->get(world, a, &info) != 0u && info.handle == a && info.model == MODEL_CUBE,
           "instance reads back");
    Expect(info.transform.position.cell.x == 1 && info.transform.position.cell.y == -1 &&
               info.transform.position.cell.z == 0 &&
               Near(info.transform.position.local[0], 4.0f, 1e-5f) &&
               Near(info.transform.position.local[1], 15.0f, 1e-5f) &&
               Near(info.transform.position.local[2], 3.0f, 1e-5f),
           "position is normalized into its cell");

    LaiueMeshInstanceDescV1 bad = Desc(MODEL_CUBE, Position(0, 0, 0, 0.0f, 0.0f, 0.0f), 1.0f);
    LaiueMeshInstanceV1 refused = 0u;
    bad.transform.rotation[3] = 0.0f;
    Expect(g_service->add(world, &bad, &refused) == 0u && refused == 0u,
           "zero rotation is refused");
    bad = Desc(MODEL_CUBE, Position(0, 0, 0, 0.0f, 0.0f, 0.0f), 0.0f);
    Expect(g_service->add(world, &bad, &refused) == 0u, "zero scale is refused");
    bad = Desc(MODEL_CUBE, Position(LAIUE_MESH_WORLD_MAX_CELL, 0, 0, 0.0f, 0.0f, 0.0f), 1.0f);
    bad.transform.position.local[0] = 32.0f;
    Expect(g_service->add(world, &bad, &refused) == 0u, "cell beyond the limit is refused");
    bad = Desc(MODEL_CUBE, Position(0, 0, 0, 0.0f, 0.0f, 0.0f), 1.0f);
    bad.flags = 1u << 7;
    Expect(g_service->add(world, &bad, &refused) == 0u, "unknown flags are refused");

    // Ревизии: 0 у пустой ячейки, новые значения на каждое изменение.
    const LaiueMeshCellV1 cell = {1, -1, 0};
    const uint64_t first = g_service->cellRevision(world, &cell);
    Expect(first != 0u, "occupied cell has a revision");
    const LaiueMeshInstanceV1 b =
        Add(world, MODEL_CUBE, Position(1, -1, 0, 8.0f, 8.0f, 1.0f), 1.0f);
    const uint64_t second = g_service->cellRevision(world, &cell);
    Expect(second > first, "adding changes the revision");
    Expect(g_service->setFlags(world, b, LAIUE_MESH_INSTANCE_VISIBLE) != 0u, "flags change");
    const uint64_t third = g_service->cellRevision(world, &cell);
    Expect(third > second, "flags change the revision");

    LaiueMeshInstanceInfoV1 listed[2];
    uint32_t count = 0u;
    Expect(g_service->cellInstances(world, &cell, listed, 1u, &count) != 0u && count == 2u &&
               listed[0].handle == a,
           "cell lists instances in insertion order with the full count");

    // Переезд в другую ячейку очищает старую.
    LaiueMeshTransformV1 moved = info.transform;
    moved.position = Position(5, 5, 5, 1.0f, 1.0f, 1.0f);
    Expect(g_service->setTransform(world, a, &moved) != 0u, "instance moves between cells");
    Expect(g_service->cellInstances(world, &cell, listed, 2u, &count) != 0u && count == 1u &&
               listed[0].handle == b,
           "old cell keeps the other instance");
    const LaiueMeshCellV1 target = {5, 5, 5};
    Expect(g_service->cellRevision(world, &target) > third, "new cell gets a fresh revision");

    // Старый хэндл не адресует новый экземпляр в том же слоте.
    Expect(g_service->remove(world, b) != 0u, "instance is removed");
    Expect(g_service->cellRevision(world, &cell) == 0u, "emptied cell reports no revision");
    Expect(g_service->get(world, b, &info) == 0u && g_service->remove(world, b) == 0u &&
               g_service->setTransform(world, b, &moved) == 0u,
           "stale handle is refused");
    const LaiueMeshInstanceV1 c =
        Add(world, MODEL_CUBE, Position(1, -1, 0, 2.0f, 2.0f, 2.0f), 1.0f);
    Expect(c != b && g_service->get(world, b, &info) == 0u && g_service->get(world, c, &info) != 0u,
           "reused slot gets a new handle");
    Expect(g_service->instanceCount(world) == 2u, "instance count");
    Expect(g_service->remove(world, a) != 0u && g_service->remove(world, c) != 0u &&
               g_service->instanceCount(world) == 0u,
           "world empties");
}

static void CheckCollision(LaiueMeshWorldV1 *world, int64_t baseX, int64_t baseY, int64_t baseZ)
{
    // Пол в центре ячейки base, стена-куб на x = 11, наклонный куб на y = 8.
    const LaiueMeshInstanceV1 floor =
        Add(world, MODEL_FLOOR, Position(baseX, baseY, baseZ, 8.0f, 8.0f, 4.0f), 1.0f);
    const LaiueMeshInstanceV1 wall =
        Add(world, MODEL_CUBE, Position(baseX, baseY, baseZ, 11.0f, 8.0f, 5.0f), 1.0f);
    const LaiueMeshInstanceV1 tilted =
        Add(world, MODEL_TILTED, Position(baseX, baseY, baseZ, 8.0f, 13.0f, 5.0f), 1.0f);
    const float half[3] = {0.4f, 0.4f, 0.9f};

    LaiueMeshHitV1 hit;
    LaiueMeshPositionV1 start = Position(baseX, baseY, baseZ, 8.0f, 8.0f, 6.0f);
    const float down[3] = {0.0f, 0.0f, -3.0f};
    Expect(g_service->sweepBox(world, &start, half, down, &hit) != 0u && hit.instance == floor &&
               Near(hit.time, (6.0f - 0.9f - 4.0f) / 3.0f, 1e-4f) &&
               Near(hit.normal[2], 1.0f, 1e-5f),
           "falling box hits the floor top");

    const float across[3] = {5.0f, 0.0f, 0.0f};
    start = Position(baseX, baseY, baseZ, 8.0f, 8.0f, 5.0f);
    Expect(g_service->sweepBox(world, &start, half, across, &hit) != 0u && hit.instance == wall &&
               Near(hit.time, (11.0f - 0.5f - 0.4f - 8.0f) / 5.0f, 1e-4f) &&
               Near(hit.normal[0], -1.0f, 1e-5f),
           "sideways box hits the wall face");

    // Угол повёрнутого куба на расстоянии √2 от центра.
    const float smallHalf[3] = {0.25f, 0.25f, 0.25f};
    const float north[3] = {0.0f, 10.0f, 0.0f};
    start = Position(baseX, baseY, baseZ, 8.0f, 8.0f, 5.0f);
    Expect(g_service->sweepBox(world, &start, smallHalf, north, &hit) != 0u && hit.instance == tilted &&
               Near(hit.time, (5.0f - 1.41421356f - 0.25f) / 10.0f, 1e-4f) &&
               Near(hit.normal[1], -1.0f, 1e-4f),
           "box hits the edge of the rotated box");

    const float sideways[3] = {0.0f, -3.0f, 0.0f};
    start = Position(baseX, baseY, baseZ, 8.0f, 8.0f, 4.0f + 0.9f + 0.01f);
    Expect(g_service->sweepBox(world, &start, half, sideways, &hit) == 0u,
           "box gliding above the floor touches nothing");

    // Падение с проскальзыванием: горизонталь сохраняется, ящик стоит на полу.
    LaiueMeshMoveResultV1 move;
    const float fallAndSlide[3] = {-2.0f, 0.0f, -4.0f};
    start = Position(baseX, baseY, baseZ, 8.0f, 8.0f, 6.0f);
    Expect(g_service->moveBox(world, &start, half, fallAndSlide, &move) != 0u &&
               move.grounded != 0u && move.collided != 0u,
           "falling box lands");
    Expect(Near(move.position.local[2], 4.9f, 2e-3f) && Near(move.position.local[0], 6.0f, 2e-3f),
           "landing keeps the horizontal motion");
    Expect(move.position.local[2] > 4.9f + 5e-4f, "the box rests a skin above the floor");
    const float drift[3] = {-1.0f, 0.0f, 0.0f};
    Expect(g_service->moveBox(world, &move.position, half, drift, &move) != 0u &&
               move.collided == 0u && Near(move.position.local[0], 5.0f, 2e-3f),
           "free motion is applied as is");

    // Ящик, частично вошедший в стену, выходит из неё свободно, а вглубь
    // упирается сразу.
    start = Position(baseX, baseY, baseZ, 10.4f, 8.0f, 5.0f);
    const float back[3] = {-1.0f, 0.0f, 0.0f};
    const float deeper[3] = {1.0f, 0.0f, 0.0f};
    Expect(g_service->moveBox(world, &start, smallHalf, back, &move) != 0u && move.collided == 0u &&
               Near(move.position.local[0], 9.4f, 1e-4f),
           "an overlapping box can leave");
    Expect(g_service->sweepBox(world, &start, smallHalf, deeper, &hit) != 0u && hit.instance == wall &&
               hit.time == 0.0f && Near(hit.normal[0], -1.0f, 1e-5f),
           "an overlapping box cannot go deeper");

    // Луч сверху попадает в верх пола; луч изнутри коробки её не видит.
    const LaiueMeshPositionV1 eye = Position(baseX, baseY, baseZ, 3.0f, 3.0f, 9.0f);
    const float ray[3] = {0.0f, 0.0f, -2.0f};
    Expect(g_service->raycast(world, &eye, ray, 10.0f, &hit) != 0u && hit.instance == floor &&
               Near(hit.time, 0.5f, 1e-4f) && Near(hit.normal[2], 1.0f, 1e-5f),
           "ray hits the floor");
    const LaiueMeshPositionV1 inside = Position(baseX, baseY, baseZ, 11.0f, 8.0f, 5.0f);
    const float east[3] = {1.0f, 0.0f, 0.0f};
    Expect(g_service->raycast(world, &inside, east, 3.0f, &hit) == 0u,
           "ray leaving a box does not hit it");

    // Выборка коллайдеров вокруг стены — в системе отсчёта запроса.
    LaiueMeshColliderV1 colliders[4];
    uint32_t count = 0u;
    const LaiueMeshPositionV1 reference = Position(baseX, baseY, baseZ, 10.0f, 8.0f, 5.0f);
    const float minimum[3] = {-0.25f, -0.25f, -0.25f};
    const float maximum[3] = {0.75f, 0.25f, 0.25f};
    Expect(g_service->overlapBoxes(world, &reference, minimum, maximum, colliders, 4u, &count) !=
                   0u &&
               count == 1u && colliders[0].instance == wall &&
               Near(colliders[0].minimum[0], 0.5f, 1e-4f) &&
               Near(colliders[0].maximum[0], 1.5f, 1e-4f) &&
               Near(colliders[0].minimum[2], -0.5f, 1e-4f),
           "overlap returns the wall bounds relative to the reference");

    Expect(g_service->remove(world, floor) != 0u && g_service->remove(world, wall) != 0u &&
               g_service->remove(world, tilted) != 0u,
           "collision fixtures are removed");
}

static void CheckBlocksAndCells(LaiueMeshWorldV1 *world)
{
    // Пол в ячейке (0,0,0) с верхом на z = 0: блок под ним твёрд, блок над
    // ним только касается и пуст.
    const LaiueMeshInstanceV1 floor =
        Add(world, MODEL_FLOOR, Position(0, 0, 0, 8.0f, 8.0f, 0.0f), 1.0f);
    Expect(g_service->blockSolid(world, 3, 3, -1, 1.0f) != 0u,
           "block under the floor top is solid");
    Expect(g_service->blockSolid(world, 3, 3, 0, 1.0f) == 0u,
           "block resting on the floor is empty");
    Expect(g_service->blockSolid(world, 300, 3, -1, 1.0f) == 0u, "far block is empty");
    Expect(g_service->blockSolid(world, -1, 3, -1, 1.0f) == 0u, "block past the edge is empty");

    // Блок 1000³ и куб рядом: все оси граней пересекаются, разделяет только
    // ось ребро×ребро (зазор 0.18 м). Блок с центром куба твёрд.
    const LaiueMeshInstanceV1 edge =
        Add(world, MODEL_EDGE, Position(62, 62, 62, 9.6416911f, 7.3815018f, 8.5299993f), 1.0f);
    Expect(g_service->blockSolid(world, 1000, 1000, 1000, 1.0f) == 0u,
           "an edge-edge separating axis keeps the block empty");
    Expect(g_service->blockSolid(world, 1001, 999, 1000, 1.0f) != 0u,
           "the block holding the cube centre is solid");
    Expect(g_service->remove(world, edge) != 0u, "edge fixture is removed");

    // Большой экземпляр выступает далеко за свою ячейку и всё равно виден.
    const LaiueMeshInstanceV1 big =
        Add(world, MODEL_FLOOR, Position(10, 0, 0, 8.0f, 8.0f, 0.0f), 4.0f);
    LaiueMeshHitV1 hit;
    const LaiueMeshPositionV1 above = Position(12, 0, 0, 2.0f, 8.0f, 3.0f);
    const float down[3] = {0.0f, 0.0f, -4.0f};
    const float half[3] = {0.5f, 0.5f, 0.5f};
    Expect(g_service->sweepBox(world, &above, half, down, &hit) != 0u && hit.instance == big,
           "a scaled instance is found from a neighbouring cell");

    const LaiueMeshInstanceV1 c1 =
        Add(world, MODEL_CUBE, Position(0, 2, 0, 1.0f, 1.0f, 1.0f), 1.0f);
    const LaiueMeshInstanceV1 c2 =
        Add(world, MODEL_CUBE, Position(0, -2, 0, 1.0f, 1.0f, 1.0f), 1.0f);
    LaiueMeshCellV1 cells[8];
    uint32_t count = 0u;
    const LaiueMeshPositionV1 center = Position(0, 0, 0, 8.0f, 8.0f, 0.0f);
    Expect(g_service->queryCells(world, &center, 40.0f, cells, 8u, &count) != 0u && count == 3u,
           "cells within the radius");
    Expect(cells[0].y == -2 && cells[1].y == 0 && cells[2].y == 2, "cells are sorted");
    Expect(g_service->queryCells(world, &center, 40.0f, cells, 2u, &count) != 0u && count == 3u &&
               cells[0].y == -2 && cells[1].y == 0,
           "a truncated answer is the sorted prefix");
    Expect(g_service->queryCells(world, &center, 4.0f, cells, 8u, &count) != 0u && count == 1u,
           "a smallHalf radius sees only the local cell");
    // Масштабированный пол из ячейки 10 дотягивается до x = 10*16+8-32 = 136,
    // в 128 м от центра запроса.
    Expect(g_service->queryCells(world, &center, 120.0f, cells, 8u, &count) != 0u && count == 3u,
           "a large instance beyond the radius is not reported");
    Expect(g_service->queryCells(world, &center, 130.0f, cells, 8u, &count) != 0u && count == 4u &&
               cells[3].x == 10,
           "a large instance is found through its bounds");

    // Поздняя регистрация формы меняет габариты уже размещённых экземпляров.
    const LaiueMeshInstanceV1 late =
        Add(world, MODEL_LATE, Position(0, 0, 0, 2.0f, 2.0f, 2.0f), 1.0f);
    const LaiueMeshPositionV1 overLate = Position(0, 0, 0, 2.0f, 2.0f, 5.0f);
    Expect(g_service->sweepBox(world, &overLate, half, down, &hit) == 0u,
           "an instance without a shape does not collide");
    const LaiueMeshCellV1 home = {0, 0, 0};
    const uint64_t before = g_service->cellRevision(world, &home);
    LaiueMeshShapeV1 shape = {0};
    shape.structSize = sizeof(shape);
    shape.flags = LAIUE_MESH_SHAPE_COLLIDE_BOUNDS;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        shape.boundsMin[axis] = -1.0f;
        shape.boundsMax[axis] = 1.0f;
    }
    Expect(g_service->registerShape(world, MODEL_LATE, &shape) != 0u, "late shape");
    Expect(g_service->cellRevision(world, &home) > before, "a new shape changes the revision");
    Expect(g_service->sweepBox(world, &overLate, half, down, &hit) != 0u && hit.instance == late &&
               Near(hit.time, (5.0f - 0.5f - 3.0f) / 4.0f, 1e-4f),
           "the new shape collides");

    // Rebase: координаты ячеек сдвигаются, ревизии и абсолютные позиции нет.
    const LaiueMeshCellV1 bigCell = {10, 0, 0};
    const uint64_t bigRevision = g_service->cellRevision(world, &bigCell);
    const LaiueMeshCellV1 shift = {10, 0, 0};
    Expect(g_service->rebase(world, &shift) != 0u, "rebase");
    const LaiueMeshCellV1 shifted = {0, 0, 0};
    Expect(g_service->cellRevision(world, &shifted) == bigRevision, "revision survives rebase");
    LaiueMeshInstanceInfoV1 info;
    Expect(g_service->get(world, big, &info) != 0u && info.transform.position.cell.x == 0,
           "instances follow the rebase");
    const LaiueMeshPositionV1 aboveShifted = Position(2, 0, 0, 2.0f, 8.0f, 3.0f);
    Expect(g_service->sweepBox(world, &aboveShifted, half, down, &hit) != 0u && hit.instance == big,
           "queries work in the shifted frame");
    const LaiueMeshCellV1 tooFar = {0, -LAIUE_MESH_WORLD_MAX_CELL, 0};
    Expect(g_service->rebase(world, &tooFar) == 0u, "a rebase past the limit is refused");
    Expect(g_service->get(world, big, &info) != 0u && info.transform.position.cell.x == 0,
           "a refused rebase changes nothing");

    Expect(g_service->remove(world, floor) != 0u && g_service->remove(world, big) != 0u &&
               g_service->remove(world, c1) != 0u && g_service->remove(world, c2) != 0u &&
               g_service->remove(world, late) != 0u && g_service->instanceCount(world) == 0u,
           "block fixtures are removed");
}

// Много ячеек и удаление вперемешку: таблица ячеек после сдвигов при
// удалении обязана находить каждую оставшуюся ячейку.
static void CheckChurn(LaiueMeshWorldV1 *world)
{
    enum
    {
        CHURN_COUNT = 4096u,
    };
    static LaiueMeshInstanceV1 handles[CHURN_COUNT];
    for (uint32_t i = 0u; i < CHURN_COUNT; ++i)
    {
        const int64_t x = (int64_t)(i % 64u) - 32;
        const int64_t y = (int64_t)(i / 64u) - 32;
        handles[i] =
            Add(world, MODEL_CUBE, Position(x, y, (int64_t)(i % 3u), 1.0f, 1.0f, 1.0f), 1.0f);
    }
    for (uint32_t i = 0u; i < CHURN_COUNT; i += 2u)
        Expect(g_service->remove(world, handles[i]) != 0u, "churn removal");
    Expect(g_service->instanceCount(world) == CHURN_COUNT / 2u, "churn count");
    for (uint32_t i = 0u; i < CHURN_COUNT; ++i)
    {
        const LaiueMeshCellV1 cell = {(int64_t)(i % 64u) - 32, (int64_t)(i / 64u) - 32,
                                      (int64_t)(i % 3u)};
        LaiueMeshInstanceInfoV1 info;
        uint32_t count = 0u;
        Expect(g_service->cellInstances(world, &cell, &info, 1u, &count) != 0u, "churn cell query");
        Expect(count == ((i % 2u) != 0u ? 1u : 0u), "every surviving cell is found");
        Expect((i % 2u) == 0u || info.handle == handles[i], "surviving cell holds its instance");
    }
    for (uint32_t i = 1u; i < CHURN_COUNT; i += 2u)
        Expect(g_service->remove(world, handles[i]) != 0u, "churn cleanup");
    Expect(g_service->instanceCount(world) == 0u, "churn empties");
}

// Поставщик ячеек: плитка земли 16×16 м в каждой ячейке с z = 0. Мир сам
// наполняет ячейки вокруг точки и убирает дальние, поэтому идти можно по
// любой оси сколько угодно.
typedef struct GroundProvider
{
    uint32_t calls;
    LaiueMeshCellV1 failCell;
    bool failEnabled;
} GroundProvider;

static uint32_t PopulateGround(void *context, const LaiueMeshCellV1 *cell,
                               LaiueMeshInstanceDescV1 *outInstances, uint32_t capacity,
                               uint32_t *outCount)
{
    GroundProvider *provider = (GroundProvider *)context;
    ++provider->calls;
    *outCount = 0u;
    if (provider->failEnabled && cell->x == provider->failCell.x &&
        cell->y == provider->failCell.y && cell->z == provider->failCell.z)
        return 0u;
    if (cell->z != 0 || capacity < 2u)
        return 1u;
    outInstances[0] = Desc(MODEL_FLOOR, Position(0, 0, 0, 8.0f, 8.0f, 0.0f), 1.0f);
    // Второй экземпляр выходит за ячейку соседа: смещение считается от
    // наполняемой ячейки и нормализуется.
    outInstances[1] = Desc(MODEL_CUBE, Position(0, 0, 0, 20.0f, 8.0f, 0.5f), 1.0f);
    outInstances[1].flags = LAIUE_MESH_INSTANCE_VISIBLE;
    *outCount = 2u;
    return 1u;
}

static void CheckProvider(LaiueMeshWorldV1 *world)
{
    GroundProvider ground = {0};
    LaiueMeshCellProviderV1 provider = {sizeof(provider), 8u, &ground, PopulateGround};
    // Экземпляр приложения переживает смену и снятие поставщика.
    const LaiueMeshInstanceV1 own =
        Add(world, MODEL_CUBE, Position(0, 0, 3, 1.0f, 1.0f, 1.0f), 1.0f);
    Expect(g_service->setProvider(world, &provider) != 0u, "provider installs");
    LaiueMeshCellProviderV1 invalid = provider;
    invalid.maximumInstancesPerCell = 0u;
    Expect(g_service->setProvider(world, &invalid) == 0u, "provider without capacity is refused");

    uint32_t pending = 99u;
    const LaiueMeshPositionV1 start = Position(0, 0, 0, 8.0f, 8.0f, 1.0f);
    Expect(g_service->stream(world, &start, 40.0f, 0u, &pending) != 0u && pending == 0u,
           "stream fills the range");
    // 275 ячеек в радиусе, из них 45 на уровне земли по два экземпляра.
    Expect(ground.calls == 275u, "every cell in range is asked once");
    Expect(g_service->instanceCount(world) == 1u + 90u, "ground instances are placed");
    Expect(g_service->stream(world, &start, 40.0f, 0u, &pending) != 0u && ground.calls == 275u,
           "a repeated stream from the same cell asks nothing");

    // Идём по X на 100 ячеек: старые ячейки уходят, новые приходят, по
    // земле можно стоять.
    const LaiueMeshPositionV1 distant = Position(100, 0, 0, 8.0f, 8.0f, 1.0f);
    Expect(g_service->stream(world, &distant, 40.0f, 0u, &pending) != 0u && pending == 0u,
           "stream follows the walker");
    Expect(g_service->instanceCount(world) == 1u + 90u, "the window keeps its size");
    LaiueMeshHitV1 hit;
    const float half[3] = {0.4f, 0.4f, 0.9f};
    const float down[3] = {0.0f, 0.0f, -2.0f};
    const LaiueMeshPositionV1 standing = Position(100, 0, 0, 3.0f, 3.0f, 1.5f);
    Expect(g_service->sweepBox(world, &standing, half, down, &hit) != 0u &&
               Near(hit.normal[2], 1.0f, 1e-5f),
           "the streamed ground holds the walker");
    const LaiueMeshCellV1 oldCell = {0, 0, 0};
    Expect(g_service->cellRevision(world, &oldCell) == 0u, "cells left behind are emptied");

    // Rebase на 100 ячеек: то же место в новых координатах, поставщик не
    // спрашивается заново.
    const uint32_t callsBeforeRebase = ground.calls;
    const LaiueMeshCellV1 shift = {100, 0, 0};
    Expect(g_service->rebase(world, &shift) != 0u, "provider world rebases");
    const LaiueMeshPositionV1 here = Position(0, 0, 0, 8.0f, 8.0f, 1.0f);
    Expect(g_service->stream(world, &here, 40.0f, 0u, &pending) != 0u &&
               ground.calls == callsBeforeRebase && g_service->instanceCount(world) == 1u + 90u,
           "populated cells follow the rebase");

    // Вверх по Z: земля остаётся внизу, наверху поставщик ничего не ставит.
    const LaiueMeshPositionV1 high = Position(0, 0, 1000, 8.0f, 8.0f, 1.0f);
    Expect(g_service->stream(world, &high, 40.0f, 0u, &pending) != 0u &&
               g_service->instanceCount(world) == 1u,
           "climbing far up leaves only the application instance");

    // Бюджет растягивает наполнение на несколько вызовов.
    Expect(g_service->setProvider(world, &provider) != 0u, "provider reinstalls");
    ground.calls = 0u;
    uint32_t rounds = 0u;
    do
    {
        Expect(g_service->stream(world, &here, 40.0f, 50u, &pending) != 0u, "budgeted stream");
        ++rounds;
    } while (pending != 0u && rounds < 100u);
    Expect(rounds == 6u && ground.calls == 275u && g_service->instanceCount(world) == 1u + 90u,
           "a budget of 50 cells fills 275 cells in six calls");

    // Отказ поставщика не оставляет полуготовую ячейку.
    Expect(g_service->setProvider(world, &provider) != 0u, "provider resets");
    ground.failEnabled = true;
    ground.failCell.x = 0;
    ground.failCell.y = 0;
    ground.failCell.z = 0;
    Expect(g_service->stream(world, &here, 40.0f, 0u, &pending) == 0u &&
               g_service->instanceCount(world) == 1u,
           "a failing provider stops the stream");
    ground.failEnabled = false;
    Expect(g_service->stream(world, &here, 40.0f, 0u, &pending) != 0u &&
               g_service->instanceCount(world) == 1u + 90u,
           "the failed cell is retried");

    // Экземпляр поставщика, удалённый приложением, не мешает уборке.
    const LaiueMeshCellV1 home = {0, 0, 0};
    LaiueMeshInstanceInfoV1 info;
    uint32_t count = 0u;
    Expect(g_service->cellInstances(world, &home, &info, 1u, &count) != 0u && count != 0u &&
               g_service->remove(world, info.handle) != 0u,
           "application removes a provider instance");
    Expect(g_service->setProvider(world, NULL) != 0u && g_service->instanceCount(world) == 1u,
           "removing the provider removes only its instances");
    Expect(g_service->stream(world, &here, 40.0f, 0u, &pending) != 0u && pending == 0u,
           "stream without a provider succeeds");
    Expect(g_service->remove(world, own) != 0u, "application instance is removed");
}

static void CheckModuleTable(void)
{
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "module host");
    const LaiueModuleBinaryV1 binary = {NULL, LAIUE_MODULE_BINARY_STATIC,
                                        LaiueMeshWorldGetStaticModuleApiV1()};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);
    const void *table = LaiueModuleHostQueryService(host, LAIUE_MESH_WORLD_SERVICE_NAME,
                                                    LAIUE_MESH_WORLD_SERVICE_ABI_VERSION_1,
                                                    sizeof(LaiueMeshWorldServiceV1), NULL, NULL);
    Expect(table == LaiueMeshWorldGetStaticServiceV1(), "module publishes the service table");
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);
}

LAIUE_TEST_ENTRY(MeshWorldTestEntryPoint)
{
    g_service = LaiueMeshWorldGetStaticServiceV1();
    Expect(g_service != NULL && g_service->structSize == sizeof(LaiueMeshWorldServiceV1),
           "service table");
    CheckCreate();
    CheckModuleTable();

    LaiueMeshWorldConfigV1 config = {sizeof(config), 16.0f, 0u};
    LaiueMeshWorldV1 *world = NULL;
    Expect(g_service->create(&config, &world) != 0u && world != NULL, "world creates");
    Expect(g_service->cellSize(world) == 16.0f, "cell size");
    RegisterShapes(world);
    CheckPlacement(world);
    CheckCollision(world, 0, 0, 0);
    // Тот же сценарий за 2^60 ячеек от начала: точность не зависит от
    // удалённости, потому что считаются только разности ячеек.
    CheckCollision(world, INT64_C(1) << 60, -(INT64_C(1) << 60), INT64_C(123456789012));
    CheckBlocksAndCells(world);
    CheckChurn(world);
    CheckProvider(world);
    g_service->destroy(world);

    LaiueTestRuntimeWrite("Mesh world checks passed\r\n");
    LAIUE_TEST_SUCCESS();
}
