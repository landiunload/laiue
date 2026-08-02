#include "construct/physical_construct_persistence.h"

#include "construct/physical_construct_store.h"
#include "platform/system.h"
#include "world/world.h"

#include <stddef.h>
#include <string.h>

#define CONSTRUCT_PERSISTENCE_VERSION 1U
#define CONSTRUCT_PERSISTENCE_SLOT_COUNT 2U
#define CONSTRUCT_PERSISTENCE_PREFIX_SIZE 96U
#define CONSTRUCT_PERSISTENCE_RECORD_SIZE 128U
#define CONSTRUCT_PERSISTENCE_MAX_DATA_BYTES (64U * 1024U * 1024U)

#define COMMIT_VERSION_OFFSET 4U
#define COMMIT_SLOT_OFFSET 6U
#define COMMIT_RESERVED_OFFSET 7U
#define COMMIT_GENERATION_OFFSET 8U
#define COMMIT_WORLD_SIZE_OFFSET 16U
#define COMMIT_CONSTRUCT_SIZE_OFFSET 24U
#define COMMIT_WORLD_HASH_OFFSET 32U
#define COMMIT_CONSTRUCT_HASH_OFFSET 64U
#define COMMIT_RECORD_HASH_OFFSET 96U

static const uint8_t g_commitMagic[4] = {'L', 'P', 'C', 'P'};

typedef struct PersistenceCandidate
{
    bool present;
    bool valid;
    uint8_t slot;
    uint64_t generation;
    uint64_t worldSize;
    uint64_t constructSize;
    uint8_t worldHash[32];
    uint8_t constructHash[32];
} PersistenceCandidate;

// Windows no-CRT targets cannot rely on __chkstk. Wide paths are therefore
// kept in one bounded heap scratch instead of several multi-kilobyte frames.
typedef struct PersistencePathScratch
{
    wchar_t first[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t second[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t third[LAIUE_PLATFORM_PATH_CAPACITY];
} PersistencePathScratch;

static bool PathsEqual(const wchar_t* left, const wchar_t* right)
{
    if (left == NULL || right == NULL) return false;
    uint32_t index = 0U;
    while (left[index] == right[index])
    {
        if (left[index] == L'\0') return true;
        ++index;
    }
    return false;
}

static bool DistinctPaths(const wchar_t* worldPath,
                          const wchar_t* constructPath,
                          const wchar_t* commitPath)
{
    return worldPath != NULL && constructPath != NULL && commitPath != NULL &&
           !PathsEqual(worldPath, constructPath) &&
           !PathsEqual(worldPath, commitPath) &&
           !PathsEqual(constructPath, commitPath);
}

static void WriteU16(uint8_t* destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void WriteU64(uint8_t* destination, uint64_t value)
{
    for (uint32_t index = 0U; index < 8U; ++index)
    {
        destination[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint16_t ReadU16(const uint8_t* source)
{
    return (uint16_t)source[0] | (uint16_t)((uint16_t)source[1] << 8U);
}

static uint64_t ReadU64(const uint8_t* source)
{
    uint64_t value = 0U;
    for (uint32_t index = 0U; index < 8U; ++index)
    {
        value |= (uint64_t)source[index] << (index * 8U);
    }
    return value;
}

static bool BuildSlotPath(const wchar_t* base, uint8_t slot,
                          wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY])
{
    if (base == NULL || output == NULL || slot >= CONSTRUCT_PERSISTENCE_SLOT_COUNT)
    {
        return false;
    }
    uint32_t length = 0U;
    while (base[length] != L'\0')
    {
        if (length + 3U > LAIUE_PLATFORM_PATH_CAPACITY)
        {
            return false;
        }
        output[length] = base[length];
        ++length;
    }
    if (length + 3U > LAIUE_PLATFORM_PATH_CAPACITY)
    {
        return false;
    }
    output[length++] = L'.';
    output[length++] = (wchar_t)(L'0' + slot);
    output[length] = L'\0';
    return true;
}

static bool RegularFileInformation(const wchar_t* path,
                                   PlatformPathInformation* information)
{
    return PlatformGetPathInformation(path, information) && information->exists &&
           !information->isDirectory && !information->isSymbolicLink;
}

static bool ReadFileHash(const wchar_t* path, uint64_t expectedSize,
                         uint8_t output[32])
{
    if (expectedSize == 0U ||
        expectedSize > CONSTRUCT_PERSISTENCE_MAX_DATA_BYTES)
    {
        return false;
    }
    PlatformPathInformation information;
    if (!RegularFileInformation(path, &information) ||
        information.size != expectedSize)
    {
        return false;
    }
    uint8_t* bytes = NULL;
    uint64_t size = 0U;
    bool succeeded = PlatformReadEntireFile(path, expectedSize, &bytes, &size) &&
                     size == expectedSize && PlatformSha256(bytes, size, output);
    PlatformFree(bytes);
    return succeeded;
}

static bool HashFile(const wchar_t* path, uint64_t* outSize,
                     uint8_t output[32])
{
    PlatformPathInformation information;
    if (outSize == NULL || !RegularFileInformation(path, &information) ||
        information.size == 0U ||
        information.size > CONSTRUCT_PERSISTENCE_MAX_DATA_BYTES)
    {
        return false;
    }
    *outSize = information.size;
    return ReadFileHash(path, information.size, output);
}

static bool ReadCandidate(const wchar_t* worldPath,
                          const wchar_t* constructPath,
                          const wchar_t* commitPath, uint8_t slot,
                          PersistenceCandidate* candidate)
{
    memset(candidate, 0, sizeof(*candidate));
    candidate->slot = slot;

    PersistencePathScratch* paths =
        PlatformAllocate(sizeof(*paths), false);
    if (paths == NULL)
    {
        return false;
    }
    if (!BuildSlotPath(commitPath, slot, paths->first) ||
        !BuildSlotPath(worldPath, slot, paths->second) ||
        !BuildSlotPath(constructPath, slot, paths->third))
    {
        PlatformFree(paths);
        return false;
    }

    PlatformPathInformation markerInformation;
    if (!PlatformGetPathInformation(paths->first, &markerInformation))
    {
        PlatformFree(paths);
        return false;
    }
    candidate->present = markerInformation.exists;
    if (!candidate->present || markerInformation.isDirectory ||
        markerInformation.isSymbolicLink ||
        markerInformation.size != CONSTRUCT_PERSISTENCE_RECORD_SIZE)
    {
        PlatformFree(paths);
        return true;
    }

    uint8_t* bytes = NULL;
    uint64_t size = 0U;
    uint8_t actualRecordHash[32];
    bool recordValid = PlatformReadEntireFile(
                           paths->first, CONSTRUCT_PERSISTENCE_RECORD_SIZE,
                           &bytes, &size) &&
                       size == CONSTRUCT_PERSISTENCE_RECORD_SIZE &&
                       memcmp(bytes, g_commitMagic, sizeof(g_commitMagic)) == 0 &&
                       ReadU16(bytes + COMMIT_VERSION_OFFSET) ==
                           CONSTRUCT_PERSISTENCE_VERSION &&
                       bytes[COMMIT_SLOT_OFFSET] == slot &&
                       bytes[COMMIT_RESERVED_OFFSET] == 0U &&
                       PlatformSha256(bytes, CONSTRUCT_PERSISTENCE_PREFIX_SIZE,
                                      actualRecordHash) &&
                       PlatformConstantTimeEqual(
                           actualRecordHash,
                           bytes + COMMIT_RECORD_HASH_OFFSET, 32U);
    if (!recordValid)
    {
        PlatformFree(bytes);
        PlatformFree(paths);
        return true;
    }

    candidate->generation = ReadU64(bytes + COMMIT_GENERATION_OFFSET);
    candidate->worldSize = ReadU64(bytes + COMMIT_WORLD_SIZE_OFFSET);
    candidate->constructSize = ReadU64(bytes + COMMIT_CONSTRUCT_SIZE_OFFSET);
    memcpy(candidate->worldHash, bytes + COMMIT_WORLD_HASH_OFFSET, 32U);
    memcpy(candidate->constructHash,
           bytes + COMMIT_CONSTRUCT_HASH_OFFSET, 32U);
    PlatformFree(bytes);

    uint8_t actualWorldHash[32];
    uint8_t actualConstructHash[32];
    candidate->valid = candidate->generation != 0U &&
        ReadFileHash(paths->second, candidate->worldSize, actualWorldHash) &&
        ReadFileHash(paths->third, candidate->constructSize,
                     actualConstructHash) &&
        PlatformConstantTimeEqual(candidate->worldHash,
                                  actualWorldHash, 32U) &&
        PlatformConstantTimeEqual(candidate->constructHash,
                                  actualConstructHash, 32U);
    PlatformFree(paths);
    return true;
}

static bool ReadCandidates(const wchar_t* worldPath,
                           const wchar_t* constructPath,
                           const wchar_t* commitPath,
                           PersistenceCandidate candidates[2],
                           bool* outAnyMarker)
{
    if (worldPath == NULL || constructPath == NULL || commitPath == NULL ||
        candidates == NULL || outAnyMarker == NULL)
    {
        return false;
    }
    *outAnyMarker = false;
    for (uint8_t slot = 0U; slot < CONSTRUCT_PERSISTENCE_SLOT_COUNT; ++slot)
    {
        if (!ReadCandidate(worldPath, constructPath, commitPath,
                           slot, &candidates[slot]))
        {
            return false;
        }
        *outAnyMarker = *outAnyMarker || candidates[slot].present;
    }
    return true;
}

static int32_t NewestCandidate(const PersistenceCandidate candidates[2])
{
    if (candidates[0].valid && candidates[1].valid)
    {
        if (candidates[0].generation == candidates[1].generation)
        {
            return -2;
        }
        return candidates[0].generation > candidates[1].generation ? 0 : 1;
    }
    if (candidates[0].valid) return 0;
    if (candidates[1].valid) return 1;
    return -1;
}

static bool WriteCommit(const wchar_t* commitPath, uint8_t slot,
                        uint64_t generation, uint64_t worldSize,
                        const uint8_t worldHash[32],
                        uint64_t constructSize,
                        const uint8_t constructHash[32])
{
    wchar_t marker[LAIUE_PLATFORM_PATH_CAPACITY];
    if (!BuildSlotPath(commitPath, slot, marker)) return false;

    uint8_t bytes[CONSTRUCT_PERSISTENCE_RECORD_SIZE];
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, g_commitMagic, sizeof(g_commitMagic));
    WriteU16(bytes + COMMIT_VERSION_OFFSET, CONSTRUCT_PERSISTENCE_VERSION);
    bytes[COMMIT_SLOT_OFFSET] = slot;
    WriteU64(bytes + COMMIT_GENERATION_OFFSET, generation);
    WriteU64(bytes + COMMIT_WORLD_SIZE_OFFSET, worldSize);
    WriteU64(bytes + COMMIT_CONSTRUCT_SIZE_OFFSET, constructSize);
    memcpy(bytes + COMMIT_WORLD_HASH_OFFSET, worldHash, 32U);
    memcpy(bytes + COMMIT_CONSTRUCT_HASH_OFFSET, constructHash, 32U);
    if (!PlatformSha256(bytes, CONSTRUCT_PERSISTENCE_PREFIX_SIZE,
                        bytes + COMMIT_RECORD_HASH_OFFSET))
    {
        return false;
    }
    return PlatformWriteFileAtomic(marker, bytes, sizeof(bytes));
}

bool PhysicalConstructPersistenceSave(
    World* world, const PhysicalConstructSystem* constructs,
    const wchar_t* worldPath, const wchar_t* constructPath,
    const wchar_t* commitPath)
{
    if (world == NULL || constructs == NULL ||
        !DistinctPaths(worldPath, constructPath, commitPath))
    {
        return false;
    }

    PersistenceCandidate candidates[2];
    bool anyMarker = false;
    if (!ReadCandidates(worldPath, constructPath, commitPath,
                        candidates, &anyMarker))
    {
        return false;
    }
    int32_t newest = NewestCandidate(candidates);
    if (newest == -2 || (newest < 0 && anyMarker))
    {
        return false;
    }

    uint8_t targetSlot = newest < 0 ? 0U
        : (uint8_t)(1U - (uint8_t)newest);
    uint64_t generation = newest < 0 ? 1U
        : candidates[newest].generation + 1U;
    if (generation == 0U) return false;

    PersistencePathScratch* paths =
        PlatformAllocate(sizeof(*paths), false);
    if (paths == NULL)
    {
        return false;
    }
    bool dataSaved = BuildSlotPath(worldPath, targetSlot, paths->first) &&
        BuildSlotPath(constructPath, targetSlot, paths->second) &&
        WorldSaveDeltas(world, paths->first) &&
        PhysicalConstructStoreSave(constructs, paths->second);
    if (!dataSaved)
    {
        PlatformFree(paths);
        return false;
    }

    uint64_t worldSize = 0U;
    uint64_t constructSize = 0U;
    uint8_t worldHash[32];
    uint8_t constructHash[32];
    bool committed = HashFile(paths->first, &worldSize, worldHash) &&
        HashFile(paths->second, &constructSize, constructHash) &&
        WriteCommit(commitPath, targetSlot, generation,
                    worldSize, worldHash,
                    constructSize, constructHash);
    PlatformFree(paths);
    return committed;
}

static bool LoadCandidate(World* world, PhysicalConstructSystem* constructs,
                          const wchar_t* worldPath,
                          const wchar_t* constructPath,
                          const PersistenceCandidate* candidate)
{
    PersistencePathScratch* paths =
        PlatformAllocate(sizeof(*paths), false);
    if (paths == NULL)
    {
        return false;
    }
    // Construct loading validates its entire payload before replacing the
    // in-memory set. World is loaded second; callers create a fresh World and
    // destroy the pair on failure.
    bool loaded = BuildSlotPath(
            worldPath, candidate->slot, paths->first) &&
        BuildSlotPath(constructPath, candidate->slot, paths->second) &&
        PhysicalConstructStoreLoad(constructs, paths->second) &&
        WorldLoadDeltas(world, paths->first);
    PlatformFree(paths);
    return loaded;
}

static bool LoadLegacy(World* world, PhysicalConstructSystem* constructs,
                       const wchar_t* worldPath,
                       const wchar_t* constructPath)
{
    PlatformPathInformation worldInformation;
    PlatformPathInformation constructInformation;
    if (!PlatformGetPathInformation(worldPath, &worldInformation) ||
        !PlatformGetPathInformation(constructPath, &constructInformation))
    {
        return false;
    }
    if ((worldInformation.exists &&
         (worldInformation.isDirectory || worldInformation.isSymbolicLink)) ||
        (constructInformation.exists &&
         (constructInformation.isDirectory ||
          constructInformation.isSymbolicLink)))
    {
        return false;
    }
    return (!constructInformation.exists ||
            PhysicalConstructStoreLoad(constructs, constructPath)) &&
           (!worldInformation.exists || WorldLoadDeltas(world, worldPath));
}

bool PhysicalConstructPersistenceLoad(
    World* world, PhysicalConstructSystem* constructs,
    const wchar_t* worldPath, const wchar_t* constructPath,
    const wchar_t* commitPath)
{
    if (world == NULL || constructs == NULL ||
        !DistinctPaths(worldPath, constructPath, commitPath))
    {
        return false;
    }
    PersistenceCandidate candidates[2];
    bool anyMarker = false;
    if (!ReadCandidates(worldPath, constructPath, commitPath,
                        candidates, &anyMarker))
    {
        return false;
    }
    int32_t newest = NewestCandidate(candidates);
    if (newest == -2) return false;
    if (newest >= 0)
    {
        // Damage/incomplete writes are filtered while selecting candidates,
        // before either live object is mutated. A semantic/OOM failure while
        // loading a hash-valid generation is fail-closed: retrying an older
        // file into a possibly partially loaded World would mix generations.
        return LoadCandidate(world, constructs, worldPath, constructPath,
                             &candidates[newest]);
    }
    return !anyMarker &&
           LoadLegacy(world, constructs, worldPath, constructPath);
}
