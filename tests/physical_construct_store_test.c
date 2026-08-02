#include "construct/physical_construct_store.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

static const wchar_t g_storePathA[] = L"physical-construct-store-a.dat";
static const wchar_t g_storePathB[] = L"physical-construct-store-b.dat";
static const wchar_t g_storePathBad[] = L"physical-construct-store-bad.dat";

static void Fail(const char *message)
{
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool BytesEqual(const uint8_t *left, const uint8_t *right, uint64_t size)
{
    for (uint64_t index = 0U; index < size; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }
    return true;
}

static void RemoveTestFiles(void)
{
    PlatformDeleteFile(g_storePathA);
    PlatformDeleteFile(g_storePathB);
    PlatformDeleteFile(g_storePathBad);
}

static bool AddBodies(PhysicalConstructSystem *system)
{
    // Intentionally import IDs and block coordinates out of order. The store
    // must emit a canonical body-ID/local-coordinate order.
    PhysicalConstructBodyState second = {
        .id = 9U,
        .topologyRevision = 4U,
        .origin = {120.25, -8.5, 42.0},
        .velocity = {-1.0, 2.0, 0.5},
        .blockCount = 2U,
    };
    PhysicalConstructBlock secondBlocks[2] = {
        {
            .local = {4, 3, -2},
            .material = BLOCK_GRASS,
            .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
        },
        {
            .local = {3, 3, -2},
            .material = BLOCK_EARTH,
            .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
        },
    };
    if (PhysicalConstructImportBody(system, &second, secondBlocks, 2U) != PHYSICAL_CONSTRUCT_OK)
    {
        return false;
    }

    PhysicalConstructBodyState first = {
        .id = 2U,
        .topologyRevision = 7U,
        .origin = {-0.0, 7.75, -11.0},
        .velocity = {0.0, -0.0, 3.25},
        .blockCount = 3U,
        .grabOwner = 55U,
        .grabbed = true,
    };
    PhysicalConstructBlock firstBlocks[3] = {
        {
            .local = {-2, 0, 0},
            .material = BLOCK_GRASS,
            .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
        },
        {
            .local = {0, 0, 0},
            .mountNormal = {1, 0, 0},
            .material = BLOCK_AIR,
            .kind = PHYSICAL_CONSTRUCT_BLOCK_LEVER,
        },
        {
            .local = {-1, 0, 0},
            .material = BLOCK_EARTH,
            .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
        },
    };
    return PhysicalConstructImportBody(system, &first, firstBlocks, 3U) == PHYSICAL_CONSTRUCT_OK;
}

static bool LoadedStateValid(PhysicalConstructSystem *system)
{
    PhysicalConstructBodyState states[PHYSICAL_CONSTRUCT_MAX_BODIES];
    bool truncated = false;
    uint32_t count =
        PhysicalConstructCopyBodyStates(system, states, PHYSICAL_CONSTRUCT_MAX_BODIES, &truncated);
    if (truncated || count != 2U || states[0].id != 2U || states[1].id != 9U ||
        states[0].topologyRevision != 7U || states[1].topologyRevision != 4U || states[0].grabbed ||
        states[0].grabOwner != 0U || states[0].origin[0] != 0.0 || states[0].origin[1] != 7.75 ||
        states[0].velocity[2] != 3.25 || states[1].origin[0] != 120.25)
    {
        return false;
    }
    PhysicalConstructBlock blocks[3];
    uint32_t blockCount = 0U;
    return PhysicalConstructCopyBlocks(system, 2U, blocks, 3U, &blockCount) && blockCount == 3U &&
           blocks[0].local[0] == -2 && blocks[1].local[0] == -1 &&
           blocks[2].kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER;
}

LAIUE_TEST_ENTRY(PhysicalConstructStoreTestEntryPoint)
{
    RemoveTestFiles();
    World *firstWorld = WorldCreate(12345);
    World *loadedWorld = WorldCreate(12345);
    PhysicalConstructSystem *first = PhysicalConstructSystemCreate(firstWorld, NULL);
    PhysicalConstructSystem *loaded = PhysicalConstructSystemCreate(loadedWorld, NULL);
    if (firstWorld == NULL || loadedWorld == NULL || first == NULL || loaded == NULL ||
        !AddBodies(first))
    {
        Fail("Не удалось подготовить construct store test");
    }
    if (!PhysicalConstructStoreSave(first, g_storePathA) ||
        !PhysicalConstructStoreLoad(loaded, g_storePathA) || !LoadedStateValid(loaded) ||
        !PhysicalConstructStoreSave(loaded, g_storePathB))
    {
        Fail("Construct store round-trip потерял состояние");
    }

    uint8_t *firstBytes = NULL;
    uint8_t *secondBytes = NULL;
    uint64_t firstSize = 0U;
    uint64_t secondSize = 0U;
    if (!PlatformReadEntireFile(g_storePathA, 1024U * 1024U, &firstBytes, &firstSize) ||
        !PlatformReadEntireFile(g_storePathB, 1024U * 1024U, &secondBytes, &secondSize) ||
        firstSize != secondSize || !BytesEqual(firstBytes, secondBytes, firstSize))
    {
        Fail("Construct store не детерминирован");
    }

    // Any payload corruption is detected by the payload SHA-256. A rejected
    // file must not replace the live system.
    if (firstSize <= 48U)
    {
        Fail("Construct store имеет неверный размер");
    }
    firstBytes[firstSize - 1U] ^= 0x40U;
    if (!PlatformWriteEntireFile(g_storePathBad, firstBytes, firstSize) ||
        PhysicalConstructStoreLoad(loaded, g_storePathBad) || !LoadedStateValid(loaded))
    {
        Fail("Construct store принял повреждённый payload");
    }

    // A truncated file is rejected before the current system is reset.
    if (!PlatformWriteEntireFile(g_storePathBad, secondBytes, secondSize - 1U) ||
        PhysicalConstructStoreLoad(loaded, g_storePathBad) || !LoadedStateValid(loaded))
    {
        Fail("Construct store принял усечённый файл");
    }

    // The first canonical body has voxel X coordinates -2, -1 and a lever at
    // zero mounted toward +X. Moving the first voxel to +1 creates a component
    // that only touches the lever's non-mount face. Re-sign the crafted payload
    // so this exercises topology validation rather than digest validation.
    const uint32_t firstBlockXOffset = 48U + 72U;
    secondBytes[firstBlockXOffset + 0U] = 1U;
    secondBytes[firstBlockXOffset + 1U] = 0U;
    secondBytes[firstBlockXOffset + 2U] = 0U;
    secondBytes[firstBlockXOffset + 3U] = 0U;
    uint8_t craftedDigest[32];
    if (!PlatformSha256(secondBytes + 48U, secondSize - 48U, craftedDigest))
    {
        Fail("Не удалось подписать crafted construct store");
    }
    for (uint32_t index = 0U; index < 32U; ++index)
    {
        secondBytes[16U + index] = craftedDigest[index];
    }
    if (!PlatformWriteEntireFile(g_storePathBad, secondBytes, secondSize) ||
        PhysicalConstructStoreLoad(loaded, g_storePathBad) || !LoadedStateValid(loaded))
    {
        Fail("Construct store принял связь через боковую грань рычага");
    }

    // A core-local coordinate outside int16 remains valid for the local
    // solver, but cannot be persisted because protocol v6 could not replicate
    // it to another peer.
    PhysicalConstructBodyState unreplicable = {
        .id = 12U,
        .topologyRevision = 1U,
        .origin = {0.0, 0.0, 0.0},
        .velocity = {0.0, 0.0, 0.0},
        .blockCount = 1U,
    };
    PhysicalConstructBlock farBlock = {
        .local = {32768, 0, 0},
        .material = BLOCK_EARTH,
        .kind = PHYSICAL_CONSTRUCT_BLOCK_VOXEL,
    };
    if (PhysicalConstructImportBody(first, &unreplicable, &farBlock, 1U) != PHYSICAL_CONSTRUCT_OK ||
        PhysicalConstructStoreSave(first, g_storePathBad))
    {
        Fail("Construct store принял нереплицируемую локальную координату");
    }

    PlatformFree(firstBytes);
    PlatformFree(secondBytes);
    PhysicalConstructSystemDestroy(first);
    PhysicalConstructSystemDestroy(loaded);
    WorldDestroy(firstWorld);
    WorldDestroy(loadedWorld);
    RemoveTestFiles();
    LAIUE_TEST_SUCCESS();
}
