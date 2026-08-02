#include "world/world.h"
#include "test_runtime.h"

#include <string.h>

static uint32_t worldSnapshotChecks;
static WorldBlockRange g_boundedRanges[WORLD_MAX_BOUNDED_BLOCK_RANGES];

static void SnapshotExpect(bool condition, const char* name)
{
    ++worldSnapshotChecks;
    if (condition) return;
    LaiueTestRuntimeWrite("Проверка не пройдена: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool ChunkEquals(const int64_t left[3], const int64_t right[3])
{
    return left[0] == right[0]
        && left[1] == right[1]
        && left[2] == right[2];
}

LAIUE_TEST_ENTRY(WorldSnapshotTestEntryPoint)
{
    World* server = WorldCreate(0x102030405060708LL);
    World* client = WorldCreate(0x102030405060708LL);
    SnapshotExpect(server != NULL && client != NULL,
        "миры не созданы");
    BlockType invalidRegionGuard = 0x7fU;
    SnapshotExpect(
        WorldFillRegion(server, 0, 0, 0, 1, 1, 0,
            &invalidRegionGuard, NULL, 0) ==
                WORLD_REGION_ALL_AIR &&
        invalidRegionGuard == 0x7fU,
        "некорректный размер региона изменил буфер");

    const int64_t blockA[3] = { 3, 5, 96 };
    const int64_t blockB[3] = { 4, 5, 96 };
    const int64_t blockOther[3] = { CHUNK_SIZE + 2, 5, 96 };
    const int64_t chunkA[3] = { 0, 0, 1 };
    const int64_t chunkOther[3] = { 1, 0, 1 };
    BlockType generatedA = WorldGetBlock(
        server, blockA[0], blockA[1], blockA[2]);
    BlockType generatedB = WorldGetBlock(
        server, blockB[0], blockB[1], blockB[2]);
    BlockType generatedOther = WorldGetBlock(
        server, blockOther[0], blockOther[1], blockOther[2]);
    BlockType changedA = generatedA == BLOCK_AIR
        ? BLOCK_EARTH : BLOCK_AIR;
    BlockType changedB = generatedB == BLOCK_AIR
        ? BLOCK_GRASS : BLOCK_AIR;
    BlockType changedOther = generatedOther == BLOCK_AIR
        ? BLOCK_EARTH : BLOCK_AIR;

    WorldSetBlock(server, blockA[0], blockA[1], blockA[2], changedA);
    WorldSetBlock(server, blockOther[0], blockOther[1], blockOther[2],
        changedOther);
    WorldSetBlock(server, blockB[0], blockB[1], blockB[2], changedB);
    SnapshotExpect(WorldGetRevision(server) == 3,
        "глобальная revision должна считать все изменения");
    SnapshotExpect(WorldGetChunkRevision(server, chunkA) == 2,
        "revision первого чанка не должна включать чужой чанк");
    SnapshotExpect(WorldGetChunkRevision(server, chunkOther) == 1,
        "revision второго чанка неверна");

    WorldChunkSummary summaries[4];
    bool truncated = false;
    uint64_t snapshotRevision = 0;
    const int64_t minimum[3] = { -1, -1, 0 };
    const int64_t maximum[3] = { 2, 1, 2 };
    uint32_t summaryCount = WorldCopyEditedChunkSummaries(
        server, minimum, maximum, summaries, 4,
        &truncated, &snapshotRevision);
    SnapshotExpect(summaryCount == 2 && !truncated
        && snapshotRevision == 3,
        "summary снимка неполон");
    SnapshotExpect(ChunkEquals(summaries[0].chunk, chunkA)
        && ChunkEquals(summaries[1].chunk, chunkOther),
        "summary должны иметь детерминированный порядок");

    WorldChunkDelta deltas[4];
    uint32_t deltaCount = 0;
    uint64_t chunkRevision = 0;
    SnapshotExpect(WorldCopyChunkDeltas(server, chunkA,
            deltas, 4, &deltaCount, &chunkRevision)
        && deltaCount == 2 && chunkRevision == 2,
        "дельты первого чанка не скопированы");
    SnapshotExpect(WorldReplaceChunkDeltas(client, chunkA,
            deltas, deltaCount, chunkRevision, snapshotRevision),
        "дельты первого чанка не применены");
    SnapshotExpect(WorldGetBlock(client,
            blockA[0], blockA[1], blockA[2]) == changedA
        && WorldGetBlock(client,
            blockB[0], blockB[1], blockB[2]) == changedB,
        "клиент не воспроизвёл snapshot");

    WorldSetBlock(server, blockA[0], blockA[1], blockA[2], generatedA);
    WorldSetBlock(server, blockB[0], blockB[1], blockB[2], generatedB);
    SnapshotExpect(WorldCopyChunkDeltas(server, chunkA,
            deltas, 4, &deltaCount, &chunkRevision)
        && deltaCount == 0 && chunkRevision == 4,
        "пустой tombstone чанка потерял revision");
    summaryCount = WorldCopyEditedChunkSummaries(
        server, minimum, maximum, summaries, 4,
        &truncated, &snapshotRevision);
    SnapshotExpect(summaryCount == 2 && !truncated
        && ChunkEquals(summaries[0].chunk, chunkA)
        && summaries[0].deltaCount == 0
        && summaries[0].revision == 4,
        "пустой tombstone отсутствует в enumeration snapshot");
    SnapshotExpect(WorldReplaceChunkDeltas(client, chunkA,
            NULL, 0, chunkRevision, WorldGetRevision(server)),
        "пустой resync чанка не применён");
    SnapshotExpect(WorldGetBlock(client,
            blockA[0], blockA[1], blockA[2]) == generatedA
        && WorldGetBlock(client,
            blockB[0], blockB[1], blockB[2]) == generatedB,
        "пустой resync не восстановил базовый terrain");

    // QUIC control и snapshot streams могут доставляться независимо:
    // delta R+1 имеет право прийти раньше snapshot chunk R. Старый chunk
    // должен считаться обработанным, не откатывая ни данные, ни revision.
    const uint64_t snapshotChunkRevision =
        WorldGetChunkRevision(client, chunkA);
    WorldSetBlock(client,
        blockA[0], blockA[1], blockA[2], changedA);
    SnapshotExpect(WorldGetChunkRevision(client, chunkA)
            == snapshotChunkRevision + 1U
        && WorldGetBlock(client,
            blockA[0], blockA[1], blockA[2]) == changedA,
        "live delta R+1 не подготовлена");
    const WorldChunkDelta staleSnapshotDeltas[1] = {
        {
            .localIndex =
                (uint32_t)blockB[0] * CHUNK_SIZE * CHUNK_SIZE
                + (uint32_t)blockB[1] * CHUNK_SIZE
                + (uint32_t)(blockB[2] % CHUNK_SIZE),
            .block = changedB,
        },
    };
    SnapshotExpect(WorldReplaceChunkDeltas(client, chunkA,
            staleSnapshotDeltas, 1, snapshotChunkRevision,
            WorldGetRevision(client)),
        "старый snapshot chunk должен безопасно подтверждаться");
    SnapshotExpect(WorldGetChunkRevision(client, chunkA)
            == snapshotChunkRevision + 1U
        && WorldGetBlock(client,
            blockA[0], blockA[1], blockA[2]) == changedA
        && WorldGetBlock(client,
            blockB[0], blockB[1], blockB[2]) == generatedB,
        "snapshot R откатил уже применённую delta R+1");

    const int64_t batchA[3] = { 11, 13, 160 };
    const int64_t batchB[3] = { CHUNK_SIZE + 11, 13, 160 };
    BlockType batchGeneratedA = WorldGetBlock(server,
        batchA[0], batchA[1], batchA[2]);
    BlockType batchGeneratedB = WorldGetBlock(server,
        batchB[0], batchB[1], batchB[2]);
    WorldBlockMutation batch[2] = {
        {
            .block = { batchA[0], batchA[1], batchA[2] },
            .expected = batchGeneratedA,
            .replacement = BLOCK_EARTH,
        },
        {
            .block = { batchB[0], batchB[1], batchB[2] },
            .expected = batchGeneratedB,
            .replacement = BLOCK_GRASS,
        },
    };
    uint64_t beforeBatchRevision = WorldGetRevision(server);
    SnapshotExpect(WorldApplyBlockBatch(server, batch, 2U)
        && WorldGetRevision(server) == beforeBatchRevision + 2U
        && WorldGetBlock(server, batchA[0], batchA[1], batchA[2])
            == BLOCK_EARTH
        && WorldGetBlock(server, batchB[0], batchB[1], batchB[2])
            == BLOCK_GRASS
        && WorldIsBlockEdited(
            server, batchA[0], batchA[1], batchA[2])
        && WorldIsBlockEdited(
            server, batchB[0], batchB[1], batchB[2]),
        "atomic batch не опубликовал все правки");
    WorldBlockState blockState;
    SnapshotExpect(WorldGetBlockState(server,
            batchA[0], batchA[1], batchA[2], &blockState)
        && blockState.edited && blockState.block == BLOCK_EARTH,
        "атомарный per-cell material/provenance query неверен");
    const int64_t solidMinimum[3] = {
        batchA[0], batchA[1], batchA[2]
    };
    const int64_t solidMaximum[3] = {
        batchA[0] + 1, batchA[1], batchA[2]
    };
    SnapshotExpect(WorldAnySolidBlockInRange(
            server, solidMinimum, solidMaximum),
        "bounded world occupancy не нашёл solid block");
    const int64_t oversizedMaximum[3] = {
        batchA[0] + WORLD_MAX_BOUNDED_BLOCK_QUERY_CELLS,
        batchA[1], batchA[2]
    };
    SnapshotExpect(!WorldAnySolidBlockInRange(
            server, solidMinimum, oversizedMaximum),
        "oversized fixed-tick world occupancy query принят");
    for (uint32_t index = 0U;
         index < WORLD_MAX_BOUNDED_BLOCK_RANGES; ++index)
    {
        g_boundedRanges[index] = (WorldBlockRange){
            .minimum = { 100000, 100000, 100000 },
            .maximum = { 100000, 100000, 100000 },
        };
    }
    bool anySolid = true;
    SnapshotExpect(WorldAnySolidBlockInRanges(server, g_boundedRanges,
            WORLD_MAX_BOUNDED_BLOCK_RANGES, &anySolid) && !anySolid,
        "maximum bounded world range batch rejected");
    anySolid = true;
    SnapshotExpect(!WorldAnySolidBlockInRanges(server, g_boundedRanges,
            WORLD_MAX_BOUNDED_BLOCK_RANGES + 1U, &anySolid) && !anySolid,
        "oversized world range batch accepted or left stale output");
    g_boundedRanges[0] = (WorldBlockRange){
        .minimum = { batchA[0], batchA[1], batchA[2] },
        .maximum = { batchA[0], batchA[1], batchA[2] },
    };
    SnapshotExpect(WorldAnySolidBlockInRanges(
            server, g_boundedRanges, 1U, &anySolid) && anySolid,
        "batched world occupancy missed a solid block");

    const BlockType invalidBlock = (BlockType)(BLOCK_GRASS + 1U);
    beforeBatchRevision = WorldGetRevision(server);
    SnapshotExpect(!WorldTrySetBlock(server,
            batchA[0], batchA[1], batchA[2], invalidBlock)
        && WorldGetRevision(server) == beforeBatchRevision
        && WorldGetBlock(server, batchA[0], batchA[1], batchA[2])
            == BLOCK_EARTH,
        "invalid single mutation changed world or revision");
    WorldBlockMutation invalidMutation = {
        .block = { batchA[0], batchA[1], batchA[2] },
        .expected = invalidBlock,
        .replacement = BLOCK_AIR,
    };
    SnapshotExpect(!WorldApplyBlockBatch(server, &invalidMutation, 1U)
        && WorldGetRevision(server) == beforeBatchRevision
        && WorldGetBlock(server, batchA[0], batchA[1], batchA[2])
            == BLOCK_EARTH,
        "invalid expected material changed world or revision");
    invalidMutation.expected = BLOCK_EARTH;
    invalidMutation.replacement = invalidBlock;
    SnapshotExpect(!WorldApplyBlockBatch(server, &invalidMutation, 1U)
        && WorldGetRevision(server) == beforeBatchRevision
        && WorldGetBlock(server, batchA[0], batchA[1], batchA[2])
            == BLOCK_EARTH,
        "invalid replacement material changed world or revision");

    WorldBlockMutation rejected[2] = {
        {
            .block = { batchA[0], batchA[1], batchA[2] },
            .expected = BLOCK_GRASS,
            .replacement = BLOCK_AIR,
        },
        {
            .block = { batchB[0], batchB[1], batchB[2] },
            .expected = BLOCK_GRASS,
            .replacement = BLOCK_AIR,
        },
    };
    beforeBatchRevision = WorldGetRevision(server);
    SnapshotExpect(!WorldApplyBlockBatch(server, rejected, 2U)
        && WorldGetRevision(server) == beforeBatchRevision
        && WorldGetBlock(server, batchA[0], batchA[1], batchA[2])
            == BLOCK_EARTH
        && WorldGetBlock(server, batchB[0], batchB[1], batchB[2])
            == BLOCK_GRASS,
        "отклонённый batch частично изменил мир");
    rejected[0] = batch[0];
    rejected[1] = batch[0];
    SnapshotExpect(!WorldApplyBlockBatch(server, rejected, 2U),
        "batch с повторной координатой принят");

    WorldDestroy(client);
    WorldDestroy(server);
    LaiueTestRuntimeWrite("World snapshot/revision: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
