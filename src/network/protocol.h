#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LAIUE_PROTOCOL_MAGIC 0x5549414cU /* "LAIU" little-endian */
#define LAIUE_PROTOCOL_VERSION 1U
#define LAIUE_PROTOCOL_HEADER_SIZE 16U
#define LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE 64U
#define LAIUE_PROTOCOL_MAX_FRAME_SIZE (LAIUE_PROTOCOL_HEADER_SIZE + LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE)

typedef enum LaiueMessageType
{
    LAIUE_MESSAGE_CLIENT_HELLO = 1,
    LAIUE_MESSAGE_SERVER_WELCOME = 2,
    LAIUE_MESSAGE_PLAYER_INPUT = 3,
    LAIUE_MESSAGE_EDIT_INTENT = 4,
    LAIUE_MESSAGE_PLAYER_STATE = 5,
    LAIUE_MESSAGE_BLOCK_DELTA = 6,
    LAIUE_MESSAGE_PING = 7,
    LAIUE_MESSAGE_PONG = 8
} LaiueMessageType;

typedef struct LaiueProtocolFrame
{
    LaiueMessageType type;
    uint32_t sequence;
    uint32_t payloadSize;
    const uint8_t *payload;
} LaiueProtocolFrame;

typedef struct LaiueProtocolInput
{
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
    double position[3];
    float yaw;
    float pitch;
    bool grounded;
} LaiueProtocolPlayerState;

typedef struct LaiueProtocolBlockDelta
{
    uint32_t serverTick;
    int64_t block[3];
    uint8_t replacement;
} LaiueProtocolBlockDelta;

bool LaiueProtocolReadHeader(const uint8_t *bytes, uint32_t size, LaiueProtocolFrame *outFrame);
uint32_t LaiueProtocolWriteFrame(uint8_t *output, uint32_t capacity, LaiueMessageType type,
                                 uint32_t sequence, const uint8_t *payload, uint32_t payloadSize);

uint32_t LaiueProtocolEncodeHello(uint8_t *output, uint32_t capacity, uint64_t nonce);
bool LaiueProtocolDecodeHello(const uint8_t *payload, uint32_t size, uint64_t *outNonce);
uint32_t LaiueProtocolEncodeWelcome(uint8_t *output, uint32_t capacity, uint32_t peerId,
                                    int64_t worldSeed, uint64_t clientNonce);
bool LaiueProtocolDecodeWelcome(const uint8_t *payload, uint32_t size, uint32_t *outPeerId,
                                int64_t *outWorldSeed, uint64_t *outClientNonce);
uint32_t LaiueProtocolEncodeInput(uint8_t *output, uint32_t capacity,
                                  const LaiueProtocolInput *input);
bool LaiueProtocolDecodeInput(const uint8_t *payload, uint32_t size, LaiueProtocolInput *outInput);
uint32_t LaiueProtocolEncodeEditIntent(uint8_t *output, uint32_t capacity, bool breakBlock,
                                       bool placeBlock, const float direction[3]);
bool LaiueProtocolDecodeEditIntent(const uint8_t *payload, uint32_t size, bool *outBreakBlock,
                                   bool *outPlaceBlock, float outDirection[3]);
uint32_t LaiueProtocolEncodePlayerState(uint8_t *output, uint32_t capacity,
                                        const LaiueProtocolPlayerState *state);
bool LaiueProtocolDecodePlayerState(const uint8_t *payload, uint32_t size,
                                    LaiueProtocolPlayerState *outState);
uint32_t LaiueProtocolEncodeBlockDelta(uint8_t *output, uint32_t capacity,
                                       const LaiueProtocolBlockDelta *delta);
bool LaiueProtocolDecodeBlockDelta(const uint8_t *payload, uint32_t size,
                                   LaiueProtocolBlockDelta *outDelta);
