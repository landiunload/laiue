#include "construct/physical_construct_store.h"

#include "platform/system.h"
#include "world/world.h"

#include <float.h>
#include <stddef.h>
#include <string.h>

#define CONSTRUCT_STORE_VERSION 1U
#define CONSTRUCT_STORE_HEADER_SIZE 48U
#define CONSTRUCT_STORE_BODY_SIZE 72U
#define CONSTRUCT_STORE_BLOCK_SIZE 20U
#define CONSTRUCT_STORE_DIGEST_OFFSET 16U
#define CONSTRUCT_STORE_DIGEST_SIZE 32U
#define CONSTRUCT_STORE_POSITION_LIMIT 1099511627776.0
#define CONSTRUCT_STORE_VELOCITY_LIMIT 1048576.0
#define CONSTRUCT_STORE_MAX_BYTES                                                                  \
    (CONSTRUCT_STORE_HEADER_SIZE +                                                                 \
     PHYSICAL_CONSTRUCT_MAX_BODIES *                                                               \
         (CONSTRUCT_STORE_BODY_SIZE + PHYSICAL_CONSTRUCT_MAX_BLOCKS * CONSTRUCT_STORE_BLOCK_SIZE))

_Static_assert(sizeof(double) == 8U && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "construct store requires IEEE-754 binary64");

typedef struct ConstructStoreWriter
{
    uint8_t *bytes;
    uint32_t size;
    uint32_t capacity;
    bool failed;
} ConstructStoreWriter;

typedef struct ConstructStoreReader
{
    const uint8_t *bytes;
    uint32_t size;
    uint32_t offset;
    bool failed;
} ConstructStoreReader;

typedef struct ConstructStoreLoadedBody
{
    PhysicalConstructBodyState state;
    PhysicalConstructBlock blocks[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
} ConstructStoreLoadedBody;

typedef struct ConstructStoreLoadedData
{
    uint32_t bodyCount;
    ConstructStoreLoadedBody bodies[PHYSICAL_CONSTRUCT_MAX_BODIES];
} ConstructStoreLoadedData;

typedef struct ConstructStoreSaveScratch
{
    PhysicalConstructBodyState states[PHYSICAL_CONSTRUCT_MAX_BODIES];
    PhysicalConstructBlock blocks[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
} ConstructStoreSaveScratch;

static const uint8_t g_constructStoreMagic[4] = {'L', 'P', 'C', 'S'};

static void WriterU8(ConstructStoreWriter *writer, uint8_t value)
{
    if (writer->failed || writer->size >= writer->capacity)
    {
        writer->failed = true;
        return;
    }
    writer->bytes[writer->size++] = value;
}

static void WriterU16(ConstructStoreWriter *writer, uint16_t value)
{
    WriterU8(writer, (uint8_t)value);
    WriterU8(writer, (uint8_t)(value >> 8U));
}

static void WriterU32(ConstructStoreWriter *writer, uint32_t value)
{
    WriterU8(writer, (uint8_t)value);
    WriterU8(writer, (uint8_t)(value >> 8U));
    WriterU8(writer, (uint8_t)(value >> 16U));
    WriterU8(writer, (uint8_t)(value >> 24U));
}

static void WriterU64(ConstructStoreWriter *writer, uint64_t value)
{
    WriterU32(writer, (uint32_t)value);
    WriterU32(writer, (uint32_t)(value >> 32U));
}

static void WriterI32(ConstructStoreWriter *writer, int32_t value)
{
    WriterU32(writer, (uint32_t)(int64_t)value);
}

static void WriterDouble(ConstructStoreWriter *writer, double value)
{
    uint64_t bits = 0U;
    if (value == 0.0)
    {
        value = 0.0; // Canonicalize negative zero for deterministic files.
    }
    memcpy(&bits, &value, sizeof(bits));
    WriterU64(writer, bits);
}

static uint8_t ReaderU8(ConstructStoreReader *reader)
{
    if (reader->failed || reader->offset >= reader->size)
    {
        reader->failed = true;
        return 0U;
    }
    return reader->bytes[reader->offset++];
}

static uint16_t ReaderU16(ConstructStoreReader *reader)
{
    uint16_t value = ReaderU8(reader);
    value |= (uint16_t)((uint16_t)ReaderU8(reader) << 8U);
    return value;
}

static uint32_t ReaderU32(ConstructStoreReader *reader)
{
    uint32_t value = ReaderU8(reader);
    value |= (uint32_t)ReaderU8(reader) << 8U;
    value |= (uint32_t)ReaderU8(reader) << 16U;
    value |= (uint32_t)ReaderU8(reader) << 24U;
    return value;
}

static uint64_t ReaderU64(ConstructStoreReader *reader)
{
    uint64_t value = ReaderU32(reader);
    value |= (uint64_t)ReaderU32(reader) << 32U;
    return value;
}

static int32_t ReaderI32(ConstructStoreReader *reader)
{
    uint32_t raw = ReaderU32(reader);
    int64_t value = raw <= INT32_MAX ? (int64_t)raw : (int64_t)raw - 0x100000000LL;
    return (int32_t)value;
}

static double ReaderDouble(ConstructStoreReader *reader, bool *outFinite)
{
    uint64_t bits = ReaderU64(reader);
    double value = 0.0;
    *outFinite = (bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
    memcpy(&value, &bits, sizeof(value));
    if (value == 0.0)
    {
        value = 0.0;
    }
    return value;
}

static bool ValueBounded(double value, double limit)
{
    return value >= -limit && value <= limit;
}

static bool UnitNormal(const int8_t normal[3])
{
    uint32_t nonzero = 0U;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (normal[axis] < -1 || normal[axis] > 1)
        {
            return false;
        }
        if (normal[axis] != 0)
        {
            ++nonzero;
        }
    }
    return nonzero == 1U;
}

static int CompareLocal(const PhysicalConstructBlock *left, const PhysicalConstructBlock *right)
{
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (left->local[axis] < right->local[axis])
        {
            return -1;
        }
        if (left->local[axis] > right->local[axis])
        {
            return 1;
        }
    }
    return 0;
}

static void SortBlocks(PhysicalConstructBlock *blocks, uint32_t count)
{
    for (uint32_t index = 1U; index < count; ++index)
    {
        PhysicalConstructBlock value = blocks[index];
        uint32_t destination = index;
        while (destination > 0U && CompareLocal(&value, &blocks[destination - 1U]) < 0)
        {
            blocks[destination] = blocks[destination - 1U];
            --destination;
        }
        blocks[destination] = value;
    }
}

static void SortStates(PhysicalConstructBodyState *states, uint32_t count)
{
    for (uint32_t index = 1U; index < count; ++index)
    {
        PhysicalConstructBodyState value = states[index];
        uint32_t destination = index;
        while (destination > 0U && value.id < states[destination - 1U].id)
        {
            states[destination] = states[destination - 1U];
            --destination;
        }
        states[destination] = value;
    }
}

static bool BlockFieldsValid(const PhysicalConstructBlock *block)
{
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        // Stored constructs must remain representable by protocol v6, whose
        // canonical local coordinate is a signed little-endian int16.
        if (block->local[axis] < INT16_MIN || block->local[axis] > INT16_MAX)
        {
            return false;
        }
    }
    if (block->kind == PHYSICAL_CONSTRUCT_BLOCK_VOXEL)
    {
        return (block->material == BLOCK_EARTH || block->material == BLOCK_GRASS) &&
               block->mountNormal[0] == 0 && block->mountNormal[1] == 0 &&
               block->mountNormal[2] == 0;
    }
    if (block->kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
    {
        return block->material == BLOCK_AIR && block->local[0] == 0 && block->local[1] == 0 &&
               block->local[2] == 0 && UnitNormal(block->mountNormal);
    }
    return false;
}

static bool BlocksStructurallyConnected(const PhysicalConstructBlock *left,
                                        const PhysicalConstructBlock *right)
{
    const PhysicalConstructBlock *lever = NULL;
    const PhysicalConstructBlock *voxel = NULL;
    if (left->kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER &&
        right->kind == PHYSICAL_CONSTRUCT_BLOCK_VOXEL)
    {
        lever = left;
        voxel = right;
    }
    else if (right->kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER &&
             left->kind == PHYSICAL_CONSTRUCT_BLOCK_VOXEL)
    {
        lever = right;
        voxel = left;
    }
    if (lever != NULL)
    {
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            if (voxel->local[axis] !=
                lever->local[axis] - (int32_t)lever->mountNormal[axis])
            {
                return false;
            }
        }
        return true;
    }
    if (left->kind != PHYSICAL_CONSTRUCT_BLOCK_VOXEL ||
        right->kind != PHYSICAL_CONSTRUCT_BLOCK_VOXEL)
    {
        return false;
    }

    int64_t distance = 0;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        int64_t difference = (int64_t)left->local[axis] - right->local[axis];
        distance += difference < 0 ? -difference : difference;
    }
    return distance == 1;
}

static bool BlocksConnected(const PhysicalConstructBlock *blocks, uint32_t count)
{
    bool visited[PHYSICAL_CONSTRUCT_MAX_BLOCKS] = {false};
    uint16_t queue[PHYSICAL_CONSTRUCT_MAX_BLOCKS];
    uint32_t read = 0U;
    uint32_t written = 1U;
    visited[0] = true;
    queue[0] = 0U;
    while (read < written)
    {
        const PhysicalConstructBlock *current = &blocks[queue[read++]];
        for (uint32_t candidate = 0U; candidate < count; ++candidate)
        {
            if (visited[candidate])
            {
                continue;
            }
            if (BlocksStructurallyConnected(current, &blocks[candidate]))
            {
                visited[candidate] = true;
                queue[written++] = (uint16_t)candidate;
            }
        }
    }
    return written == count;
}

static bool BlocksValid(PhysicalConstructBlock *blocks, uint32_t count)
{
    uint32_t leverCount = 0U;
    if (count == 0U || count > PHYSICAL_CONSTRUCT_MAX_BLOCKS)
    {
        return false;
    }
    for (uint32_t index = 0U; index < count; ++index)
    {
        if (!BlockFieldsValid(&blocks[index]))
        {
            return false;
        }
        if (blocks[index].kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER && ++leverCount > 1U)
        {
            return false;
        }
    }
    SortBlocks(blocks, count);
    for (uint32_t index = 1U; index < count; ++index)
    {
        if (CompareLocal(&blocks[index - 1U], &blocks[index]) == 0)
        {
            return false;
        }
    }
    return BlocksConnected(blocks, count);
}

static bool StateValid(const PhysicalConstructBodyState *state)
{
    if (state->id == 0U || state->topologyRevision == 0U || state->blockCount == 0U ||
        state->blockCount > PHYSICAL_CONSTRUCT_MAX_BLOCKS)
    {
        return false;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (!ValueBounded(state->origin[axis], CONSTRUCT_STORE_POSITION_LIMIT) ||
            !ValueBounded(state->velocity[axis], CONSTRUCT_STORE_VELOCITY_LIMIT))
        {
            return false;
        }
    }
    return true;
}

static void WriteBody(ConstructStoreWriter *writer, const PhysicalConstructBodyState *state,
                      const PhysicalConstructBlock *blocks)
{
    WriterU64(writer, state->id);
    WriterU64(writer, state->topologyRevision);
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        WriterDouble(writer, state->origin[axis]);
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        WriterDouble(writer, state->velocity[axis]);
    }
    WriterU32(writer, state->blockCount);
    WriterU32(writer, 0U);
    for (uint32_t index = 0U; index < state->blockCount; ++index)
    {
        const PhysicalConstructBlock *block = &blocks[index];
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            WriterI32(writer, block->local[axis]);
        }
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            WriterU8(writer, (uint8_t)block->mountNormal[axis]);
        }
        WriterU8(writer, block->material);
        WriterU8(writer, block->kind);
        WriterU8(writer, 0U);
        WriterU8(writer, 0U);
        WriterU8(writer, 0U);
    }
}

bool PhysicalConstructStoreSave(const PhysicalConstructSystem *system, const wchar_t *path)
{
    if (system == NULL || path == NULL)
    {
        return false;
    }
    ConstructStoreSaveScratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        return false;
    }
    bool truncated = false;
    uint32_t bodyCount = PhysicalConstructCopyBodyStates(system, scratch->states,
                                                         PHYSICAL_CONSTRUCT_MAX_BODIES, &truncated);
    if (truncated || bodyCount > PHYSICAL_CONSTRUCT_MAX_BODIES)
    {
        PlatformFree(scratch);
        return false;
    }
    SortStates(scratch->states, bodyCount);
    for (uint32_t index = 0U; index < bodyCount; ++index)
    {
        if (!StateValid(&scratch->states[index]) ||
            (index > 0U && scratch->states[index - 1U].id == scratch->states[index].id))
        {
            PlatformFree(scratch);
            return false;
        }
    }

    uint8_t *bytes = PlatformAllocate(CONSTRUCT_STORE_MAX_BYTES, false);
    if (bytes == NULL)
    {
        PlatformFree(scratch);
        return false;
    }
    ConstructStoreWriter writer = {
        .bytes = bytes,
        .capacity = CONSTRUCT_STORE_MAX_BYTES,
    };
    for (uint32_t index = 0U; index < 4U; ++index)
    {
        WriterU8(&writer, g_constructStoreMagic[index]);
    }
    WriterU16(&writer, CONSTRUCT_STORE_VERSION);
    WriterU16(&writer, CONSTRUCT_STORE_HEADER_SIZE);
    WriterU32(&writer, bodyCount);
    WriterU32(&writer, 0U); // Filled with the final byte count below.
    for (uint32_t index = 0U; index < CONSTRUCT_STORE_DIGEST_SIZE; ++index)
    {
        WriterU8(&writer, 0U);
    }

    for (uint32_t body = 0U; body < bodyCount && !writer.failed; ++body)
    {
        uint32_t blockCount = 0U;
        if (!PhysicalConstructCopyBlocks(system, scratch->states[body].id, scratch->blocks,
                                         PHYSICAL_CONSTRUCT_MAX_BLOCKS, &blockCount) ||
            blockCount != scratch->states[body].blockCount ||
            !BlocksValid(scratch->blocks, blockCount))
        {
            writer.failed = true;
            break;
        }
        WriteBody(&writer, &scratch->states[body], scratch->blocks);
    }

    uint8_t digest[CONSTRUCT_STORE_DIGEST_SIZE];
    bool succeeded = !writer.failed;
    if (succeeded)
    {
        uint32_t fileSize = writer.size;
        bytes[12] = (uint8_t)fileSize;
        bytes[13] = (uint8_t)(fileSize >> 8U);
        bytes[14] = (uint8_t)(fileSize >> 16U);
        bytes[15] = (uint8_t)(fileSize >> 24U);
        succeeded = PlatformSha256(bytes + CONSTRUCT_STORE_HEADER_SIZE,
                                   fileSize - CONSTRUCT_STORE_HEADER_SIZE, digest);
        for (uint32_t index = 0U; succeeded && index < CONSTRUCT_STORE_DIGEST_SIZE; ++index)
        {
            bytes[CONSTRUCT_STORE_DIGEST_OFFSET + index] = digest[index];
        }
        if (succeeded)
        {
            succeeded = PlatformWriteFileAtomic(path, bytes, fileSize);
        }
    }
    PlatformFree(bytes);
    PlatformFree(scratch);
    return succeeded;
}

static bool ReadBody(ConstructStoreReader *reader, ConstructStoreLoadedBody *body)
{
    body->state.id = ReaderU64(reader);
    body->state.topologyRevision = ReaderU64(reader);
    bool finite = true;
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        bool valueFinite = false;
        body->state.origin[axis] = ReaderDouble(reader, &valueFinite);
        finite = finite && valueFinite;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        bool valueFinite = false;
        body->state.velocity[axis] = ReaderDouble(reader, &valueFinite);
        finite = finite && valueFinite;
    }
    body->state.blockCount = ReaderU32(reader);
    uint32_t reserved = ReaderU32(reader);
    body->state.grabOwner = 0U;
    body->state.grabbed = false;
    if (reader->failed || !finite || reserved != 0U || !StateValid(&body->state))
    {
        return false;
    }
    for (uint32_t index = 0U; index < body->state.blockCount; ++index)
    {
        PhysicalConstructBlock *block = &body->blocks[index];
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            block->local[axis] = ReaderI32(reader);
        }
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            uint8_t raw = ReaderU8(reader);
            int16_t signedValue = raw <= INT8_MAX ? (int16_t)raw : (int16_t)raw - 256;
            block->mountNormal[axis] = (int8_t)signedValue;
        }
        block->material = ReaderU8(reader);
        block->kind = ReaderU8(reader);
        uint8_t reserved0 = ReaderU8(reader);
        uint8_t reserved1 = ReaderU8(reader);
        uint8_t reserved2 = ReaderU8(reader);
        if (reserved0 != 0U || reserved1 != 0U || reserved2 != 0U)
        {
            return false;
        }
    }
    return !reader->failed && BlocksValid(body->blocks, body->state.blockCount);
}

bool PhysicalConstructStoreLoad(PhysicalConstructSystem *system, const wchar_t *path)
{
    if (system == NULL || path == NULL)
    {
        return false;
    }
    uint8_t *bytes = NULL;
    uint64_t fileSize64 = 0U;
    if (!PlatformReadEntireFile(path, CONSTRUCT_STORE_MAX_BYTES, &bytes, &fileSize64) ||
        fileSize64 < CONSTRUCT_STORE_HEADER_SIZE || fileSize64 > UINT32_MAX)
    {
        PlatformFree(bytes);
        return false;
    }

    uint32_t fileSize = (uint32_t)fileSize64;
    ConstructStoreReader reader = {.bytes = bytes, .size = fileSize};
    bool headerValid = true;
    for (uint32_t index = 0U; index < 4U; ++index)
    {
        uint8_t actual = ReaderU8(&reader);
        if (actual != g_constructStoreMagic[index])
        {
            headerValid = false;
        }
    }
    uint16_t version = ReaderU16(&reader);
    uint16_t headerSize = ReaderU16(&reader);
    uint32_t bodyCount = ReaderU32(&reader);
    uint32_t storedSize = ReaderU32(&reader);
    uint8_t storedDigest[CONSTRUCT_STORE_DIGEST_SIZE];
    for (uint32_t index = 0U; index < CONSTRUCT_STORE_DIGEST_SIZE; ++index)
    {
        storedDigest[index] = ReaderU8(&reader);
    }
    uint8_t actualDigest[CONSTRUCT_STORE_DIGEST_SIZE];
    bool valid = headerValid && !reader.failed && version == CONSTRUCT_STORE_VERSION &&
                 headerSize == CONSTRUCT_STORE_HEADER_SIZE &&
                 bodyCount <= PHYSICAL_CONSTRUCT_MAX_BODIES && storedSize == fileSize &&
                 PlatformSha256(bytes + CONSTRUCT_STORE_HEADER_SIZE,
                                fileSize - CONSTRUCT_STORE_HEADER_SIZE, actualDigest) &&
                 PlatformConstantTimeEqual(storedDigest, actualDigest, CONSTRUCT_STORE_DIGEST_SIZE);

    ConstructStoreLoadedData *loaded = NULL;
    if (valid)
    {
        loaded = PlatformAllocate(sizeof(*loaded), true);
        valid = loaded != NULL;
    }
    if (valid)
    {
        loaded->bodyCount = bodyCount;
        uint64_t previousId = 0U;
        for (uint32_t index = 0U; index < bodyCount && valid; ++index)
        {
            valid = ReadBody(&reader, &loaded->bodies[index]) &&
                    loaded->bodies[index].state.id > previousId;
            previousId = loaded->bodies[index].state.id;
        }
        valid = valid && !reader.failed && reader.offset == fileSize;
    }

    if (valid)
    {
        PhysicalConstructSystemReset(system);
        for (uint32_t index = 0U; index < loaded->bodyCount; ++index)
        {
            ConstructStoreLoadedBody *body = &loaded->bodies[index];
            if (PhysicalConstructImportBody(system, &body->state, body->blocks,
                                            body->state.blockCount) != PHYSICAL_CONSTRUCT_OK)
            {
                PhysicalConstructSystemReset(system);
                valid = false;
                break;
            }
        }
    }

    PlatformFree(loaded);
    PlatformFree(bytes);
    return valid;
}
