#pragma once

#include "core/ui.h"
#include "core/chunk_streaming.h"
#include "gameplay/game_mode.h"
#include "gameplay/player_controller.h"
#include "render/renderer.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

// Кешируемая модель HUD. Форматирование координат и значений выполняется
// только при изменении данных; квады всё равно собираются каждый кадр.
typedef struct GameHud
{
    bool initialized;
    int64_t blockPosition[3];
    uint32_t framesPerSecond;
    uint32_t timeMinutes;
    GameMode gameMode;
    bool crouching;
    bool networkConnected;
    uint32_t networkPeerId;
    bool diagnosticsVisible;

    int32_t measuredPixelSize;
    float panelWidth;

    wchar_t coordinateText[3][32];
    wchar_t framesPerSecondText[16];
    wchar_t timeText[8];
    wchar_t modeText[24];
    wchar_t networkText[24];
    wchar_t meshQueueText[24];
    wchar_t buildTimeText[24];
    wchar_t wastedBuildsText[24];
    wchar_t drawText[32];
    wchar_t uploadText[24];
    wchar_t geometryPoolText[32];
    ChunkStreamingStats streamingStats;
    RendererStats rendererStats;
} GameHud;

void GameHudInit(GameHud* hud);

void GameHudDraw(GameHud* hud, UiContext* ui,
    World* world, const PlayerController* player,
    GameMode gameMode, uint32_t framesPerSecond, uint32_t timeMinutes,
    const ChunkStreamingStats* streamingStats,
    const RendererStats* rendererStats,
    bool diagnosticsVisible, bool networkConnected, uint32_t networkPeerId,
    const int64_t cameraBlockPosition[3],
    int32_t viewportWidth, int32_t viewportHeight);
