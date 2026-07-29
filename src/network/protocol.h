#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LAIUE_PROTOCOL_MAGIC 0x5549414cU /* "LAIU" little-endian */
#define LAIUE_PROTOCOL_VERSION 5U
#define LAIUE_PROTOCOL_ALPN "laiue/5"
#define LAIUE_PROTOCOL_HEADER_SIZE 16U
#define LAIUE_PROTOCOL_MAX_CONTROL_PAYLOAD_SIZE 1024U
#define LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE LAIUE_PROTOCOL_MAX_CONTROL_PAYLOAD_SIZE
#define LAIUE_PROTOCOL_MAX_FRAME_SIZE (LAIUE_PROTOCOL_HEADER_SIZE + LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE)
#define LAIUE_PROTOCOL_PLAYER_STATE_PAYLOAD_SIZE 117U

#define LAIUE_PROTOCOL_MAX_MODS 32U
#define LAIUE_PROTOCOL_MOD_ID_CAPACITY 32U
#define LAIUE_PROTOCOL_MOD_VERSION_CAPACITY 16U
#define LAIUE_PROTOCOL_MOD_HASH_SIZE 32U
#define LAIUE_PROTOCOL_CONTENT_HASH_SIZE 32U
#define LAIUE_PROTOCOL_MAX_SNAPSHOT_CHUNKS 4096U
#define LAIUE_PROTOCOL_MAX_CHUNK_EDITS 128U
#define LAIUE_PROTOCOL_CHUNK_SIZE 64U
#define LAIUE_PROTOCOL_MAX_CHUNK_PARTS \
    ((LAIUE_PROTOCOL_CHUNK_SIZE * LAIUE_PROTOCOL_CHUNK_SIZE * \
      LAIUE_PROTOCOL_CHUNK_SIZE) / LAIUE_PROTOCOL_MAX_CHUNK_EDITS)
#define LAIUE_PROTOCOL_MAX_CONTROL_STREAMS 1U
#define LAIUE_PROTOCOL_MAX_SERVER_STREAMS 2U
#define LAIUE_PROTOCOL_MAX_QUEUED_FRAMES 256U
#define LAIUE_PROTOCOL_MAX_OUTSTANDING_SENDS 256U

typedef enum LaiueMessageType
{
    LAIUE_MESSAGE_CLIENT_HELLO = 1,
    LAIUE_MESSAGE_SERVER_MOD_LIST = 2,
    LAIUE_MESSAGE_SERVER_MOD_ENTRY = 3,
    LAIUE_MESSAGE_CLIENT_MOD_LIST = 4,
    LAIUE_MESSAGE_CLIENT_MOD_ENTRY = 5,
    LAIUE_MESSAGE_SERVER_WELCOME = 6,
    LAIUE_MESSAGE_SERVER_REJECT = 7,
    LAIUE_MESSAGE_PLAYER_INPUT = 8,
    LAIUE_MESSAGE_EDIT_INTENT = 9,
    LAIUE_MESSAGE_PLAYER_STATE = 10,
    LAIUE_MESSAGE_BLOCK_DELTA = 11,
    LAIUE_MESSAGE_PING = 12,
    LAIUE_MESSAGE_PONG = 13,
    LAIUE_MESSAGE_CLIENT_CONTENT_REQUEST = 14,
    LAIUE_MESSAGE_SERVER_CONTENT_BEGIN = 15,
    LAIUE_MESSAGE_SERVER_CONTENT_CHUNK = 16,
    LAIUE_MESSAGE_SERVER_CONTENT_END = 17,
    LAIUE_MESSAGE_BLOCK_DROP_SPAWN = 18,
    LAIUE_MESSAGE_BLOCK_DROP_REMOVE = 19,
    LAIUE_MESSAGE_INVENTORY_STATE = 20,
    LAIUE_MESSAGE_SELECT_HOTBAR_SLOT = 21,
    LAIUE_MESSAGE_SNAPSHOT_BEGIN = 22,
    LAIUE_MESSAGE_SNAPSHOT_CHUNK = 23,
    LAIUE_MESSAGE_SNAPSHOT_END = 24,
    LAIUE_MESSAGE_CHUNK_RESYNC_REQUEST = 25,
    LAIUE_MESSAGE_PLAYER_JOINED = 26,
    LAIUE_MESSAGE_PLAYER_LEFT = 27,
    LAIUE_MESSAGE_PLAYER_ROSTER_ENTRY = 28,
    LAIUE_MESSAGE_WORLD_TIME = 29,
    LAIUE_MESSAGE_SYNC_READY = 30,
    LAIUE_MESSAGE_SYNC_BEGIN = 31,
    LAIUE_MESSAGE_SYNC_APPLIED = 32,
    LAIUE_MESSAGE_CHUNK_RESYNC_CANCELLED = 33,

    // Первый недопустимый номер: граница проверки заголовка. Новый тип
    // добавляется строго перед этой строкой и сразу попадает в разрешённый
    // диапазон. Раньше граница была прибита к SERVER_CONTENT_END, поэтому
    // добавленные после него drop/inventory/hotbar не проходили проверку:
    // WriteHeader возвращал 0, ChannelQueuePayload — false, и вместо
    // отправки сообщения соединение рвалось как OVERFLOW.
    LAIUE_MESSAGE_COUNT
} LaiueMessageType;

typedef struct LaiueProtocolMod
{
    char id[LAIUE_PROTOCOL_MOD_ID_CAPACITY];
    char version[LAIUE_PROTOCOL_MOD_VERSION_CAPACITY];
    uint8_t contentHash[LAIUE_PROTOCOL_MOD_HASH_SIZE];
} LaiueProtocolMod;

typedef struct LaiueProtocolFrame
{
    LaiueMessageType type;
    uint32_t sequence;
    uint32_t payloadSize;
    const uint8_t *payload;
} LaiueProtocolFrame;

typedef struct LaiueProtocolInput
{
    uint32_t sequence;
    float movementX;
    float movementY;
    float yaw;
    float pitch;
    bool jumpPressed;
    bool jumpHeld;
    bool sprintHeld;
    bool crouchHeld;
} LaiueProtocolInput;

typedef struct LaiueProtocolPlayerState
{
    uint32_t serverTick;
    uint32_t peerId;
    uint32_t lastProcessedInputSequence;
    double position[3];
    float yaw;
    float pitch;
    double locomotionVelocityX;
    double locomotionVelocityY;
    double verticalVelocity;
    double externalVelocityX;
    double externalVelocityY;
    double jumpBufferRemaining;
    double coyoteTimeRemaining;
    double colliderCrouchProgress;
    double eyeCrouchProgress;
    int32_t airJumpsRemaining;
    bool crouchingRequested;
    bool grounded;
} LaiueProtocolPlayerState;

typedef struct LaiueProtocolBlockDelta
{
    uint32_t serverTick;
    uint64_t revision;
    int64_t block[3];
    uint8_t replacement;
} LaiueProtocolBlockDelta;

#define LAIUE_PROTOCOL_INVENTORY_SLOTS 36U

typedef struct LaiueProtocolBlockDrop
{
    uint32_t id;
    double position[3];
    uint8_t block;
} LaiueProtocolBlockDrop;

typedef struct LaiueProtocolInventorySlot
{
    uint8_t item;
    uint16_t count;
} LaiueProtocolInventorySlot;

typedef struct LaiueProtocolInventory
{
    uint8_t selectedHotbarSlot;
    LaiueProtocolInventorySlot slots[LAIUE_PROTOCOL_INVENTORY_SLOTS];
} LaiueProtocolInventory;

typedef struct LaiueProtocolSnapshotBegin
{
    uint64_t snapshotId;
    uint64_t worldRevision;
    uint32_t serverTick;
    uint32_t chunkCount;
    uint32_t peerId;
    int64_t worldSeed;
    uint64_t worldTime;
    bool requiresReadyBarrier;
} LaiueProtocolSnapshotBegin;

typedef struct LaiueProtocolChunkEdit
{
    uint8_t localX;
    uint8_t localY;
    uint8_t localZ;
    uint8_t replacement;
} LaiueProtocolChunkEdit;

typedef struct LaiueProtocolChunkDelta
{
    int64_t chunk[3];
    uint64_t revision;
    // A logical 64^3 chunk may contain more edits than fit in one bounded
    // control frame. Parts are zero-based, contiguous and never interleaved.
    uint16_t partIndex;
    uint16_t partCount;
    uint16_t editCount;
    LaiueProtocolChunkEdit edits[LAIUE_PROTOCOL_MAX_CHUNK_EDITS];
} LaiueProtocolChunkDelta;

typedef struct LaiueProtocolChunkResyncRequest
{
    int64_t chunk[3];
    uint64_t expectedRevision;
} LaiueProtocolChunkResyncRequest;

bool LaiueProtocolReadHeader(const uint8_t *bytes, uint32_t size, LaiueProtocolFrame *outFrame);
uint32_t LaiueProtocolWriteHeader(uint8_t *output, uint32_t capacity,
                                  LaiueMessageType type, uint32_t sequence,
                                  uint32_t payloadSize);
uint32_t LaiueProtocolWriteFrame(uint8_t *output, uint32_t capacity, LaiueMessageType type,
                                 uint32_t sequence, const uint8_t *payload, uint32_t payloadSize);

uint32_t LaiueProtocolEncodeHello(uint8_t *output, uint32_t capacity, uint64_t nonce);
bool LaiueProtocolDecodeHello(const uint8_t *payload, uint32_t size, uint64_t *outNonce);
uint32_t LaiueProtocolEncodeWelcome(uint8_t *output, uint32_t capacity, uint32_t peerId,
                                    int64_t worldSeed, uint64_t clientNonce);
bool LaiueProtocolDecodeWelcome(const uint8_t *payload, uint32_t size, uint32_t *outPeerId,
                                int64_t *outWorldSeed, uint64_t *outClientNonce);
uint32_t LaiueProtocolEncodeModList(uint8_t *output, uint32_t capacity,
                                    uint32_t count, uint8_t flags);
bool LaiueProtocolDecodeModList(const uint8_t *payload, uint32_t size,
                                uint32_t *outCount, uint8_t *outFlags);
uint32_t LaiueProtocolEncodeMod(uint8_t *output, uint32_t capacity,
                               const LaiueProtocolMod *mod);
bool LaiueProtocolDecodeMod(const uint8_t *payload, uint32_t size,
                            LaiueProtocolMod *outMod);
uint32_t LaiueProtocolEncodeReject(uint8_t *output, uint32_t capacity,
                                   uint8_t reason);
bool LaiueProtocolDecodeReject(const uint8_t *payload, uint32_t size,
                               uint8_t *outReason);
uint32_t LaiueProtocolEncodeContentBegin(uint8_t *output, uint32_t capacity,
                                         uint64_t size, const uint8_t hash[32]);
bool LaiueProtocolDecodeContentBegin(const uint8_t *payload, uint32_t payloadSize,
                                     uint64_t *outSize, uint8_t outHash[32]);
uint32_t LaiueProtocolEncodeInput(uint8_t *output, uint32_t capacity,
                                  const LaiueProtocolInput *input);
bool LaiueProtocolDecodeInput(const uint8_t *payload, uint32_t size, LaiueProtocolInput *outInput);
uint32_t LaiueProtocolEncodeEditIntent(uint8_t *output, uint32_t capacity, bool breakBlock,
                                       bool placeBlock, uint8_t placementBlock,
                                       const float direction[3]);
bool LaiueProtocolDecodeEditIntent(const uint8_t *payload, uint32_t size, bool *outBreakBlock,
                                   bool *outPlaceBlock, uint8_t* outPlacementBlock,
                                   float outDirection[3]);
uint32_t LaiueProtocolEncodePlayerState(uint8_t *output, uint32_t capacity,
                                        const LaiueProtocolPlayerState *state);
bool LaiueProtocolDecodePlayerState(const uint8_t *payload, uint32_t size,
                                    LaiueProtocolPlayerState *outState);
uint32_t LaiueProtocolEncodeBlockDelta(uint8_t *output, uint32_t capacity,
                                       const LaiueProtocolBlockDelta *delta);
bool LaiueProtocolDecodeBlockDelta(const uint8_t *payload, uint32_t size,
                                   LaiueProtocolBlockDelta *outDelta);
uint32_t LaiueProtocolEncodeBlockDrop(uint8_t* output, uint32_t capacity,
                                     const LaiueProtocolBlockDrop* drop);
bool LaiueProtocolDecodeBlockDrop(const uint8_t* payload, uint32_t size,
                                  LaiueProtocolBlockDrop* outDrop);
uint32_t LaiueProtocolEncodeDropRemove(uint8_t* output, uint32_t capacity,
                                      uint32_t dropId);
bool LaiueProtocolDecodeDropRemove(const uint8_t* payload, uint32_t size,
                                   uint32_t* outDropId);
uint32_t LaiueProtocolEncodeInventory(uint8_t* output, uint32_t capacity,
                                     const LaiueProtocolInventory* inventory);
bool LaiueProtocolDecodeInventory(const uint8_t* payload, uint32_t size,
                                  LaiueProtocolInventory* outInventory);
uint32_t LaiueProtocolEncodeSnapshotBegin(
    uint8_t *output, uint32_t capacity,
    const LaiueProtocolSnapshotBegin *snapshot);
bool LaiueProtocolDecodeSnapshotBegin(
    const uint8_t *payload, uint32_t size,
    LaiueProtocolSnapshotBegin *outSnapshot);
uint32_t LaiueProtocolEncodeSnapshotChunk(
    uint8_t *output, uint32_t capacity,
    const LaiueProtocolChunkDelta *chunk);
bool LaiueProtocolDecodeSnapshotChunk(
    const uint8_t *payload, uint32_t size,
    LaiueProtocolChunkDelta *outChunk);
uint32_t LaiueProtocolEncodeSnapshotEnd(
    uint8_t *output, uint32_t capacity,
    uint64_t snapshotId, uint64_t worldRevision);
bool LaiueProtocolDecodeSnapshotEnd(
    const uint8_t *payload, uint32_t size,
    uint64_t *outSnapshotId, uint64_t *outWorldRevision);
uint32_t LaiueProtocolEncodeSyncApplied(
    uint8_t *output, uint32_t capacity,
    uint64_t snapshotId, uint64_t worldRevision);
bool LaiueProtocolDecodeSyncApplied(
    const uint8_t *payload, uint32_t size,
    uint64_t *outSnapshotId, uint64_t *outWorldRevision);
uint32_t LaiueProtocolEncodeChunkResyncRequest(
    uint8_t *output, uint32_t capacity,
    const LaiueProtocolChunkResyncRequest *request);
bool LaiueProtocolDecodeChunkResyncRequest(
    const uint8_t *payload, uint32_t size,
    LaiueProtocolChunkResyncRequest *outRequest);
uint32_t LaiueProtocolEncodeChunkResyncCancelled(
    uint8_t *output, uint32_t capacity,
    const LaiueProtocolChunkResyncRequest *request);
bool LaiueProtocolDecodeChunkResyncCancelled(
    const uint8_t *payload, uint32_t size,
    LaiueProtocolChunkResyncRequest *outRequest);
uint32_t LaiueProtocolEncodePeerId(
    uint8_t *output, uint32_t capacity, uint32_t peerId);
bool LaiueProtocolDecodePeerId(
    const uint8_t *payload, uint32_t size, uint32_t *outPeerId);
uint32_t LaiueProtocolEncodeWorldTime(
    uint8_t *output, uint32_t capacity, uint64_t worldTime);
bool LaiueProtocolDecodeWorldTime(
    const uint8_t *payload, uint32_t size, uint64_t *outWorldTime);
