#include "construct/physical_construct_persistence.h"
#include "construct/physical_construct_store.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

static const wchar_t g_worldPath[] = L"physical-persistence-world.dat";
static const wchar_t g_constructPath[] = L"physical-persistence-construct.dat";
static const wchar_t g_commitPath[] = L"physical-persistence-state.commit";
static const wchar_t g_worldSlot0[] = L"physical-persistence-world.dat.0";
static const wchar_t g_worldSlot1[] = L"physical-persistence-world.dat.1";
static const wchar_t g_constructSlot0[] = L"physical-persistence-construct.dat.0";
static const wchar_t g_constructSlot1[] = L"physical-persistence-construct.dat.1";
static const wchar_t g_commitSlot0[] = L"physical-persistence-state.commit.0";
static const wchar_t g_commitSlot1[] = L"physical-persistence-state.commit.1";

#define TEST_SEED 73421
#define TEST_BLOCK_X 11
#define TEST_BLOCK_Y -7
#define TEST_BLOCK_Z 500

static void Fail(const char* message)
{
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void RemoveTestFiles(void)
{
    PlatformDeleteFile(g_worldPath);
    PlatformDeleteFile(g_constructPath);
    PlatformDeleteFile(g_commitPath);
    PlatformDeleteFile(g_worldSlot0);
    PlatformDeleteFile(g_worldSlot1);
    PlatformDeleteFile(g_constructSlot0);
    PlatformDeleteFile(g_constructSlot1);
    PlatformDeleteFile(g_commitSlot0);
    PlatformDeleteFile(g_commitSlot1);
}

static bool AddInitialConstruct(PhysicalConstructSystem* constructs)
{
    PhysicalConstructBodyState state = {
        .id = 1U,
        .topologyRevision = 1U,
        .origin = {0.0, 0.0, 0.0},
        .velocity = {0.0, 0.0, 0.0},
        .blockCount = 1U,
    };
    PhysicalConstructBlock block = {
        .local = {0, 0, 0},
        .material = BLOCK_EARTH,
        .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
    };
    return PhysicalConstructImportBody(constructs, &state, &block, 1U) ==
        PHYSICAL_CONSTRUCT_OK;
}

static bool SetConstructOrigin(PhysicalConstructSystem* constructs,
                               double originX)
{
    PhysicalConstructBodyState state = {
        .id = 1U,
        .topologyRevision = 1U,
        .origin = {originX, 0.0, 0.0},
        .velocity = {0.0, 0.0, 0.0},
        .blockCount = 1U,
    };
    return PhysicalConstructApplyBodyMotion(constructs, &state);
}

static bool LoadedStateMatches(BlockType expectedBlock,
                               double expectedOriginX)
{
    World* world = WorldCreate(TEST_SEED);
    PhysicalConstructSystem* constructs =
        PhysicalConstructSystemCreate(world, NULL);
    if (world == NULL || constructs == NULL)
    {
        PhysicalConstructSystemDestroy(constructs);
        WorldDestroy(world);
        return false;
    }
    bool loaded = PhysicalConstructPersistenceLoad(
        world, constructs, g_worldPath, g_constructPath, g_commitPath);
    PhysicalConstructBodyState states[PHYSICAL_CONSTRUCT_MAX_BODIES];
    bool truncated = false;
    uint32_t count = PhysicalConstructCopyBodyStates(
        constructs, states, PHYSICAL_CONSTRUCT_MAX_BODIES, &truncated);
    bool matched = loaded && !truncated && count == 1U &&
        states[0].id == 1U && states[0].origin[0] == expectedOriginX &&
        WorldGetBlock(world, TEST_BLOCK_X, TEST_BLOCK_Y, TEST_BLOCK_Z) ==
            expectedBlock;
    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
    return matched;
}

static bool LoadMustFail(void)
{
    World* world = WorldCreate(TEST_SEED);
    PhysicalConstructSystem* constructs =
        PhysicalConstructSystemCreate(world, NULL);
    bool rejected = world != NULL && constructs != NULL &&
        !PhysicalConstructPersistenceLoad(
            world, constructs, g_worldPath,
            g_constructPath, g_commitPath);
    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
    return rejected;
}

LAIUE_TEST_ENTRY(PhysicalConstructPersistenceTestEntryPoint)
{
    RemoveTestFiles();
    World* world = WorldCreate(TEST_SEED);
    PhysicalConstructSystem* constructs =
        PhysicalConstructSystemCreate(world, NULL);
    if (world == NULL || constructs == NULL ||
        !AddInitialConstruct(constructs))
    {
        Fail("Не удалось подготовить persistence test");
    }
    WorldSetBlock(world, TEST_BLOCK_X, TEST_BLOCK_Y, TEST_BLOCK_Z,
                  BLOCK_GRASS);

    if (PhysicalConstructPersistenceSave(
            world, constructs, g_worldPath, g_worldPath, g_commitPath) ||
        PhysicalConstructPersistenceLoad(
            world, constructs, g_worldPath, g_worldPath, g_commitPath))
    {
        Fail("Persistence принял совпадающие data paths");
    }

    if (!PhysicalConstructPersistenceSave(
            world, constructs, g_worldPath,
            g_constructPath, g_commitPath) ||
        !LoadedStateMatches(BLOCK_GRASS, 0.0))
    {
        Fail("Первое committed поколение не загрузилось");
    }

    WorldSetBlock(world, TEST_BLOCK_X, TEST_BLOCK_Y, TEST_BLOCK_Z,
                  BLOCK_EARTH);
    if (!SetConstructOrigin(constructs, 8.25))
    {
        Fail("Не удалось изменить второе поколение");
    }

    // Crash after the first data-file replace but before the commit record:
    // the unpublished slot must be ignored as a unit.
    if (!WorldSaveDeltas(world, g_worldSlot1) ||
        !LoadedStateMatches(BLOCK_GRASS, 0.0))
    {
        Fail("Незавершённое поколение стало видимым");
    }

    if (!PhysicalConstructPersistenceSave(
            world, constructs, g_worldPath,
            g_constructPath, g_commitPath) ||
        !LoadedStateMatches(BLOCK_EARTH, 8.25))
    {
        Fail("Новейшее committed поколение не загрузилось");
    }

    const uint8_t damaged[] = {0x42U, 0x41U, 0x44U};
    if (!PlatformWriteEntireFile(g_worldSlot1, damaged, sizeof(damaged)) ||
        !LoadedStateMatches(BLOCK_GRASS, 0.0))
    {
        Fail("Не выполнен rollback на предыдущую целую пару");
    }

    // Once both committed data pairs are damaged, legacy unsuffixed files
    // must not be consulted: marker presence makes this a fail-closed store.
    if (!PlatformWriteEntireFile(g_worldSlot0, damaged, sizeof(damaged)) ||
        !LoadMustFail())
    {
        Fail("Persistence принял сохранение без целого поколения");
    }

    PhysicalConstructSystemDestroy(constructs);
    WorldDestroy(world);
    RemoveTestFiles();
    LAIUE_TEST_SUCCESS();
}
