#include "core/pause_menu.h"
#include "core/ui_format.h"

// Идентификаторы виджетов (стабильны между кадрами — на них живут анимации).
enum
{
    WIDGET_RESUME = 1,
    WIDGET_SETTINGS,
    WIDGET_QUIT,
    WIDGET_BACK,
    WIDGET_TABS = 8,             // + индекс вкладки
    WIDGET_FOV_SLIDER = 16,
    WIDGET_VSYNC,
    WIDGET_TIME_SLIDER,
    WIDGET_SENSITIVITY_SLIDER,
    WIDGET_FLY_SPEED_SLIDER,
    WIDGET_PROJECTION_FIRST = 32,
    WIDGET_TIME_SPEED_FIRST = 48,
};

enum
{
    SETTINGS_TAB_GRAPHICS = 0,
    SETTINGS_TAB_ADMIN,
    SETTINGS_TAB_CONTROLS,
    SETTINGS_TAB_COUNT,
};

static const wchar_t* const TAB_LABELS[SETTINGS_TAB_COUNT] = {
    L"Графика",
    L"Админ",
    L"Управление",
};

static const wchar_t* const PROJECTION_LABELS[RENDER_PROJECTION_COUNT] = {
    L"Авто",
    L"Перспектива",
    L"Рыбий глаз",
    L"Панорама (цилиндр)",
};

#define MENU_TEXT_CAPACITY 48

void PauseMenuOpen(PauseMenu* menu)
{
    menu->screen = PAUSE_MENU_MAIN;
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
    UiPanel(ui, panelX, panelY, panelWidth, panelHeight);

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

static float GraphicsTabHeight(const UiContext* ui)
{
    float s = ui->scale;
    float line = ui->font.lineHeight;
    return (line + 6.0f * s) + (20.0f * s + 4.0f * s) + (line + 12.0f * s)
        + (line + 6.0f * s)
        + 30.0f * s * 4.0f + 4.0f * s * 3.0f + 12.0f * s
        + 30.0f * s + 14.0f * s;
}

static float AdminTabHeight(const UiContext* ui)
{
    float s = ui->scale;
    float line = ui->font.lineHeight;
    return (line + 6.0f * s) + (20.0f * s + 4.0f * s) + (line + 12.0f * s)
        + (line + 6.0f * s)
        + 30.0f * s * 4.0f + 4.0f * s * 3.0f + 14.0f * s;
}

static float ControlsTabHeight(const UiContext* ui)
{
    float s = ui->scale;
    float line = ui->font.lineHeight;
    return ((line + 6.0f * s) + (20.0f * s + 12.0f * s)) * 2.0f + 2.0f * s;
}

static float DrawGraphicsTab(UiContext* ui, GameSettings* settings,
    Renderer* renderer, float x, float width, float y)
{
    float s = ui->scale;
    wchar_t text[MENU_TEXT_CAPACITY];

    UiFormatDegrees(text, MENU_TEXT_CAPACITY, settings->fovDegrees);
    y = UiLabelValueRow(ui, x, width, y,
        L"Поле зрения", text, UiScaled(ui, 6.0f));

    int32_t fov = settings->fovDegrees;
    if (UiSliderInt(ui, WIDGET_FOV_SLIDER, x, y, width, 1, 360, &fov))
    {
        settings->fovDegrees = fov;
    }
    y += 20.0f * s + 4.0f * s;

    UiText(ui, x, y, UiColor(150, 158, 172, 255), ProjectionCaption(settings));
    y += ui->font.lineHeight + 12.0f * s;

    UiText(ui, x, y, UiColor(150, 158, 172, 255), L"Рендер");
    y += ui->font.lineHeight + 6.0f * s;

    for (int32_t projection = 0; projection < RENDER_PROJECTION_COUNT;
         ++projection)
    {
        if (UiRadioRow(ui, WIDGET_PROJECTION_FIRST + (uint32_t)projection,
                x, y, width, 30.0f * s, PROJECTION_LABELS[projection],
                settings->projection == (RenderProjection)projection))
        {
            settings->projection = (RenderProjection)projection;
        }
        y += 30.0f * s
            + (projection + 1 < RENDER_PROJECTION_COUNT ? 4.0f * s : 12.0f * s);
    }

    float rowHeight = 30.0f * s;
    float toggleTop = y + (rowHeight - UiScaled(ui, 22.0f)) * 0.5f;
    UiText(ui, x, y + (rowHeight - ui->font.lineHeight) * 0.5f,
        UiColor(232, 236, 244, 255), L"Верт. синхронизация");
    bool verticalSync = RendererIsVerticalSyncEnabled(renderer);
    if (UiToggle(ui, WIDGET_VSYNC, x + width - UiScaled(ui, 40.0f), toggleTop,
            &verticalSync))
    {
        RendererSetVerticalSync(renderer, verticalSync);
    }
    return y + rowHeight + 14.0f * s;
}

static float DrawAdminTab(UiContext* ui, GameSettings* settings,
    float dayLengthMinutes, float x, float width, float y)
{
    float s = ui->scale;
    wchar_t text[MENU_TEXT_CAPACITY];

    int32_t timeMinutes = (int32_t)(settings->timeOfDayHours * 60.0f + 0.5f);
    if (timeMinutes > 1439) timeMinutes = 1439;
    if (timeMinutes < 0) timeMinutes = 0;

    UiFormatClock(text, MENU_TEXT_CAPACITY, (uint32_t)timeMinutes);
    y = UiLabelValueRow(ui, x, width, y,
        L"Время суток", text, UiScaled(ui, 6.0f));

    if (UiSliderInt(ui, WIDGET_TIME_SLIDER, x, y, width, 0, 1439, &timeMinutes))
    {
        settings->timeOfDayHours = (float)timeMinutes / 60.0f;
    }
    y += 20.0f * s + 4.0f * s;

    UiText(ui, x, y, UiColor(150, 158, 172, 255),
        L"рассвет 06:00 · закат 18:00");
    y += ui->font.lineHeight + 12.0f * s;

    UiText(ui, x, y, UiColor(150, 158, 172, 255), L"Скорость времени");
    y += ui->font.lineHeight + 6.0f * s;

    wchar_t normalLabel[MENU_TEXT_CAPACITY];
    UiTextBuilder builder;
    UiTextBuilderInit(&builder, normalLabel, MENU_TEXT_CAPACITY);
    UiTextBuilderAppend(&builder, L"Обычная — сутки за ");
    UiTextBuilderAppendUnsigned(&builder,
        (uint32_t)(dayLengthMinutes + 0.5f));
    UiTextBuilderAppend(&builder, L" мин");

    const wchar_t* speedLabels[TIME_SPEED_COUNT] = {
        L"Пауза",
        normalLabel,
        L"Быстрая — сутки за 2 мин",
        L"Реальное время",
    };

    for (int32_t speed = 0; speed < TIME_SPEED_COUNT; ++speed)
    {
        if (UiRadioRow(ui, WIDGET_TIME_SPEED_FIRST + (uint32_t)speed,
                x, y, width, 30.0f * s, speedLabels[speed],
                settings->timeSpeed == (TimeSpeedPreset)speed))
        {
            settings->timeSpeed = (TimeSpeedPreset)speed;
        }
        y += 30.0f * s + (speed + 1 < TIME_SPEED_COUNT ? 4.0f * s : 14.0f * s);
    }
    return y;
}

static float DrawControlsTab(UiContext* ui, GameSettings* settings,
    float x, float width, float y)
{
    float s = ui->scale;
    wchar_t text[MENU_TEXT_CAPACITY];

    UiFormatUnsignedSuffix(text, MENU_TEXT_CAPACITY,
        (uint32_t)settings->mouseSensitivityPercent, L"%");
    y = UiLabelValueRow(ui, x, width, y,
        L"Чувствительность мыши", text, UiScaled(ui, 6.0f));
    UiSliderInt(ui, WIDGET_SENSITIVITY_SLIDER, x, y, width,
        25, 300, &settings->mouseSensitivityPercent);
    y += 20.0f * s + 12.0f * s;

    UiFormatUnsignedSuffix(text, MENU_TEXT_CAPACITY,
        (uint32_t)settings->flySpeedBlocks, L" бл/с");
    y = UiLabelValueRow(ui, x, width, y,
        L"Скорость полёта", text, UiScaled(ui, 6.0f));
    UiSliderInt(ui, WIDGET_FLY_SPEED_SLIDER, x, y, width,
        10, 200, &settings->flySpeedBlocks);
    return y + 20.0f * s + 12.0f * s + 2.0f * s;
}

static void UpdateSettingsScreen(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer, float dayLengthMinutes,
    int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 340.0f * s;
    float padding = 20.0f * s;
    float lineHeight = ui->font.lineHeight;
    float tabsHeight = 32.0f * s;
    float buttonHeight = 36.0f * s;

    float contentHeight;
    switch (menu->settingsTab)
    {
        case SETTINGS_TAB_ADMIN:    contentHeight = AdminTabHeight(ui); break;
        case SETTINGS_TAB_CONTROLS: contentHeight = ControlsTabHeight(ui); break;
        default:                    contentHeight = GraphicsTabHeight(ui); break;
    }

    float panelHeight = padding + lineHeight + 12.0f * s
        + tabsHeight + 12.0f * s + contentHeight + buttonHeight + padding;

    float panelX = ((float)width - panelWidth) * 0.5f;
    float panelY = ((float)height - panelHeight) * 0.5f;
    UiPanel(ui, panelX, panelY, panelWidth, panelHeight);

    float contentX = panelX + padding;
    float contentWidth = panelWidth - padding * 2.0f;
    float cursorY = panelY + padding;

    UiTextCentered(ui, panelX + panelWidth * 0.5f, cursorY,
        UiColor(232, 236, 244, 255), L"Настройки");
    cursorY += lineHeight + 12.0f * s;

    UiSegmented(ui, WIDGET_TABS, contentX, cursorY, contentWidth, tabsHeight,
        TAB_LABELS, SETTINGS_TAB_COUNT, &menu->settingsTab);
    cursorY += tabsHeight + 12.0f * s;

    switch (menu->settingsTab)
    {
        case SETTINGS_TAB_ADMIN:
            cursorY = DrawAdminTab(ui, settings, dayLengthMinutes,
                contentX, contentWidth, cursorY);
            break;
        case SETTINGS_TAB_CONTROLS:
            cursorY = DrawControlsTab(ui, settings,
                contentX, contentWidth, cursorY);
            break;
        default:
            cursorY = DrawGraphicsTab(ui, settings, renderer,
                contentX, contentWidth, cursorY);
            break;
    }

    if (UiButton(ui, WIDGET_BACK, contentX, cursorY,
            contentWidth, buttonHeight, L"Назад"))
    {
        menu->screen = PAUSE_MENU_MAIN;
    }
}

PauseMenuAction PauseMenuUpdate(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer, float dayLengthMinutes,
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

    UpdateSettingsScreen(menu, ui, settings, renderer, dayLengthMinutes,
        width, height);
    return PAUSE_MENU_ACTION_NONE;
}
