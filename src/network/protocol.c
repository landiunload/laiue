#include "network/protocol.h"

#include <stddef.h>
#include <string.h>

#define HELLO_PAYLOAD_SIZE 12U
#define WELCOME_PAYLOAD_SIZE 20U
#define INPUT_PAYLOAD_SIZE 9U
#define EDIT_PAYLOAD_SIZE 8U
#define PLAYER_STATE_PAYLOAD_SIZE 37U
#define BLOCK_DELTA_PAYLOAD_SIZE 37U
#define MOD_LIST_PAYLOAD_SIZE 3U
#define REJECT_PAYLOAD_SIZE 1U
#define CONTENT_BEGIN_PAYLOAD_SIZE 40U
#define BLOCK_DROP_PAYLOAD_SIZE 29U
#define DROP_REMOVE_PAYLOAD_SIZE 4U
#define INVENTORY_PAYLOAD_SIZE (1U + LAIUE_PROTOCOL_INVENTORY_SLOTS * 3U)
#define SNAPSHOT_BEGIN_PAYLOAD_SIZE 44U
#define SNAPSHOT_CHUNK_HEADER_SIZE 38U
#define SNAPSHOT_CHUNK_EDIT_SIZE 4U
#define SNAPSHOT_END_PAYLOAD_SIZE 16U
#define CHUNK_RESYNC_PAYLOAD_SIZE 32U
#define PEER_ID_PAYLOAD_SIZE 4U
#define WORLD_TIME_PAYLOAD_SIZE 8U

#define ANGLE_PI 3.14159265358979323846f
#define PITCH_LIMIT 1.57079632679489661923f
#define POSITION_LIMIT 1099511627776.0

static uint16_t ReadU16(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8));
}

static uint32_t ReadU32(const uint8_t *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) | ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

static uint64_t ReadU64(const uint8_t *input)
{
    uint64_t low = ReadU32(input);
    uint64_t high = ReadU32(input + 4);
    return low | (high << 32);
}

static void WriteU16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void WriteU32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void WriteU64(uint8_t *output, uint64_t value)
{
    WriteU32(output, (uint32_t)value);
    WriteU32(output + 4, (uint32_t)(value >> 32));
}

// ВАЖНО: этот файл обязан собираться с /fp:precise (см. CMake рядом с
// protocol.c). Проверки NaN здесь опираются на то, что NaN != NaN. Под
// /fp:fast компилятор считает NaN невозможным и сворачивает самосравнение в
// true; на clang-cl (там /fp:fast == -ffast-math -ffinite-math-only) это
// доказанно пропускало NaN из сети в EncodeInput. Битовая проверка не
// спасает: finite-math-only — это аксиома «неконечных нет», и оптимизатор
// применяет её и к разбору битов. Единственная надёжная защита — точная
// модель FP для этой единицы трансляции.
static bool IsFiniteFloat(float value)
{
    return value == value && value >= -3.402823466e+38f && value <= 3.402823466e+38f;
}

static bool IsFinitePosition(double value)
{
    return value == value && value >= -POSITION_LIMIT && value <= POSITION_LIMIT;
}

static int16_t QuantizeSignedUnit(float value)
{
    float scaled = value * 32767.0f;
    return (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static int16_t QuantizeAngle(float value, float limit)
{
    float scaled = value * (32767.0f / limit);
    return (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static void WriteDouble(uint8_t *output, double value)
{
    union
    {
        double floating;
        uint64_t integer;
    } bits;
    bits.floating = value;
    WriteU64(output, bits.integer);
}

static double ReadDouble(const uint8_t *input)
{
    union
    {
        double floating;
        uint64_t integer;
    } bits;
    bits.integer = ReadU64(input);
    return bits.floating;
}

bool LaiueProtocolReadHeader(const uint8_t *bytes, uint32_t size, LaiueProtocolFrame *outFrame)
{
    if (bytes == NULL || outFrame == NULL || size < LAIUE_PROTOCOL_HEADER_SIZE ||
        ReadU32(bytes) != LAIUE_PROTOCOL_MAGIC || ReadU16(bytes + 4) != LAIUE_PROTOCOL_VERSION)
    {
        return false;
    }

    uint16_t type = ReadU16(bytes + 6);
    uint32_t payloadSize = ReadU32(bytes + 8);
    uint32_t sequence = ReadU32(bytes + 12);
    if (type < LAIUE_MESSAGE_CLIENT_HELLO || type >= LAIUE_MESSAGE_COUNT ||
        payloadSize > LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE || sequence == 0)
    {
        return false;
    }

    outFrame->type = (LaiueMessageType)type;
    outFrame->sequence = sequence;
    outFrame->payloadSize = payloadSize;
    outFrame->payload = bytes + LAIUE_PROTOCOL_HEADER_SIZE;
    return true;
}

uint32_t LaiueProtocolWriteHeader(uint8_t *output, uint32_t capacity,
                                  LaiueMessageType type, uint32_t sequence,
                                  uint32_t payloadSize)
{
    if (output == NULL || sequence == 0 || type < LAIUE_MESSAGE_CLIENT_HELLO ||
        type >= LAIUE_MESSAGE_COUNT || payloadSize > LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE ||
        capacity < LAIUE_PROTOCOL_HEADER_SIZE)
    {
        return 0;
    }

    WriteU32(output, LAIUE_PROTOCOL_MAGIC);
    WriteU16(output + 4, LAIUE_PROTOCOL_VERSION);
    WriteU16(output + 6, (uint16_t)type);
    WriteU32(output + 8, payloadSize);
    WriteU32(output + 12, sequence);
    return LAIUE_PROTOCOL_HEADER_SIZE;
}

uint32_t LaiueProtocolWriteFrame(uint8_t *output, uint32_t capacity, LaiueMessageType type,
                                 uint32_t sequence, const uint8_t *payload, uint32_t payloadSize)
{
    uint32_t frameSize = LAIUE_PROTOCOL_HEADER_SIZE + payloadSize;
    if (capacity < frameSize || (payloadSize != 0 && payload == NULL)
        || LaiueProtocolWriteHeader(output, capacity, type,
            sequence, payloadSize) == 0)
    {
        return 0;
    }
    if (payloadSize != 0)
    {
        memcpy(output + LAIUE_PROTOCOL_HEADER_SIZE, payload, payloadSize);
    }
    return frameSize;
}

uint32_t LaiueProtocolEncodeHello(uint8_t *output, uint32_t capacity, uint64_t nonce)
{
    if (output == NULL || capacity < HELLO_PAYLOAD_SIZE)
    {
        return 0;
    }
    WriteU16(output, (uint16_t)LAIUE_VERSION_MAJOR);
    WriteU16(output + 2, (uint16_t)LAIUE_VERSION_MINOR);
    WriteU64(output + 4, nonce);
    return HELLO_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeHello(const uint8_t *payload, uint32_t size, uint64_t *outNonce)
{
    if (payload == NULL || outNonce == NULL || size != HELLO_PAYLOAD_SIZE ||
        ReadU16(payload) != (uint16_t)LAIUE_VERSION_MAJOR ||
        ReadU16(payload + 2) != (uint16_t)LAIUE_VERSION_MINOR)
    {
        return false;
    }
    *outNonce = ReadU64(payload + 4);
    return *outNonce != 0;
}

uint32_t LaiueProtocolEncodeWelcome(uint8_t *output, uint32_t capacity, uint32_t peerId,
                                    int64_t worldSeed, uint64_t clientNonce)
{
    if (output == NULL || capacity < WELCOME_PAYLOAD_SIZE || peerId == 0 || clientNonce == 0)
    {
        return 0;
    }
    WriteU32(output, peerId);
    WriteU64(output + 4, (uint64_t)worldSeed);
    WriteU64(output + 12, clientNonce);
    return WELCOME_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeWelcome(const uint8_t *payload, uint32_t size, uint32_t *outPeerId,
                                int64_t *outWorldSeed, uint64_t *outClientNonce)
{
    if (payload == NULL || outPeerId == NULL || outWorldSeed == NULL || outClientNonce == NULL ||
        size != WELCOME_PAYLOAD_SIZE)
    {
        return false;
    }
    *outPeerId = ReadU32(payload);
    *outWorldSeed = (int64_t)ReadU64(payload + 4);
    *outClientNonce = ReadU64(payload + 12);
    return *outPeerId != 0 && *outClientNonce != 0;
}

static uint32_t AsciiLength(const char *text, uint32_t capacity)
{
    uint32_t length = 0;
    if (text == NULL) return capacity;
    while (length < capacity && text[length] != '\0') ++length;
    return length;
}

static bool ValidModText(const char *text, uint32_t length, bool identifier)
{
    if (length == 0) return false;
    for (uint32_t i = 0; i < length; ++i)
    {
        uint8_t c = (uint8_t)text[i];
        if (identifier)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                || c == '.' || c == '_' || c == '-')) return false;
        }
        else if (c < 0x21U || c > 0x7eU)
        {
            return false;
        }
    }
    return true;
}

static bool HashIsNonzero(const uint8_t hash[LAIUE_PROTOCOL_MOD_HASH_SIZE])
{
    uint8_t combined = 0;
    for (uint32_t i = 0; i < LAIUE_PROTOCOL_MOD_HASH_SIZE; ++i)
    {
        combined |= hash[i];
    }
    return combined != 0;
}

uint32_t LaiueProtocolEncodeModList(uint8_t *output, uint32_t capacity,
                                    uint32_t count, uint8_t flags)
{
    if (output == NULL || capacity < MOD_LIST_PAYLOAD_SIZE
        || count > LAIUE_PROTOCOL_MAX_MODS) return 0;
    WriteU16(output, (uint16_t)count);
    output[2] = flags;
    return MOD_LIST_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeModList(const uint8_t *payload, uint32_t size,
                                uint32_t *outCount, uint8_t *outFlags)
{
    if (payload == NULL || outCount == NULL || outFlags == NULL
        || size != MOD_LIST_PAYLOAD_SIZE) return false;
    *outCount = ReadU16(payload);
    *outFlags = payload[2];
    return *outCount <= LAIUE_PROTOCOL_MAX_MODS;
}

uint32_t LaiueProtocolEncodeMod(uint8_t *output, uint32_t capacity,
                               const LaiueProtocolMod *mod)
{
    if (output == NULL || mod == NULL) return 0;
    uint32_t idLength = AsciiLength(mod->id, LAIUE_PROTOCOL_MOD_ID_CAPACITY);
    uint32_t versionLength = AsciiLength(mod->version,
        LAIUE_PROTOCOL_MOD_VERSION_CAPACITY);
    uint32_t size = 2U + LAIUE_PROTOCOL_MOD_HASH_SIZE
        + idLength + versionLength;
    if (idLength >= LAIUE_PROTOCOL_MOD_ID_CAPACITY
        || versionLength >= LAIUE_PROTOCOL_MOD_VERSION_CAPACITY
        || !ValidModText(mod->id, idLength, true)
        || !ValidModText(mod->version, versionLength, false)
        || !HashIsNonzero(mod->contentHash) || capacity < size) return 0;
    output[0] = (uint8_t)idLength;
    output[1] = (uint8_t)versionLength;
    memcpy(output + 2, mod->contentHash, LAIUE_PROTOCOL_MOD_HASH_SIZE);
    uint32_t textOffset = 2U + LAIUE_PROTOCOL_MOD_HASH_SIZE;
    memcpy(output + textOffset, mod->id, idLength);
    memcpy(output + textOffset + idLength, mod->version, versionLength);
    return size;
}

bool LaiueProtocolDecodeMod(const uint8_t *payload, uint32_t size,
                            LaiueProtocolMod *outMod)
{
    if (payload == NULL || outMod == NULL
        || size < 4U + LAIUE_PROTOCOL_MOD_HASH_SIZE) return false;
    uint32_t idLength = payload[0];
    uint32_t versionLength = payload[1];
    if (idLength == 0 || idLength >= LAIUE_PROTOCOL_MOD_ID_CAPACITY
        || versionLength == 0
        || versionLength >= LAIUE_PROTOCOL_MOD_VERSION_CAPACITY
        || size != 2U + LAIUE_PROTOCOL_MOD_HASH_SIZE
            + idLength + versionLength) return false;
    memset(outMod, 0, sizeof(*outMod));
    memcpy(outMod->contentHash, payload + 2,
        LAIUE_PROTOCOL_MOD_HASH_SIZE);
    uint32_t textOffset = 2U + LAIUE_PROTOCOL_MOD_HASH_SIZE;
    memcpy(outMod->id, payload + textOffset, idLength);
    memcpy(outMod->version, payload + textOffset + idLength, versionLength);
    return HashIsNonzero(outMod->contentHash)
        && ValidModText(outMod->id, idLength, true)
        && ValidModText(outMod->version, versionLength, false);
}

uint32_t LaiueProtocolEncodeReject(uint8_t *output, uint32_t capacity,
                                   uint8_t reason)
{
    if (output == NULL || capacity < REJECT_PAYLOAD_SIZE || reason == 0) return 0;
    output[0] = reason;
    return REJECT_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeReject(const uint8_t *payload, uint32_t size,
                               uint8_t *outReason)
{
    if (payload == NULL || outReason == NULL || size != REJECT_PAYLOAD_SIZE
        || payload[0] == 0) return false;
    *outReason = payload[0];
    return true;
}

uint32_t LaiueProtocolEncodeContentBegin(uint8_t *output, uint32_t capacity,
                                         uint64_t size, const uint8_t hash[32])
{
    if (output == NULL || hash == NULL || size == 0
        || capacity < CONTENT_BEGIN_PAYLOAD_SIZE) return 0;
    WriteU64(output, size);
    memcpy(output + 8, hash, 32);
    return CONTENT_BEGIN_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeContentBegin(const uint8_t *payload, uint32_t payloadSize,
                                     uint64_t *outSize, uint8_t outHash[32])
{
    if (payload == NULL || outSize == NULL || outHash == NULL
        || payloadSize != CONTENT_BEGIN_PAYLOAD_SIZE) return false;
    *outSize = ReadU64(payload);
    memcpy(outHash, payload + 8, 32);
    return *outSize != 0;
}

uint32_t LaiueProtocolEncodeInput(uint8_t *output, uint32_t capacity,
                                  const LaiueProtocolInput *input)
{
    if (output == NULL || input == NULL || capacity < INPUT_PAYLOAD_SIZE ||
        !IsFiniteFloat(input->movementX) || !IsFiniteFloat(input->movementY) ||
        !IsFiniteFloat(input->yaw) || !IsFiniteFloat(input->pitch) || input->movementX < -1.0f ||
        input->movementX > 1.0f || input->movementY < -1.0f || input->movementY > 1.0f ||
        input->yaw < -ANGLE_PI || input->yaw > ANGLE_PI || input->pitch < -PITCH_LIMIT ||
        input->pitch > PITCH_LIMIT)
    {
        return 0;
    }

    WriteU16(output, (uint16_t)QuantizeSignedUnit(input->movementX));
    WriteU16(output + 2, (uint16_t)QuantizeSignedUnit(input->movementY));
    WriteU16(output + 4, (uint16_t)QuantizeAngle(input->yaw, ANGLE_PI));
    WriteU16(output + 6, (uint16_t)QuantizeAngle(input->pitch, PITCH_LIMIT));
    output[8] = (uint8_t)((input->jumpPressed ? 1U : 0U) | (input->jumpHeld ? 2U : 0U) |
                          (input->sprintHeld ? 4U : 0U) | (input->crouchHeld ? 8U : 0U));
    return INPUT_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeInput(const uint8_t *payload, uint32_t size, LaiueProtocolInput *outInput)
{
    if (payload == NULL || outInput == NULL || size != INPUT_PAYLOAD_SIZE ||
        (payload[8] & 0xf0U) != 0)
    {
        return false;
    }

    outInput->movementX = (float)(int16_t)ReadU16(payload) / 32767.0f;
    outInput->movementY = (float)(int16_t)ReadU16(payload + 2) / 32767.0f;
    outInput->yaw = (float)(int16_t)ReadU16(payload + 4) * (ANGLE_PI / 32767.0f);
    outInput->pitch = (float)(int16_t)ReadU16(payload + 6) * (PITCH_LIMIT / 32767.0f);
    outInput->jumpPressed = (payload[8] & 1U) != 0;
    outInput->jumpHeld = (payload[8] & 2U) != 0;
    outInput->sprintHeld = (payload[8] & 4U) != 0;
    outInput->crouchHeld = (payload[8] & 8U) != 0;

    float lengthSquared =
        outInput->movementX * outInput->movementX + outInput->movementY * outInput->movementY;
    return lengthSquared <= 1.01f;
}

uint32_t LaiueProtocolEncodeEditIntent(uint8_t *output, uint32_t capacity, bool breakBlock,
                                       bool placeBlock, uint8_t placementBlock,
                                       const float direction[3])
{
    if (output == NULL || capacity < EDIT_PAYLOAD_SIZE || breakBlock == placeBlock ||
        (placeBlock && (placementBlock == 0U || placementBlock > 2U)) ||
        direction == NULL || !IsFiniteFloat(direction[0]) || !IsFiniteFloat(direction[1]) ||
        !IsFiniteFloat(direction[2]) || direction[0] < -1.0f || direction[0] > 1.0f ||
        direction[1] < -1.0f || direction[1] > 1.0f || direction[2] < -1.0f || direction[2] > 1.0f)
    {
        return 0;
    }
    float lengthSquared =
        direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
    if (lengthSquared < 0.98f || lengthSquared > 1.02f)
    {
        return 0;
    }
    output[0] = breakBlock ? 1U : 2U;
    WriteU16(output + 1, (uint16_t)QuantizeSignedUnit(direction[0]));
    WriteU16(output + 3, (uint16_t)QuantizeSignedUnit(direction[1]));
    WriteU16(output + 5, (uint16_t)QuantizeSignedUnit(direction[2]));
    output[7] = placeBlock ? placementBlock : 0U;
    return EDIT_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeEditIntent(const uint8_t *payload, uint32_t size, bool *outBreakBlock,
                                   bool *outPlaceBlock, uint8_t* outPlacementBlock,
                                   float outDirection[3])
{
    if (payload == NULL || outBreakBlock == NULL || outPlaceBlock == NULL
        || outPlacementBlock == NULL || outDirection == NULL
        || size != EDIT_PAYLOAD_SIZE || (payload[0] != 1U && payload[0] != 2U)
        || (payload[0] == 1U && payload[7] != 0U)
        || (payload[0] == 2U && (payload[7] == 0U || payload[7] > 2U)))
    {
        return false;
    }
    *outBreakBlock = payload[0] == 1U;
    *outPlaceBlock = payload[0] == 2U;
    *outPlacementBlock = payload[7];
    outDirection[0] = (float)(int16_t)ReadU16(payload + 1) / 32767.0f;
    outDirection[1] = (float)(int16_t)ReadU16(payload + 3) / 32767.0f;
    outDirection[2] = (float)(int16_t)ReadU16(payload + 5) / 32767.0f;
    float lengthSquared = outDirection[0] * outDirection[0] + outDirection[1] * outDirection[1] +
                          outDirection[2] * outDirection[2];
    return lengthSquared >= 0.98f && lengthSquared <= 1.02f;
}

uint32_t LaiueProtocolEncodePlayerState(uint8_t *output, uint32_t capacity,
                                        const LaiueProtocolPlayerState *state)
{
    if (output == NULL || state == NULL || capacity < PLAYER_STATE_PAYLOAD_SIZE ||
        state->peerId == 0 || !IsFinitePosition(state->position[0]) ||
        !IsFinitePosition(state->position[1]) || !IsFinitePosition(state->position[2]) ||
        !IsFiniteFloat(state->yaw) || !IsFiniteFloat(state->pitch) || state->yaw < -ANGLE_PI ||
        state->yaw > ANGLE_PI || state->pitch < -PITCH_LIMIT || state->pitch > PITCH_LIMIT)
    {
        return 0;
    }

    WriteU32(output, state->serverTick);
    WriteU32(output + 4, state->peerId);
    WriteDouble(output + 8, state->position[0]);
    WriteDouble(output + 16, state->position[1]);
    WriteDouble(output + 24, state->position[2]);
    WriteU16(output + 32, (uint16_t)QuantizeAngle(state->yaw, ANGLE_PI));
    WriteU16(output + 34, (uint16_t)QuantizeAngle(state->pitch, PITCH_LIMIT));
    output[36] = state->grounded ? 1U : 0U;
    return PLAYER_STATE_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodePlayerState(const uint8_t *payload, uint32_t size,
                                    LaiueProtocolPlayerState *outState)
{
    if (payload == NULL || outState == NULL || size != PLAYER_STATE_PAYLOAD_SIZE ||
        payload[36] > 1U)
    {
        return false;
    }
    outState->serverTick = ReadU32(payload);
    outState->peerId = ReadU32(payload + 4);
    outState->position[0] = ReadDouble(payload + 8);
    outState->position[1] = ReadDouble(payload + 16);
    outState->position[2] = ReadDouble(payload + 24);
    outState->yaw = (float)(int16_t)ReadU16(payload + 32) * (ANGLE_PI / 32767.0f);
    outState->pitch = (float)(int16_t)ReadU16(payload + 34) * (PITCH_LIMIT / 32767.0f);
    outState->grounded = payload[36] != 0;
    return outState->peerId != 0 && IsFinitePosition(outState->position[0]) &&
           IsFinitePosition(outState->position[1]) && IsFinitePosition(outState->position[2]);
}

uint32_t LaiueProtocolEncodeBlockDelta(uint8_t *output, uint32_t capacity,
                                       const LaiueProtocolBlockDelta *delta)
{
    if (output == NULL || delta == NULL || capacity < BLOCK_DELTA_PAYLOAD_SIZE ||
        delta->revision == 0 || delta->replacement > 2U)
    {
        return 0;
    }
    WriteU32(output, delta->serverTick);
    WriteU64(output + 4, delta->revision);
    WriteU64(output + 12, (uint64_t)delta->block[0]);
    WriteU64(output + 20, (uint64_t)delta->block[1]);
    WriteU64(output + 28, (uint64_t)delta->block[2]);
    output[36] = delta->replacement;
    return BLOCK_DELTA_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeBlockDelta(const uint8_t *payload, uint32_t size,
                                   LaiueProtocolBlockDelta *outDelta)
{
    if (payload == NULL || outDelta == NULL ||
        size != BLOCK_DELTA_PAYLOAD_SIZE || payload[36] > 2U)
    {
        return false;
    }
    outDelta->serverTick = ReadU32(payload);
    outDelta->revision = ReadU64(payload + 4);
    outDelta->block[0] = (int64_t)ReadU64(payload + 12);
    outDelta->block[1] = (int64_t)ReadU64(payload + 20);
    outDelta->block[2] = (int64_t)ReadU64(payload + 28);
    outDelta->replacement = payload[36];
    return outDelta->revision != 0;
}

uint32_t LaiueProtocolEncodeBlockDrop(uint8_t* output, uint32_t capacity,
    const LaiueProtocolBlockDrop* drop)
{
    if (output == NULL || drop == NULL || capacity < BLOCK_DROP_PAYLOAD_SIZE
        || drop->id == 0 || drop->block == 0 || drop->block > 2
        || !IsFinitePosition(drop->position[0])
        || !IsFinitePosition(drop->position[1])
        || !IsFinitePosition(drop->position[2])) return 0;
    WriteU32(output, drop->id);
    output[4] = drop->block;
    WriteDouble(output + 5, drop->position[0]);
    WriteDouble(output + 13, drop->position[1]);
    WriteDouble(output + 21, drop->position[2]);
    return BLOCK_DROP_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeBlockDrop(const uint8_t* payload, uint32_t size,
    LaiueProtocolBlockDrop* outDrop)
{
    if (payload == NULL || outDrop == NULL || size != BLOCK_DROP_PAYLOAD_SIZE
        || payload[4] == 0 || payload[4] > 2) return false;
    outDrop->id = ReadU32(payload);
    outDrop->block = payload[4];
    outDrop->position[0] = ReadDouble(payload + 5);
    outDrop->position[1] = ReadDouble(payload + 13);
    outDrop->position[2] = ReadDouble(payload + 21);
    return outDrop->id != 0
        && IsFinitePosition(outDrop->position[0])
        && IsFinitePosition(outDrop->position[1])
        && IsFinitePosition(outDrop->position[2]);
}

uint32_t LaiueProtocolEncodeDropRemove(uint8_t* output, uint32_t capacity,
    uint32_t dropId)
{
    if (output == NULL || capacity < DROP_REMOVE_PAYLOAD_SIZE || dropId == 0)
        return 0;
    WriteU32(output, dropId);
    return DROP_REMOVE_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeDropRemove(const uint8_t* payload, uint32_t size,
    uint32_t* outDropId)
{
    if (payload == NULL || outDropId == NULL
        || size != DROP_REMOVE_PAYLOAD_SIZE) return false;
    *outDropId = ReadU32(payload);
    return *outDropId != 0;
}

uint32_t LaiueProtocolEncodeInventory(uint8_t* output, uint32_t capacity,
    const LaiueProtocolInventory* inventory)
{
    if (output == NULL || inventory == NULL
        || capacity < INVENTORY_PAYLOAD_SIZE
        || inventory->selectedHotbarSlot >= 9U) return 0;
    output[0] = inventory->selectedHotbarSlot;
    for (uint32_t i = 0; i < LAIUE_PROTOCOL_INVENTORY_SLOTS; ++i)
    {
        const LaiueProtocolInventorySlot* slot = &inventory->slots[i];
        if (slot->count > 64U
            || (slot->count == 0 && slot->item != 0)
            || (slot->count != 0 && (slot->item == 0 || slot->item > 2)))
            return 0;
        output[1U + i * 3U] = slot->item;
        WriteU16(output + 2U + i * 3U, slot->count);
    }
    return INVENTORY_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeInventory(const uint8_t* payload, uint32_t size,
    LaiueProtocolInventory* outInventory)
{
    if (payload == NULL || outInventory == NULL
        || size != INVENTORY_PAYLOAD_SIZE || payload[0] >= 9U) return false;
    outInventory->selectedHotbarSlot = payload[0];
    for (uint32_t i = 0; i < LAIUE_PROTOCOL_INVENTORY_SLOTS; ++i)
    {
        uint8_t item = payload[1U + i * 3U];
        uint16_t count = ReadU16(payload + 2U + i * 3U);
        if (count > 64U || (count == 0 && item != 0)
            || (count != 0 && (item == 0 || item > 2))) return false;
        outInventory->slots[i].item = item;
        outInventory->slots[i].count = count;
    }
    return true;
}

uint32_t LaiueProtocolEncodeSnapshotBegin(
    uint8_t *output, uint32_t capacity,
    const LaiueProtocolSnapshotBegin *snapshot)
{
    if (output == NULL || snapshot == NULL ||
        capacity < SNAPSHOT_BEGIN_PAYLOAD_SIZE ||
        snapshot->snapshotId == 0 ||
        snapshot->peerId == 0 ||
        snapshot->chunkCount > LAIUE_PROTOCOL_MAX_SNAPSHOT_CHUNKS)
    {
        return 0;
    }
    WriteU64(output, snapshot->snapshotId);
    WriteU64(output + 8, snapshot->worldRevision);
    WriteU32(output + 16, snapshot->serverTick);
    WriteU32(output + 20, snapshot->chunkCount);
    WriteU32(output + 24, snapshot->peerId);
    WriteU64(output + 28, (uint64_t)snapshot->worldSeed);
    WriteU64(output + 36, snapshot->worldTime);
    return SNAPSHOT_BEGIN_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeSnapshotBegin(
    const uint8_t *payload, uint32_t size,
    LaiueProtocolSnapshotBegin *outSnapshot)
{
    if (payload == NULL || outSnapshot == NULL ||
        size != SNAPSHOT_BEGIN_PAYLOAD_SIZE)
    {
        return false;
    }
    outSnapshot->snapshotId = ReadU64(payload);
    outSnapshot->worldRevision = ReadU64(payload + 8);
    outSnapshot->serverTick = ReadU32(payload + 16);
    outSnapshot->chunkCount = ReadU32(payload + 20);
    outSnapshot->peerId = ReadU32(payload + 24);
    outSnapshot->worldSeed = (int64_t)ReadU64(payload + 28);
    outSnapshot->worldTime = ReadU64(payload + 36);
    return outSnapshot->snapshotId != 0 && outSnapshot->peerId != 0 &&
           outSnapshot->chunkCount <= LAIUE_PROTOCOL_MAX_SNAPSHOT_CHUNKS;
}

static uint32_t ChunkEditIndex(const LaiueProtocolChunkEdit *edit)
{
    return (uint32_t)edit->localX *
               LAIUE_PROTOCOL_CHUNK_SIZE * LAIUE_PROTOCOL_CHUNK_SIZE +
           (uint32_t)edit->localY * LAIUE_PROTOCOL_CHUNK_SIZE +
           (uint32_t)edit->localZ;
}

static bool ChunkEditsAreCanonical(
    const LaiueProtocolChunkEdit *edits, uint32_t count)
{
    uint32_t previous = 0;
    for (uint32_t index = 0; index < count; ++index)
    {
        const LaiueProtocolChunkEdit *edit = &edits[index];
        if (edit->localX >= LAIUE_PROTOCOL_CHUNK_SIZE ||
            edit->localY >= LAIUE_PROTOCOL_CHUNK_SIZE ||
            edit->localZ >= LAIUE_PROTOCOL_CHUNK_SIZE ||
            edit->replacement > 2U)
        {
            return false;
        }
        uint32_t current = ChunkEditIndex(edit);
        if (index != 0 && current <= previous)
        {
            return false;
        }
        previous = current;
    }
    return true;
}

uint32_t LaiueProtocolEncodeSnapshotChunk(
    uint8_t *output, uint32_t capacity,
    const LaiueProtocolChunkDelta *chunk)
{
    if (output == NULL || chunk == NULL ||
        chunk->revision == 0 ||
        chunk->partCount == 0 ||
        chunk->partCount > LAIUE_PROTOCOL_MAX_CHUNK_PARTS ||
        chunk->partIndex >= chunk->partCount ||
        (chunk->editCount == 0 &&
         (chunk->partIndex != 0 || chunk->partCount != 1)) ||
        (chunk->partIndex + 1U < chunk->partCount &&
         chunk->editCount != LAIUE_PROTOCOL_MAX_CHUNK_EDITS) ||
        chunk->editCount > LAIUE_PROTOCOL_MAX_CHUNK_EDITS ||
        !ChunkEditsAreCanonical(chunk->edits, chunk->editCount))
    {
        return 0;
    }
    uint32_t size = SNAPSHOT_CHUNK_HEADER_SIZE +
                    (uint32_t)chunk->editCount * SNAPSHOT_CHUNK_EDIT_SIZE;
    if (capacity < size || size > LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE)
    {
        return 0;
    }
    WriteU64(output, (uint64_t)chunk->chunk[0]);
    WriteU64(output + 8, (uint64_t)chunk->chunk[1]);
    WriteU64(output + 16, (uint64_t)chunk->chunk[2]);
    WriteU64(output + 24, chunk->revision);
    WriteU16(output + 32, chunk->partIndex);
    WriteU16(output + 34, chunk->partCount);
    WriteU16(output + 36, chunk->editCount);
    for (uint32_t index = 0; index < chunk->editCount; ++index)
    {
        uint8_t *encoded =
            output + SNAPSHOT_CHUNK_HEADER_SIZE +
            index * SNAPSHOT_CHUNK_EDIT_SIZE;
        encoded[0] = chunk->edits[index].localX;
        encoded[1] = chunk->edits[index].localY;
        encoded[2] = chunk->edits[index].localZ;
        encoded[3] = chunk->edits[index].replacement;
    }
    return size;
}

bool LaiueProtocolDecodeSnapshotChunk(
    const uint8_t *payload, uint32_t size,
    LaiueProtocolChunkDelta *outChunk)
{
    if (payload == NULL || outChunk == NULL ||
        size < SNAPSHOT_CHUNK_HEADER_SIZE)
    {
        return false;
    }
    uint32_t partIndex = ReadU16(payload + 32);
    uint32_t partCount = ReadU16(payload + 34);
    uint32_t editCount = ReadU16(payload + 36);
    uint32_t expectedSize = SNAPSHOT_CHUNK_HEADER_SIZE +
                            editCount * SNAPSHOT_CHUNK_EDIT_SIZE;
    if (partCount == 0 ||
        partCount > LAIUE_PROTOCOL_MAX_CHUNK_PARTS ||
        partIndex >= partCount ||
        (editCount == 0 &&
         (partIndex != 0 || partCount != 1)) ||
        (partIndex + 1U < partCount &&
         editCount != LAIUE_PROTOCOL_MAX_CHUNK_EDITS) ||
        editCount > LAIUE_PROTOCOL_MAX_CHUNK_EDITS ||
        size != expectedSize)
    {
        return false;
    }
    outChunk->chunk[0] = (int64_t)ReadU64(payload);
    outChunk->chunk[1] = (int64_t)ReadU64(payload + 8);
    outChunk->chunk[2] = (int64_t)ReadU64(payload + 16);
    outChunk->revision = ReadU64(payload + 24);
    outChunk->partIndex = (uint16_t)partIndex;
    outChunk->partCount = (uint16_t)partCount;
    outChunk->editCount = (uint16_t)editCount;
    for (uint32_t index = 0; index < editCount; ++index)
    {
        const uint8_t *encoded =
            payload + SNAPSHOT_CHUNK_HEADER_SIZE +
            index * SNAPSHOT_CHUNK_EDIT_SIZE;
        outChunk->edits[index].localX = encoded[0];
        outChunk->edits[index].localY = encoded[1];
        outChunk->edits[index].localZ = encoded[2];
        outChunk->edits[index].replacement = encoded[3];
    }
    return outChunk->revision != 0 &&
           ChunkEditsAreCanonical(outChunk->edits, editCount);
}

uint32_t LaiueProtocolEncodeSnapshotEnd(
    uint8_t *output, uint32_t capacity,
    uint64_t snapshotId, uint64_t worldRevision)
{
    if (output == NULL || capacity < SNAPSHOT_END_PAYLOAD_SIZE ||
        snapshotId == 0)
    {
        return 0;
    }
    WriteU64(output, snapshotId);
    WriteU64(output + 8, worldRevision);
    return SNAPSHOT_END_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeSnapshotEnd(
    const uint8_t *payload, uint32_t size,
    uint64_t *outSnapshotId, uint64_t *outWorldRevision)
{
    if (payload == NULL || outSnapshotId == NULL ||
        outWorldRevision == NULL ||
        size != SNAPSHOT_END_PAYLOAD_SIZE)
    {
        return false;
    }
    *outSnapshotId = ReadU64(payload);
    *outWorldRevision = ReadU64(payload + 8);
    return *outSnapshotId != 0;
}

uint32_t LaiueProtocolEncodeChunkResyncRequest(
    uint8_t *output, uint32_t capacity,
    const LaiueProtocolChunkResyncRequest *request)
{
    if (output == NULL || request == NULL ||
        capacity < CHUNK_RESYNC_PAYLOAD_SIZE)
    {
        return 0;
    }
    WriteU64(output, (uint64_t)request->chunk[0]);
    WriteU64(output + 8, (uint64_t)request->chunk[1]);
    WriteU64(output + 16, (uint64_t)request->chunk[2]);
    WriteU64(output + 24, request->expectedRevision);
    return CHUNK_RESYNC_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeChunkResyncRequest(
    const uint8_t *payload, uint32_t size,
    LaiueProtocolChunkResyncRequest *outRequest)
{
    if (payload == NULL || outRequest == NULL ||
        size != CHUNK_RESYNC_PAYLOAD_SIZE)
    {
        return false;
    }
    outRequest->chunk[0] = (int64_t)ReadU64(payload);
    outRequest->chunk[1] = (int64_t)ReadU64(payload + 8);
    outRequest->chunk[2] = (int64_t)ReadU64(payload + 16);
    outRequest->expectedRevision = ReadU64(payload + 24);
    return true;
}

uint32_t LaiueProtocolEncodePeerId(
    uint8_t *output, uint32_t capacity, uint32_t peerId)
{
    if (output == NULL || capacity < PEER_ID_PAYLOAD_SIZE || peerId == 0)
    {
        return 0;
    }
    WriteU32(output, peerId);
    return PEER_ID_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodePeerId(
    const uint8_t *payload, uint32_t size, uint32_t *outPeerId)
{
    if (payload == NULL || outPeerId == NULL ||
        size != PEER_ID_PAYLOAD_SIZE)
    {
        return false;
    }
    *outPeerId = ReadU32(payload);
    return *outPeerId != 0;
}

uint32_t LaiueProtocolEncodeWorldTime(
    uint8_t *output, uint32_t capacity, uint64_t worldTime)
{
    if (output == NULL || capacity < WORLD_TIME_PAYLOAD_SIZE)
    {
        return 0;
    }
    WriteU64(output, worldTime);
    return WORLD_TIME_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeWorldTime(
    const uint8_t *payload, uint32_t size, uint64_t *outWorldTime)
{
    if (payload == NULL || outWorldTime == NULL ||
        size != WORLD_TIME_PAYLOAD_SIZE)
    {
        return false;
    }
    *outWorldTime = ReadU64(payload);
    return true;
}
