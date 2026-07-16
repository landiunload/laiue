#include "core/pause_menu.h"
#include "render/shader_pack.h"
#include "render/texture_pack.h"

#include <windows.h>
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
    WIDGET_WIREFRAME,
    WIDGET_GAMMA_SLIDER,
    WIDGET_SHADER_LIST_FIRST = 80,
    WIDGET_SHADER_APPLY,
    WIDGET_TEXTURE_LIST_FIRST = 96,
    WIDGET_TEXTURE_APPLY,
    WIDGET_FULLSCREEN = 120,
    WIDGET_SHADER_RESET,
    WIDGET_TEXTURE_RESET,
};

enum
{
    SETTINGS_TAB_GRAPHICS = 0,
    SETTINGS_TAB_PACKS,
    SETTINGS_TAB_ADMIN,
    SETTINGS_TAB_CONTROLS,
    SETTINGS_TAB_COUNT,
};

static const wchar_t* const TAB_LABELS[SETTINGS_TAB_COUNT] = {
    L"Графика",
    L"Паки",
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
        + 30.0f * s + 14.0f * s
        + 30.0f * s + 14.0f * s
        + (22.0f * s + 12.0f * s) + (line + 6.0f * s) + (20.0f * s + 12.0f * s);
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
    Renderer* renderer, Window* window, float x, float width, float y)
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
    y += rowHeight + 14.0f * s;

    // Полноэкранный режим без рамки: окно занимает весь текущий монитор,
    // поэтому доступно любое разрешение дисплея.
    float fullscreenTop = y + (rowHeight - UiScaled(ui, 22.0f)) * 0.5f;
    UiText(ui, x, y + (rowHeight - ui->font.lineHeight) * 0.5f,
        UI_COLOR_TEXT, L"Полный экран");
    bool fullscreen = WindowIsFullscreen(window);
    if (UiToggle(ui, WIDGET_FULLSCREEN, x + width - UiScaled(ui, 40.0f),
            fullscreenTop, &fullscreen))
    {
        WindowSetFullscreen(window, fullscreen);
    }
    y += rowHeight + 14.0f * s;

    // === Настройки шейдера ===
    UiText(ui, x, y, UiColor(150, 158, 172, 255), L"Шейдер");
    y += ui->font.lineHeight + 6.0f * s;

    float toggleRowHeight = 22.0f * s;
    float toggleRowTop = y + (toggleRowHeight - UiScaled(ui, 22.0f)) * 0.5f;
    UiText(ui, x, y + (toggleRowHeight - ui->font.lineHeight) * 0.5f,
        UiColor(232, 236, 244, 255), L"Каркасный режим");
    bool wireframe = settings->wireframe;
    if (UiToggle(ui, WIDGET_WIREFRAME, x + width - UiScaled(ui, 40.0f), toggleRowTop,
            &wireframe))
    {
        settings->wireframe = wireframe;
        RendererSetWireframe(renderer, wireframe);
    }
    y += toggleRowHeight + 12.0f * s;

    UiFormatUnsignedSuffix(text, MENU_TEXT_CAPACITY,
        (uint32_t)settings->gamma, L"%");
    y = UiLabelValueRow(ui, x, width, y,
        L"Гамма", text, UiScaled(ui, 6.0f));
    UiSliderInt(ui, WIDGET_GAMMA_SLIDER, x, y, width,
        50, 150, &settings->gamma);
    y += 20.0f * s + 12.0f * s;

    return y;
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

// === Паки (шейдерпаки + текстурпаки) ===

static float PacksTabHeight(const UiContext* ui)
{
    float s = ui->scale;
    float line = ui->font.lineHeight;
    // Заголовок шейдерпаков + до 5 строк + кнопка Apply (если есть)
    // + заголовок текстурпаков + до 5 строк + кнопка Apply (если есть)
    float sectionHeader = line + 12.0f * s;
    float packRows = 30.0f * s * 6.0f + 4.0f * s * 5.0f;
    float applyButton = 36.0f * s + 10.0f * s;
    float resetButton = 30.0f * s + 8.0f * s;
    float separator = 12.0f * s;
    return (sectionHeader + packRows + applyButton + resetButton + separator)
        + (sectionHeader + packRows + applyButton + resetButton);
}

static float DrawPacksTab(UiContext* ui, GameSettings* settings,
    Renderer* renderer, float x, float width, float y)
{
    float s = ui->scale;
    float lineHeight = ui->font.lineHeight;

    // ─── Шейдерпаки ───
    UiText(ui, x, y, UiColor(232, 236, 244, 255), L"Шейдерпаки");
    UiText(ui, x + width - UiTextWidth(ui, L".lsp"), y,
        UiColor(108, 148, 255, 255), L".lsp");
    y += lineHeight + 12.0f * s;

    ShaderPackList shaderList;
    if (ShaderPackEnumerate(&shaderList))
    {
        for (uint32_t i = 0; i < shaderList.count && i < 6; ++i)
        {
            uint32_t widgetId = WIDGET_SHADER_LIST_FIRST + i;
            bool isSelected = (int32_t)i == settings->selectedShaderPack;
            bool isActive = shaderList.entries[i].active;

            uint32_t textColor = isActive
                ? UiColor(108, 148, 255, 255)
                : (isSelected ? UiColor(232, 236, 244, 255)
                              : UiColor(180, 188, 200, 255));

            UiText(ui, x, y + (30.0f * s - lineHeight) * 0.5f, textColor,
                shaderList.entries[i].name);

            if (isActive)
            {
                UiText(ui, x + width - UiTextWidth(ui, L"✓"), y,
                    UiColor(80, 200, 80, 255), L"✓");
            }

            if (UiRadioRow(ui, widgetId, x, y, width, 30.0f * s,
                    L"", isSelected || isActive) && !isActive)
            {
                settings->selectedShaderPack = (int32_t)i;
            }

            y += 30.0f * s + 4.0f * s;
        }
        ShaderPackListRelease(&shaderList);
    }

    y += 4.0f * s;

    if (settings->selectedShaderPack >= 0)
    {
        if (UiButton(ui, WIDGET_SHADER_APPLY, x, y,
                width, 36.0f * s, L"Применить шейдерпак"))
        {
            int32_t index = settings->selectedShaderPack;
            // Повторно перечисляем, чтобы получить имя
            ShaderPackList sl;
            if (ShaderPackEnumerate(&sl) && (uint32_t)index < sl.count)
            {
                if (ShaderPackActivate(sl.entries[index].name))
                {
                    void* shaders[6];
                    uint32_t lengths[6];
                    if (ShaderPackLoadActiveBytecode(
                            &shaders[0], &lengths[0],
                            &shaders[1], &lengths[1],
                            &shaders[2], &lengths[2],
                            &shaders[3], &lengths[3],
                            &shaders[4], &lengths[4],
                            &shaders[5], &lengths[5]))
                    {
                        RendererReloadShaders(renderer,
                            shaders[0], lengths[0],
                            shaders[1], lengths[1],
                            shaders[2], lengths[2],
                            shaders[3], lengths[3],
                            shaders[4], lengths[4],
                            shaders[5], lengths[5]);
                        for (int32_t si = 0; si < 6; ++si)
                        {
                            if (shaders[si] != NULL)
                                HeapFree(GetProcessHeap(), 0, shaders[si]);
                        }
                    }
                    else
                    {
                        RendererReloadShaders(renderer,
                            NULL,0, NULL,0, NULL,0, NULL,0, NULL,0, NULL,0);
                    }
                    settings->selectedShaderPack = -1;
                }
            }
            ShaderPackListRelease(&sl);
        }
        y += 36.0f * s + 10.0f * s;
    }

    if (UiButton(ui, WIDGET_SHADER_RESET, x, y,
            width, 30.0f * s, L"Встроенные шейдеры"))
    {
        ShaderPackActivate(NULL);
        RendererReloadShaders(renderer,
            NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
        settings->selectedShaderPack = -1;
    }
    y += 30.0f * s + 8.0f * s;

    y += 8.0f * s;

    // ─── Текстурпаки ───
    UiText(ui, x, y, UiColor(232, 236, 244, 255), L"Текстурпаки");
    UiText(ui, x + width - UiTextWidth(ui, L".ltp"), y,
        UiColor(108, 148, 255, 255), L".ltp");
    y += lineHeight + 12.0f * s;

    TexturePackList texList;
    if (TexturePackEnumerate(&texList))
    {
        for (uint32_t i = 0; i < texList.count && i < 6; ++i)
        {
            uint32_t widgetId = WIDGET_TEXTURE_LIST_FIRST + i;
            bool isSelected = (int32_t)i == settings->selectedTexturePack;
            bool isActive = texList.entries[i].active;

            uint32_t textColor = isActive
                ? UiColor(108, 148, 255, 255)
                : (isSelected ? UiColor(232, 236, 244, 255)
                              : UiColor(180, 188, 200, 255));

            UiText(ui, x, y + (30.0f * s - lineHeight) * 0.5f, textColor,
                texList.entries[i].name);

            if (isActive)
            {
                UiText(ui, x + width - UiTextWidth(ui, L"✓"), y,
                    UiColor(80, 200, 80, 255), L"✓");
            }

            if (UiRadioRow(ui, widgetId, x, y, width, 30.0f * s,
                    L"", isSelected || isActive) && !isActive)
            {
                settings->selectedTexturePack = (int32_t)i;
            }

            y += 30.0f * s + 4.0f * s;
        }
        TexturePackListRelease(&texList);
    }

    y += 4.0f * s;

    if (settings->selectedTexturePack >= 0)
    {
        if (UiButton(ui, WIDGET_TEXTURE_APPLY, x, y,
                width, 36.0f * s, L"Применить текстурпак"))
        {
            int32_t index = settings->selectedTexturePack;
            TexturePackList tl;
            if (TexturePackEnumerate(&tl) && (uint32_t)index < tl.count)
            {
                if (TexturePackActivate(tl.entries[index].name))
                {
                    RendererReloadTexturePack(renderer);
                    settings->selectedTexturePack = -1;
                }
            }
            TexturePackListRelease(&tl);
        }
        y += 36.0f * s + 10.0f * s;
    }

    if (UiButton(ui, WIDGET_TEXTURE_RESET, x, y,
            width, 30.0f * s, L"Встроенные текстуры"))
    {
        TexturePackActivate(NULL);
        RendererReloadTexturePack(renderer);
        settings->selectedTexturePack = -1;
    }
    y += 30.0f * s + 8.0f * s;

    return y;
}

static void UpdateSettingsScreen(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer, Window* window,
    float dayLengthMinutes, int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 400.0f * s;
    float padding = 20.0f * s;
    float lineHeight = ui->font.lineHeight;
    float tabsHeight = 36.0f * s;
    float buttonHeight = 36.0f * s;

    float contentHeight;
    switch (menu->settingsTab)
    {
        case SETTINGS_TAB_PACKS:    contentHeight = PacksTabHeight(ui); break;
        case SETTINGS_TAB_ADMIN:    contentHeight = AdminTabHeight(ui); break;
        case SETTINGS_TAB_CONTROLS: contentHeight = ControlsTabHeight(ui); break;
        default:                    contentHeight = GraphicsTabHeight(ui); break;
    }

    // Панель не выше окна: на маленьких разрешениях шапка и кнопка «Назад»
    // остаются на месте, а середина прокручивается колесом мыши.
    float desiredHeight = padding + lineHeight + 12.0f * s
        + tabsHeight + 12.0f * s + contentHeight + buttonHeight + padding;
    float maxHeight = (float)height - 16.0f * s;
    float panelHeight = desiredHeight < maxHeight ? desiredHeight : maxHeight;

    float panelX = ((float)width - panelWidth) * 0.5f;
    float panelY = ((float)height - panelHeight) * 0.5f;
    UiPanel(ui, panelX, panelY, panelWidth, panelHeight);

    float contentX = panelX + padding;
    float contentWidth = panelWidth - padding * 2.0f;
    float cursorY = panelY + padding;

    UiTextCentered(ui, panelX + panelWidth * 0.5f, cursorY,
        UI_COLOR_TEXT, L"Настройки");
    cursorY += lineHeight + 12.0f * s;

    UiSegmented(ui, WIDGET_TABS, contentX, cursorY, contentWidth, tabsHeight,
        TAB_LABELS, SETTINGS_TAB_COUNT, &menu->settingsTab);
    cursorY += tabsHeight + 12.0f * s;

    // Видимая область контента между вкладками и кнопкой «Назад».
    float viewportTop = cursorY;
    float viewportBottom = panelY + panelHeight - padding
        - buttonHeight - 10.0f * s;
    float viewportHeight = viewportBottom - viewportTop;
    if (viewportHeight < 40.0f * s)
    {
        viewportHeight = 40.0f * s;
        viewportBottom = viewportTop + viewportHeight;
    }

    float maxScroll = contentHeight - viewportHeight;
    if (maxScroll < 0.0f)
    {
        maxScroll = 0.0f;
    }
    menu->settingsScroll -= ui->wheelSteps * 48.0f * s;
    if (menu->settingsScroll < 0.0f) menu->settingsScroll = 0.0f;
    if (menu->settingsScroll > maxScroll) menu->settingsScroll = maxScroll;

    UiSetClip(ui, contentX, viewportTop, contentWidth, viewportHeight);
    float scrolledY = viewportTop - menu->settingsScroll;
    switch (menu->settingsTab)
    {
        case SETTINGS_TAB_PACKS:
            DrawPacksTab(ui, settings, renderer,
                contentX, contentWidth, scrolledY);
            break;
        case SETTINGS_TAB_ADMIN:
            DrawAdminTab(ui, settings, dayLengthMinutes,
                contentX, contentWidth, scrolledY);
            break;
        case SETTINGS_TAB_CONTROLS:
            DrawControlsTab(ui, settings,
                contentX, contentWidth, scrolledY);
            break;
        default:
            DrawGraphicsTab(ui, settings, renderer, window,
                contentX, contentWidth, scrolledY);
            break;
    }
    UiClearClip(ui);

    // Полоса прокрутки: подсказывает, что ниже есть ещё настройки.
    if (maxScroll > 0.0f)
    {
        float trackX = panelX + panelWidth - 6.0f * s;
        float thumbHeight = viewportHeight * (viewportHeight / contentHeight);
        if (thumbHeight < 24.0f * s) thumbHeight = 24.0f * s;
        float thumbTravel = viewportHeight - thumbHeight;
        float thumbY = viewportTop
            + thumbTravel * (maxScroll > 0.0f
                ? menu->settingsScroll / maxScroll : 0.0f);
        UiRect(ui, trackX, viewportTop, 3.0f * s, viewportHeight,
            1.5f * s, UiColor(255, 255, 255, 24));
        UiRect(ui, trackX, thumbY, 3.0f * s, thumbHeight,
            1.5f * s, UiColor(255, 255, 255, 90));
    }

    if (UiButton(ui, WIDGET_BACK, contentX, viewportBottom + 10.0f * s,
            contentWidth, buttonHeight, L"Назад"))
    {
        menu->screen = PAUSE_MENU_MAIN;
    }
}

PauseMenuAction PauseMenuUpdate(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer, Window* window,
    float dayLengthMinutes, int32_t width, int32_t height,
    bool escapePressed)
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

    UpdateSettingsScreen(menu, ui, settings, renderer, window,
        dayLengthMinutes, width, height);
    return PAUSE_MENU_ACTION_NONE;
}
