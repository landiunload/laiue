#include "core/pause_menu.h"

// Идентификаторы виджетов (стабильны между кадрами — на них живут анимации).
enum
{
    WIDGET_RESUME = 1,
    WIDGET_SETTINGS,
    WIDGET_QUIT,
    WIDGET_BACK,
    WIDGET_FOV_SLIDER,
    WIDGET_VSYNC,
    WIDGET_PROJECTION_FIRST = 16,
};

static const wchar_t* const PROJECTION_LABELS[RENDER_PROJECTION_COUNT] = {
    L"Авто",
    L"Перспектива",
    L"Рыбий глаз",
    L"Панорама (цилиндр)",
};

static void FormatDegrees(wchar_t* buffer, uint32_t capacity, int32_t value)
{
    wchar_t digits[8];
    uint32_t digitCount = 0;
    if (value <= 0)
    {
        digits[digitCount++] = L'0';
    }
    while (value > 0 && digitCount < 7)
    {
        digits[digitCount++] = (wchar_t)(L'0' + value % 10);
        value /= 10;
    }

    uint32_t out = 0;
    while (digitCount > 0 && out + 2 < capacity)
    {
        buffer[out++] = digits[--digitCount];
    }
    buffer[out++] = 0x00B0; // знак градуса
    buffer[out] = L'\0';
}

void PauseMenuOpen(PauseMenu* menu)
{
    menu->screen = PAUSE_MENU_MAIN;
}

static void DrawPanel(UiContext* ui, float x, float y,
    float width, float height)
{
    float shadow = UiScaled(ui, 7.0f);
    UiRect(ui, x - shadow, y - shadow + UiScaled(ui, 3.0f),
        width + shadow * 2.0f, height + shadow * 2.0f,
        UiScaled(ui, 20.0f), UiColor(0, 0, 0, 90));
    UiRect(ui, x, y, width, height,
        UiScaled(ui, 14.0f), UiColor(22, 26, 34, 244));
}

static PauseMenuAction UpdateMainScreen(PauseMenu* menu, UiContext* ui,
    int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 250.0f * s;
    float buttonHeight = 36.0f * s;
    float buttonGap = 10.0f * s;
    float padding = 20.0f * s;
    float titleHeight = ui->font.lineHeight;
    float panelHeight = padding + titleHeight + 16.0f * s
        + buttonHeight * 3.0f + buttonGap * 2.0f + padding;

    float panelX = ((float)width - panelWidth) * 0.5f;
    float panelY = ((float)height - panelHeight) * 0.5f;
    DrawPanel(ui, panelX, panelY, panelWidth, panelHeight);

    float cursorY = panelY + padding;
    UiTextCentered(ui, panelX + panelWidth * 0.5f, cursorY,
        UiColor(232, 236, 244, 255), L"Пауза");
    cursorY += titleHeight + 16.0f * s;

    float buttonX = panelX + padding;
    float buttonWidth = panelWidth - padding * 2.0f;

    PauseMenuAction action = PAUSE_MENU_ACTION_NONE;
    if (UiButton(ui, WIDGET_RESUME, buttonX, cursorY,
            buttonWidth, buttonHeight, L"Продолжить"))
    {
        action = PAUSE_MENU_ACTION_RESUME;
    }
    cursorY += buttonHeight + buttonGap;

    if (UiButton(ui, WIDGET_SETTINGS, buttonX, cursorY,
            buttonWidth, buttonHeight, L"Настройки"))
    {
        menu->screen = PAUSE_MENU_SETTINGS;
    }
    cursorY += buttonHeight + buttonGap;

    if (UiButton(ui, WIDGET_QUIT, buttonX, cursorY,
            buttonWidth, buttonHeight, L"Выход"))
    {
        action = PAUSE_MENU_ACTION_QUIT;
    }

    return action;
}

static const wchar_t* ProjectionCaption(const GameSettings* settings)
{
    switch (settings->projection)
    {
        case RENDER_PROJECTION_PERSPECTIVE:
            return (float)settings->fovDegrees > PANORAMA_PERSPECTIVE_MAX_DEGREES
                ? L"перспектива ограничена 170°"
                : L"классический однопроходный рендер";
        case RENDER_PROJECTION_FISHEYE:
            return L"равноудалённая проекция, честные 360";
        case RENDER_PROJECTION_CYLINDER:
            return L"вертикальные линии остаются прямыми";
        default:
            return PanoramaIsActive(RENDER_PROJECTION_AUTO,
                       (float)settings->fovDegrees)
                ? L"сейчас: рыбий глаз (широкий угол)"
                : L"сейчас: перспектива (без накладных расходов)";
    }
}

static void UpdateSettingsScreen(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer,
    int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 330.0f * s;
    float padding = 20.0f * s;
    float lineHeight = ui->font.lineHeight;
    float rowHeight = 30.0f * s;
    float rowGap = 4.0f * s;
    float buttonHeight = 36.0f * s;

    float panelHeight = padding
        + lineHeight + 14.0f * s                    // заголовок
        + lineHeight + 6.0f * s                     // подпись + значение
        + 20.0f * s + 4.0f * s                      // ползунок
        + lineHeight + 12.0f * s                    // строка режима
        + lineHeight + 6.0f * s                     // подпись «Рендер»
        + rowHeight * 4.0f + rowGap * 3.0f + 12.0f * s
        + rowHeight + 14.0f * s                     // верт. синхронизация
        + buttonHeight + padding;

    float panelX = ((float)width - panelWidth) * 0.5f;
    float panelY = ((float)height - panelHeight) * 0.5f;
    DrawPanel(ui, panelX, panelY, panelWidth, panelHeight);

    float contentX = panelX + padding;
    float contentWidth = panelWidth - padding * 2.0f;
    float cursorY = panelY + padding;

    UiTextCentered(ui, panelX + panelWidth * 0.5f, cursorY,
        UiColor(232, 236, 244, 255), L"Настройки");
    cursorY += lineHeight + 14.0f * s;

    // Поле зрения: подпись, значение справа, ползунок 0..360.
    UiText(ui, contentX, cursorY, UiColor(232, 236, 244, 255), L"Поле зрения");
    wchar_t degrees[8];
    FormatDegrees(degrees, 8, settings->fovDegrees);
    UiText(ui, contentX + contentWidth - UiTextWidth(ui, degrees), cursorY,
        UiColor(108, 148, 255, 255), degrees);
    cursorY += lineHeight + 6.0f * s;

    int32_t fov = settings->fovDegrees;
    if (UiSliderInt(ui, WIDGET_FOV_SLIDER, contentX, cursorY,
            contentWidth, 0, 360, &fov))
    {
        settings->fovDegrees = fov;
    }
    cursorY += 20.0f * s + 4.0f * s;

    UiText(ui, contentX, cursorY, UiColor(150, 158, 172, 255),
        ProjectionCaption(settings));
    cursorY += lineHeight + 12.0f * s;

    // Выбор рендера из списка.
    UiText(ui, contentX, cursorY, UiColor(150, 158, 172, 255), L"Рендер");
    cursorY += lineHeight + 6.0f * s;

    for (int32_t projection = 0; projection < RENDER_PROJECTION_COUNT;
         ++projection)
    {
        if (UiRadioRow(ui, WIDGET_PROJECTION_FIRST + (uint32_t)projection,
                contentX, cursorY, contentWidth, rowHeight,
                PROJECTION_LABELS[projection],
                settings->projection == (RenderProjection)projection))
        {
            settings->projection = (RenderProjection)projection;
        }
        cursorY += rowHeight + (projection + 1 < RENDER_PROJECTION_COUNT
            ? rowGap : 12.0f * s);
    }

    // Вертикальная синхронизация.
    float toggleTop = cursorY + (rowHeight - UiScaled(ui, 22.0f)) * 0.5f;
    UiText(ui, contentX, cursorY + (rowHeight - lineHeight) * 0.5f,
        UiColor(232, 236, 244, 255), L"Верт. синхронизация");
    bool verticalSync = RendererIsVerticalSyncEnabled(renderer);
    if (UiToggle(ui, WIDGET_VSYNC,
            contentX + contentWidth - UiScaled(ui, 40.0f), toggleTop,
            &verticalSync))
    {
        RendererSetVerticalSync(renderer, verticalSync);
    }
    cursorY += rowHeight + 14.0f * s;

    if (UiButton(ui, WIDGET_BACK, contentX, cursorY,
            contentWidth, buttonHeight, L"Назад"))
    {
        menu->screen = PAUSE_MENU_MAIN;
    }
}

PauseMenuAction PauseMenuUpdate(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer,
    int32_t width, int32_t height, bool escapePressed)
{
    if (menu->screen == PAUSE_MENU_CLOSED)
    {
        return PAUSE_MENU_ACTION_NONE;
    }

    if (escapePressed)
    {
        if (menu->screen == PAUSE_MENU_SETTINGS)
        {
            menu->screen = PAUSE_MENU_MAIN;
        }
        else
        {
            return PAUSE_MENU_ACTION_RESUME;
        }
    }

    // Затемнение мира под меню.
    UiRect(ui, 0.0f, 0.0f, (float)width, (float)height,
        0.0f, UiColor(8, 10, 16, 150));

    if (menu->screen == PAUSE_MENU_MAIN)
    {
        return UpdateMainScreen(menu, ui, width, height);
    }

    UpdateSettingsScreen(menu, ui, settings, renderer, width, height);
    return PAUSE_MENU_ACTION_NONE;
}
