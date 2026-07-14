#pragma once

#include <stdint.h>

typedef enum GameEventType
{
    GAME_EVENT_NONE,
    GAME_EVENT_PLAYER_JUMPED,
    GAME_EVENT_PLAYER_LANDED,
    GAME_EVENT_PLAYER_STANCE_CHANGED,
} GameEventType;

typedef struct GameEvent
{
    GameEventType type;
    uint64_t sequence;
} GameEvent;
