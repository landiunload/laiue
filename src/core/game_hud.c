#include "core/game_hud.h"
#include "core/ui_format.h"

static float Maximum(float left, float right)
{
    return left > right ? left : right;
}

static void FormatPair(wchar_t* destination, uint32_t capacity,
    uint64_t left, uint64_t right, const wchar_t* suffix)
{
    UiTextBuilder builder;
    UiTextBuilderInit(&builder, destination, capacity);
    UiTextBuilderAppendUnsigned(&builder, left);
    UiTextBuilderAppendChar(&builder, L'/');
    UiTextBuilderAppendUnsigned(&builder, right);
    UiTextBuilderAppend(&builder, suffix);
}

static void SetModeText(GameHud* hud)
{
    const wchar_t* text;
    if (hud->gameMode == GAME_MODE_FLY)
    {
        text = L"Полёт";
    }
    else if (hud->crouching)
    {
        text = L"Присед";
    }
    else
    {
        text = L"Ходьба";
    }

    UiTextBuilder builder;
    UiTextBuilderInit(&builder, hud->modeText,
        (uint32_t)(sizeof(hud->modeText) / sizeof(hud->modeText[0])));
    UiTextBuilderAppend(&builder, text);
}

static void SetNetworkText(GameHud* hud)
{
    UiTextBuilder builder;
    UiTextBuilderInit(&builder, hud->networkText,
        (uint32_t)(sizeof(hud->networkText) / sizeof(hud->networkText[0])));
    if (!hud->networkConnected)
    {
        UiTextBuilderAppend(&builder, L"Offline");
        return;
    }
    UiTextBuilderAppend(&builder, L"Server #");
    UiTextBuilderAppendUnsigned(&builder, hud->networkPeerId);
}

void GameHudInit(GameHud* hud)
{
    hud->initialized = false;
    hud->framesPerSecond = 0;
    hud->timeMinutes = 0;
    hud->gameMode = GAME_MODE_FLY;
    hud->crouching = false;
    hud->networkConnected = false;
    hud->networkPeerId = 0;
    hud->diagnosticsVisible = false;
    hud->measuredPixelSize = 0;
    hud->panelWidth = 0.0f;

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        hud->blockPosition[axis] = 0;
        hud->coordinateText[axis][0] = L'\0';
    }
    hud->framesPerSecondText[0] = L'\0';
    hud->timeText[0] = L'\0';
    hud->modeText[0] = L'\0';
    hud->networkText[0] = L'\0';
    hud->meshQueueText[0] = L'\0';
    hud->buildTimeText[0] = L'\0';
    hud->wastedBuildsText[0] = L'\0';
    hud->drawText[0] = L'\0';
    hud->uploadText[0] = L'\0';
    hud->geometryPoolText[0] = L'\0';
    hud->streamingStats = (ChunkStreamingStats){ 0 };
    hud->rendererStats = (RendererStats){ 0 };
}

static void UpdateTextCache(GameHud* hud, World* world,
    const PlayerController* player, GameMode gameMode,
    uint32_t framesPerSecond, uint32_t timeMinutes,
    const ChunkStreamingStats* streamingStats,
    const RendererStats* rendererStats,
    bool diagnosticsVisible, bool networkConnected, uint32_t networkPeerId,
    const int64_t cameraBlockPosition[3])
{
    bool measurementDirty = !hud->initialized;
    if (!hud->initialized || hud->diagnosticsVisible != diagnosticsVisible)
    {
        hud->diagnosticsVisible = diagnosticsVisible;
        measurementDirty = true;
    }

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!hud->initialized
            || hud->blockPosition[axis] != cameraBlockPosition[axis])
        {
            hud->blockPosition[axis] = cameraBlockPosition[axis];
            WorldFormatAbsoluteBlockCoordinate(world, axis,
                cameraBlockPosition[axis], hud->coordinateText[axis],
                (uint32_t)(sizeof(hud->coordinateText[axis])
                    / sizeof(hud->coordinateText[axis][0])));
            measurementDirty = true;
        }
    }

    if (!hud->initialized || hud->framesPerSecond != framesPerSecond)
    {
        hud->framesPerSecond = framesPerSecond;
        UiFormatUnsigned(hud->framesPerSecondText,
            (uint32_t)(sizeof(hud->framesPerSecondText)
                / sizeof(hud->framesPerSecondText[0])),
            framesPerSecond);
        measurementDirty = true;
    }

    timeMinutes %= 1440u;
    if (!hud->initialized || hud->timeMinutes != timeMinutes)
    {
        hud->timeMinutes = timeMinutes;
        UiFormatClock(hud->timeText,
            (uint32_t)(sizeof(hud->timeText) / sizeof(hud->timeText[0])),
            timeMinutes);
        measurementDirty = true;
    }

    bool crouching = gameMode == GAME_MODE_WALK
        && PlayerControllerIsCrouching(player);
    if (!hud->initialized || hud->gameMode != gameMode
        || hud->crouching != crouching)
    {
        hud->gameMode = gameMode;
        hud->crouching = crouching;
        SetModeText(hud);
        measurementDirty = true;
    }
    if (!hud->initialized || hud->networkConnected != networkConnected
        || hud->networkPeerId != networkPeerId)
    {
        hud->networkConnected = networkConnected;
        hud->networkPeerId = networkPeerId;
        SetNetworkText(hud);
        measurementDirty = true;
    }

    uint64_t buildMicroseconds = (uint64_t)(
        streamingStats->averageBuildMilliseconds * 1000.0 + 0.5);
    uint64_t previousBuildMicroseconds = (uint64_t)(
        hud->streamingStats.averageBuildMilliseconds * 1000.0 + 0.5);
    if (!hud->initialized
        || hud->streamingStats.pendingRequests != streamingStats->pendingRequests
        || hud->streamingStats.pendingResults != streamingStats->pendingResults)
    {
        FormatPair(hud->meshQueueText,
            (uint32_t)(sizeof(hud->meshQueueText) / sizeof(wchar_t)),
            streamingStats->pendingRequests, streamingStats->pendingResults, L"");
        measurementDirty = true;
    }
    if (!hud->initialized || previousBuildMicroseconds != buildMicroseconds)
    {
        UiFormatUnsignedSuffix(hud->buildTimeText,
            (uint32_t)(sizeof(hud->buildTimeText) / sizeof(wchar_t)),
            buildMicroseconds, L" us");
        measurementDirty = true;
    }
    if (!hud->initialized
        || hud->streamingStats.cancelledBuilds != streamingStats->cancelledBuilds
        || hud->streamingStats.discardedBuilds != streamingStats->discardedBuilds)
    {
        FormatPair(hud->wastedBuildsText,
            (uint32_t)(sizeof(hud->wastedBuildsText) / sizeof(wchar_t)),
            streamingStats->cancelledBuilds, streamingStats->discardedBuilds, L"");
        measurementDirty = true;
    }
    if (!hud->initialized
        || hud->rendererStats.drawCalls != rendererStats->drawCalls
        || hud->rendererStats.drawnQuads != rendererStats->drawnQuads)
    {
        FormatPair(hud->drawText,
            (uint32_t)(sizeof(hud->drawText) / sizeof(wchar_t)),
            rendererStats->drawCalls, rendererStats->drawnQuads, L"");
        measurementDirty = true;
    }
    if (!hud->initialized
        || hud->rendererStats.uploadedBytes != rendererStats->uploadedBytes)
    {
        UiFormatUnsignedSuffix(hud->uploadText,
            (uint32_t)(sizeof(hud->uploadText) / sizeof(wchar_t)),
            (rendererStats->uploadedBytes + 1023u) / 1024u, L" KiB");
        measurementDirty = true;
    }
    if (!hud->initialized
        || hud->rendererStats.geometryPoolUsedBytes != rendererStats->geometryPoolUsedBytes
        || hud->rendererStats.geometryPoolCapacityBytes != rendererStats->geometryPoolCapacityBytes)
    {
        FormatPair(hud->geometryPoolText,
            (uint32_t)(sizeof(hud->geometryPoolText) / sizeof(wchar_t)),
            (rendererStats->geometryPoolUsedBytes + 1023u) / 1024u,
            rendererStats->geometryPoolCapacityBytes / 1024u, L" KiB");
        measurementDirty = true;
    }
    hud->streamingStats = *streamingStats;
    hud->rendererStats = *rendererStats;

    if (measurementDirty)
    {
        hud->measuredPixelSize = 0;
    }
    hud->initialized = true;
}

static void MeasurePanel(GameHud* hud, const UiContext* ui)
{
    float labelWidth = UiTextWidth(ui, L"Время");
    labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"Режим"));
    labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"FPS"));
    labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"Сеть"));
    if (hud->diagnosticsVisible)
    {
        labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"Очередь"));
        labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"Meshing"));
        labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"Отмена"));
        labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"Draw"));
        labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"Upload"));
        labelWidth = Maximum(labelWidth, UiTextWidth(ui, L"GPU mesh"));
    }

    float valueWidth = UiTextWidth(ui, hud->coordinateText[0]);
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->coordinateText[1]));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->coordinateText[2]));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->framesPerSecondText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->timeText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->modeText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->networkText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->meshQueueText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->buildTimeText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->wastedBuildsText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->drawText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->uploadText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->geometryPoolText));

    float padding = UiScaled(ui, 12.0f);
    float gap = UiScaled(ui, 18.0f);
    float minimumWidth = UiScaled(ui, 210.0f);
    float requiredWidth = padding * 2.0f + labelWidth + gap + valueWidth;
    hud->panelWidth = Maximum(minimumWidth, requiredWidth);
    hud->measuredPixelSize = ui->font.pixelSize;
}

void GameHudDraw(GameHud* hud, UiContext* ui,
    World* world, const PlayerController* player,
    GameMode gameMode, uint32_t framesPerSecond, uint32_t timeMinutes,
    const ChunkStreamingStats* streamingStats,
    const RendererStats* rendererStats,
    bool diagnosticsVisible, bool networkConnected, uint32_t networkPeerId,
    const int64_t cameraBlockPosition[3],
    int32_t viewportWidth, int32_t viewportHeight)
{
    if (hud == NULL || ui == NULL || world == NULL || player == NULL
        || streamingStats == NULL || rendererStats == NULL
        || cameraBlockPosition == NULL)
    {
        return;
    }

    UpdateTextCache(hud, world, player, gameMode,
        framesPerSecond, timeMinutes, streamingStats, rendererStats,
        diagnosticsVisible, networkConnected, networkPeerId,
        cameraBlockPosition);

    if (hud->measuredPixelSize != ui->font.pixelSize)
    {
        MeasurePanel(hud, ui);
    }

    float margin = UiScaled(ui, 12.0f);
    float padding = UiScaled(ui, 12.0f);
    float rowGap = UiScaled(ui, 4.0f);
    float titleGap = UiScaled(ui, 9.0f);
    float dividerHeight = UiScaled(ui, 1.0f);
    float rowAdvance = ui->font.lineHeight + rowGap;
    float panelHeight = padding + ui->font.lineHeight + titleGap
        + dividerHeight + titleGap
        + rowAdvance * (hud->diagnosticsVisible ? 13.0f : 7.0f)
        - rowGap + padding;

    float availableWidth = (float)viewportWidth - margin * 2.0f;
    float availableHeight = (float)viewportHeight - margin * 2.0f;
    if (availableWidth < UiScaled(ui, 140.0f)
        || availableHeight < panelHeight)
    {
        return;
    }

    float panelWidth = hud->panelWidth < availableWidth
        ? hud->panelWidth : availableWidth;
    float x = margin;
    float y = margin;

    UiPanel(ui, x, y, panelWidth, panelHeight);

    float contentX = x + padding;
    float contentWidth = panelWidth - padding * 2.0f;
    float cursorY = y + padding;

    UiText(ui, contentX, cursorY, UI_COLOR_TEXT, L"Мир");
    cursorY += ui->font.lineHeight + titleGap;
    UiRect(ui, contentX, cursorY, contentWidth, dividerHeight,
        0.0f, UI_COLOR_TRACK);
    cursorY += dividerHeight + titleGap;

    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"X", hud->coordinateText[0], rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Y", hud->coordinateText[1], rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Z", hud->coordinateText[2], rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"FPS", hud->framesPerSecondText, rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Время", hud->timeText, rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Режим", hud->modeText, rowGap);
    UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Сеть", hud->networkText,
        hud->diagnosticsVisible ? rowGap : 0.0f);
    if (!hud->diagnosticsVisible) return;
    cursorY += rowAdvance;
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Очередь", hud->meshQueueText, rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Meshing", hud->buildTimeText, rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Отмена", hud->wastedBuildsText, rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Draw", hud->drawText, rowGap);
    cursorY = UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Upload", hud->uploadText, rowGap);
    UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"GPU mesh", hud->geometryPoolText, 0.0f);
}
