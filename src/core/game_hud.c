#include "core/game_hud.h"
#include "core/ui_format.h"

static float Maximum(float left, float right)
{
    return left > right ? left : right;
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

void GameHudInit(GameHud* hud)
{
    hud->initialized = false;
    hud->framesPerSecond = 0;
    hud->timeMinutes = 0;
    hud->gameMode = GAME_MODE_FLY;
    hud->crouching = false;
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
}

static void UpdateTextCache(GameHud* hud, World* world,
    const PlayerController* player, GameMode gameMode,
    uint32_t framesPerSecond, uint32_t timeMinutes,
    const int64_t cameraBlockPosition[3])
{
    bool measurementDirty = !hud->initialized;

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

    float valueWidth = UiTextWidth(ui, hud->coordinateText[0]);
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->coordinateText[1]));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->coordinateText[2]));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->framesPerSecondText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->timeText));
    valueWidth = Maximum(valueWidth, UiTextWidth(ui, hud->modeText));

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
    const int64_t cameraBlockPosition[3],
    int32_t viewportWidth, int32_t viewportHeight)
{
    if (hud == NULL || ui == NULL || world == NULL || player == NULL
        || cameraBlockPosition == NULL)
    {
        return;
    }

    UpdateTextCache(hud, world, player, gameMode,
        framesPerSecond, timeMinutes, cameraBlockPosition);

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
        + rowAdvance * 6.0f - rowGap + padding;

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
    UiLabelValueRow(ui, contentX, contentWidth, cursorY,
        L"Режим", hud->modeText, 0.0f);
}
