#include "network/protocol.h"

#include <stddef.h>
#include <string.h>

#define HELLO_PAYLOAD_SIZE 12U
#define WELCOME_PAYLOAD_SIZE 20U
#define INPUT_PAYLOAD_SIZE 9U
#define EDIT_PAYLOAD_SIZE 7U
#define PLAYER_STATE_PAYLOAD_SIZE 37U
#define BLOCK_DELTA_PAYLOAD_SIZE 29U

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
    if (type < LAIUE_MESSAGE_CLIENT_HELLO || type > LAIUE_MESSAGE_PONG ||
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

uint32_t LaiueProtocolWriteFrame(uint8_t *output, uint32_t capacity, LaiueMessageType type,
                                 uint32_t sequence, const uint8_t *payload, uint32_t payloadSize)
{
    uint32_t frameSize = LAIUE_PROTOCOL_HEADER_SIZE + payloadSize;
    if (output == NULL || sequence == 0 || type < LAIUE_MESSAGE_CLIENT_HELLO ||
        type > LAIUE_MESSAGE_PONG || payloadSize > LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE ||
        capacity < frameSize || (payloadSize != 0 && payload == NULL))
    {
        return 0;
    }

    WriteU32(output, LAIUE_PROTOCOL_MAGIC);
    WriteU16(output + 4, LAIUE_PROTOCOL_VERSION);
    WriteU16(output + 6, (uint16_t)type);
    WriteU32(output + 8, payloadSize);
    WriteU32(output + 12, sequence);
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
                                       bool placeBlock, const float direction[3])
{
    if (output == NULL || capacity < EDIT_PAYLOAD_SIZE || breakBlock == placeBlock ||
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
    return EDIT_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeEditIntent(const uint8_t *payload, uint32_t size, bool *outBreakBlock,
                                   bool *outPlaceBlock, float outDirection[3])
{
    if (payload == NULL || outBreakBlock == NULL || outPlaceBlock == NULL || outDirection == NULL ||
        size != EDIT_PAYLOAD_SIZE || (payload[0] != 1U && payload[0] != 2U))
    {
        return false;
    }
    *outBreakBlock = payload[0] == 1U;
    *outPlaceBlock = payload[0] == 2U;
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
        delta->replacement > 2U)
    {
        return 0;
    }
    WriteU32(output, delta->serverTick);
    WriteU64(output + 4, (uint64_t)delta->block[0]);
    WriteU64(output + 12, (uint64_t)delta->block[1]);
    WriteU64(output + 20, (uint64_t)delta->block[2]);
    output[28] = delta->replacement;
    return BLOCK_DELTA_PAYLOAD_SIZE;
}

bool LaiueProtocolDecodeBlockDelta(const uint8_t *payload, uint32_t size,
                                   LaiueProtocolBlockDelta *outDelta)
{
    if (payload == NULL || outDelta == NULL || size != BLOCK_DELTA_PAYLOAD_SIZE || payload[28] > 2U)
    {
        return false;
    }
    outDelta->serverTick = ReadU32(payload);
    outDelta->block[0] = (int64_t)ReadU64(payload + 4);
    outDelta->block[1] = (int64_t)ReadU64(payload + 12);
    outDelta->block[2] = (int64_t)ReadU64(payload + 20);
    outDelta->replacement = payload[28];
    return true;
}
