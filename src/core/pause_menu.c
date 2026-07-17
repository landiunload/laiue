#include "core/pause_menu.h"
#include "core/save_game.h"
#include "core/server_list.h"
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
    WIDGET_RETURN_TITLE,
    WIDGET_BACK,
    WIDGET_TABS = 8,             // + индекс вкладки (два ряда)
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
    WIDGET_MOD_TOGGLE_FIRST = 160,
    WIDGET_SAVE_WORLD = 200,
    WIDGET_TITLE_SINGLE = 220,
    WIDGET_TITLE_MULTI,
    WIDGET_TITLE_SETTINGS,
    WIDGET_TITLE_QUIT,
    WIDGET_WORLD_FIRST = 240,
    WIDGET_WORLD_NEW = 276,
    WIDGET_SERVER_FIRST = 280,
    WIDGET_MENU_BACK = 300,
    WIDGET_MODS_APPLY = 310,
    WIDGET_CONTENT_DOWNLOAD,
    WIDGET_CONNECT_CANCEL,
};

enum
{
    SETTINGS_TAB_GRAPHICS = 0,
    SETTINGS_TAB_TEXTURES,
    SETTINGS_TAB_SHADERS,
    SETTINGS_TAB_MODS,
    SETTINGS_TAB_ADMIN,
    SETTINGS_TAB_CONTROLS,
    SETTINGS_TAB_COUNT,
};

// Шесть вкладок в два ряда по три сегмента.
static const wchar_t* const TAB_LABELS_TOP[3] = {
    L"Графика",
    L"Текстуры",
    L"Шейдеры",
};
static const wchar_t* const TAB_LABELS_BOTTOM[3] = {
    L"Моды",
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

void PauseMenuOpenTitle(PauseMenu* menu)
{
    menu->screen = PAUSE_MENU_TITLE;
    menu->settingsReturnScreen = PAUSE_MENU_TITLE;
}

void PauseMenuShowModCompatibility(PauseMenu* menu, uint32_t count,
    bool installed, bool downloadsAllowed)
{
    menu->requiredServerModCount = count;
    menu->requiredServerModsInstalled = installed;
    menu->serverDownloadsAllowed = downloadsAllowed;
    menu->contentDownloading = false;
    menu->contentDownloadFailed = false;
    menu->screen = PAUSE_MENU_MOD_COMPATIBILITY;
}

static void CopyMenuText(wchar_t* destination, uint32_t capacity,
    const wchar_t* source)
{
    uint32_t i = 0;
    while (source[i] != L'\0' && i + 1 < capacity)
    {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = L'\0';
}

static PauseMenuAction UpdateTitleScreen(PauseMenu* menu, UiContext* ui,
    int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 290.0f * s;
    float panelHeight = 292.0f * s;
    float x = ((float)width - panelWidth) * 0.5f;
    float y = ((float)height - panelHeight) * 0.5f;
    float padding = 22.0f * s;
    float buttonHeight = 40.0f * s;
    float buttonWidth = panelWidth - padding * 2.0f;
    UiPanel(ui, x, y, panelWidth, panelHeight);
    y += padding;
    UiTextCentered(ui, x + panelWidth * 0.5f, y,
        UI_COLOR_TEXT, L"laiue " LAIUE_VERSION_TEXT);
    y += ui->font.lineHeight + 22.0f * s;
    if (UiButton(ui, WIDGET_TITLE_SINGLE, x + padding, y,
            buttonWidth, buttonHeight, L"Одиночная игра"))
    {
        menu->worldListLoaded = false;
        menu->worldListOffset = 0;
        menu->screen = PAUSE_MENU_SINGLEPLAYER;
    }
    y += buttonHeight + 10.0f * s;
    if (UiButton(ui, WIDGET_TITLE_MULTI, x + padding, y,
            buttonWidth, buttonHeight, L"Сетевая игра"))
    {
        menu->serverListLoaded = false;
        menu->serverListOffset = 0;
        menu->screen = PAUSE_MENU_MULTIPLAYER;
    }
    y += buttonHeight + 10.0f * s;
    if (UiButton(ui, WIDGET_TITLE_SETTINGS, x + padding, y,
            buttonWidth, buttonHeight, L"Настройки"))
    {
        menu->settingsReturnScreen = PAUSE_MENU_TITLE;
        menu->screen = PAUSE_MENU_SETTINGS;
    }
    y += buttonHeight + 10.0f * s;
    if (UiButton(ui, WIDGET_TITLE_QUIT, x + padding, y,
            buttonWidth, buttonHeight, L"Выход"))
    {
        return PAUSE_MENU_ACTION_QUIT;
    }
    return PAUSE_MENU_ACTION_NONE;
}

static PauseMenuAction UpdateSingleplayerScreen(PauseMenu* menu,
    UiContext* ui, int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 420.0f * s;
    float panelHeight = 410.0f * s;
    float x = ((float)width - panelWidth) * 0.5f;
    float y = ((float)height - panelHeight) * 0.5f;
    float padding = 22.0f * s;
    float contentX = x + padding;
    float contentWidth = panelWidth - padding * 2.0f;
    UiPanel(ui, x, y, panelWidth, panelHeight);
    y += padding;
    UiTextCentered(ui, x + panelWidth * 0.5f, y,
        UI_COLOR_TEXT, L"Сохранённые миры");
    y += ui->font.lineHeight + 18.0f * s;

    if (!menu->worldListLoaded)
    {
        menu->worldListLoaded = SaveGameEnumerateSlots(&menu->worldSlots);
    }
    const SaveGameSlotList* slots = &menu->worldSlots;
    PauseMenuAction action = PAUSE_MENU_ACTION_NONE;
    if (menu->worldListLoaded && slots->count != 0)
    {
        if (ui->wheelSteps > 0.0f
            && menu->worldListOffset + 6U < slots->count)
        {
            ++menu->worldListOffset;
        }
        else if (ui->wheelSteps < 0.0f && menu->worldListOffset != 0)
        {
            --menu->worldListOffset;
        }
        for (uint32_t i = 0; i < 6U
            && menu->worldListOffset + i < slots->count; ++i)
        {
            uint32_t slotIndex = menu->worldListOffset + i;
            if (UiButton(ui, WIDGET_WORLD_FIRST + i, contentX, y,
                    contentWidth, 34.0f * s,
                    slots->entries[slotIndex].name))
            {
                CopyMenuText(menu->selectedWorld,
                    (uint32_t)(sizeof(menu->selectedWorld) / sizeof(wchar_t)),
                    slots->entries[slotIndex].name);
                action = PAUSE_MENU_ACTION_PLAY_WORLD;
            }
            y += 40.0f * s;
        }
    }
    else
    {
        UiText(ui, contentX, y, UI_COLOR_TEXT_DIM,
            L"Сохранённых миров пока нет");
        y += ui->font.lineHeight + 14.0f * s;
    }

    float bottom = ((float)height + panelHeight) * 0.5f - padding;
    if (action == PAUSE_MENU_ACTION_NONE
        && UiButton(ui, WIDGET_WORLD_NEW, contentX,
            bottom - 78.0f * s, contentWidth, 34.0f * s,
            L"Новый мир"))
    {
        menu->selectedWorld[0] = L'\0';
        if (SaveGameChooseNewSlot(menu->selectedWorld,
                (uint32_t)(sizeof(menu->selectedWorld) / sizeof(wchar_t))))
        {
            action = PAUSE_MENU_ACTION_PLAY_WORLD;
        }
    }
    if (action == PAUSE_MENU_ACTION_NONE
        && UiButton(ui, WIDGET_MENU_BACK, contentX,
            bottom - 34.0f * s, contentWidth, 34.0f * s, L"Назад"))
    {
        menu->screen = PAUSE_MENU_TITLE;
    }
    return action;
}

static PauseMenuAction UpdateMultiplayerScreen(PauseMenu* menu,
    UiContext* ui, int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 420.0f * s;
    float panelHeight = 360.0f * s;
    float x = ((float)width - panelWidth) * 0.5f;
    float y = ((float)height - panelHeight) * 0.5f;
    float padding = 22.0f * s;
    float contentX = x + padding;
    float contentWidth = panelWidth - padding * 2.0f;
    UiPanel(ui, x, y, panelWidth, panelHeight);
    y += padding;
    UiTextCentered(ui, x + panelWidth * 0.5f, y,
        UI_COLOR_TEXT, L"Сетевая игра");
    y += ui->font.lineHeight + 18.0f * s;
    if (!menu->serverListLoaded)
    {
        menu->serverListLoaded = ServerListLoad(&menu->servers);
    }
    const ServerList* list = &menu->servers;
    if (ui->wheelSteps > 0.0f
        && menu->serverListOffset + 5U < list->count)
    {
        ++menu->serverListOffset;
    }
    else if (ui->wheelSteps < 0.0f && menu->serverListOffset != 0)
    {
        --menu->serverListOffset;
    }
    for (uint32_t i = 0; i < 5U
        && menu->serverListOffset + i < list->count; ++i)
    {
        uint32_t serverIndex = menu->serverListOffset + i;
        wchar_t label[160];
        UiTextBuilder labelBuilder;
        UiTextBuilderInit(&labelBuilder, label,
            (uint32_t)(sizeof(label) / sizeof(label[0])));
        UiTextBuilderAppend(&labelBuilder, list->entries[serverIndex].name);
        UiTextBuilderAppend(&labelBuilder, L" — ");
        UiTextBuilderAppend(&labelBuilder, list->entries[serverIndex].address);
        UiTextBuilderAppendChar(&labelBuilder, L':');
        UiTextBuilderAppendUnsigned(&labelBuilder, list->entries[serverIndex].port);
        if (UiButton(ui, WIDGET_SERVER_FIRST + i, contentX, y,
                contentWidth, 38.0f * s,
                menu->networkConnecting
                    && menu->selectedServerPort == list->entries[serverIndex].port
                    ? L"Подключение…" : label)
            && !menu->networkConnecting)
        {
            menu->networkConnecting = true;
            menu->networkRejected = false;
            menu->contentDownloadFailed = false;
            menu->selectedServerPort = list->entries[serverIndex].port;
            return PAUSE_MENU_ACTION_CONNECT_LOCAL;
        }
        y += 44.0f * s;
    }
    if (menu->networkRejected)
    {
        UiText(ui, contentX, y, UiColor(216, 96, 80, 255),
            L"Сервер отклонил набор модов");
    }
    else if (menu->contentDownloadFailed)
    {
        UiText(ui, contentX, y, UiColor(216, 96, 80, 255),
            L"Содержимое не прошло проверку или установку");
    }
    float bottom = ((float)height + panelHeight) * 0.5f - padding;
    if (UiButton(ui, WIDGET_MENU_BACK, contentX,
            bottom - 34.0f * s, contentWidth, 34.0f * s, L"Назад"))
    {
        menu->networkConnecting = false;
        menu->screen = PAUSE_MENU_TITLE;
        return PAUSE_MENU_ACTION_CANCEL_CONNECT;
    }
    return PAUSE_MENU_ACTION_NONE;
}

static PauseMenuAction UpdateModCompatibilityScreen(PauseMenu* menu,
    UiContext* ui, int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 440.0f * s;
    float panelHeight = 240.0f * s;
    float x = ((float)width - panelWidth) * 0.5f;
    float y = ((float)height - panelHeight) * 0.5f;
    float padding = 22.0f * s;
    float contentX = x + padding;
    float contentWidth = panelWidth - padding * 2.0f;
    UiPanel(ui, x, y, panelWidth, panelHeight);
    y += padding;
    UiTextCentered(ui, x + panelWidth * 0.5f, y,
        UI_COLOR_TEXT, L"Моды сервера");
    y += ui->font.lineHeight + 16.0f * s;
    UiText(ui, contentX, y, UI_COLOR_TEXT_DIM,
        menu->requiredServerModsInstalled
            ? L"Необходимые моды установлены, но состав отличается."
            : L"Часть обязательных модов отсутствует.");
    y += ui->font.lineHeight + 14.0f * s;
    if (menu->requiredServerModsInstalled)
    {
        if (UiButton(ui, WIDGET_MODS_APPLY, contentX, y,
                contentWidth, 38.0f * s, L"Включить нужные моды и войти"))
        {
            return PAUSE_MENU_ACTION_APPLY_SERVER_MODS;
        }
    }
    else if (menu->serverDownloadsAllowed)
    {
        if (menu->contentDownloading)
        {
            UiText(ui, contentX, y, UI_COLOR_TEXT_DIM,
                L"Загрузка и проверка содержимого…");
        }
        else if (UiButton(ui, WIDGET_CONTENT_DOWNLOAD, contentX, y,
                contentWidth, 38.0f * s,
                L"Загрузить содержимое и войти"))
        {
            menu->contentDownloading = true;
            menu->contentDownloadFailed = false;
            return PAUSE_MENU_ACTION_DOWNLOAD_SERVER_CONTENT;
        }
    }
    else
    {
        UiText(ui, contentX, y, UiColor(216, 96, 80, 255),
            L"Загрузка содержимого на сервере отключена.");
    }
    float bottom = ((float)height + panelHeight) * 0.5f - padding;
    if (UiButton(ui, WIDGET_CONNECT_CANCEL, contentX,
            bottom - 34.0f * s, contentWidth, 34.0f * s, L"Отмена"))
    {
        menu->networkConnecting = false;
        menu->contentDownloading = false;
        menu->screen = PAUSE_MENU_MULTIPLAYER;
        return PAUSE_MENU_ACTION_CANCEL_CONNECT;
    }
    return PAUSE_MENU_ACTION_NONE;
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
        + buttonHeight * 4.0f + buttonGap * 3.0f
        + 12.0f * s + titleHeight + padding;

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
        menu->settingsReturnScreen = PAUSE_MENU_MAIN;
        menu->screen = PAUSE_MENU_SETTINGS;
    }
    cursorY += buttonHeight + buttonGap;

    if (UiButton(ui, WIDGET_RETURN_TITLE, buttonX, cursorY,
            buttonWidth, buttonHeight, L"В главное меню"))
    {
        action = PAUSE_MENU_ACTION_RETURN_TITLE;
    }
    cursorY += buttonHeight + buttonGap;

    if (UiButton(ui, WIDGET_QUIT, buttonX, cursorY,
            buttonWidth, buttonHeight, L"Выход"))
    {
        action = PAUSE_MENU_ACTION_QUIT;
    }
    cursorY += buttonHeight + 12.0f * s;

    UiTextCentered(ui, panelX + panelWidth * 0.5f, cursorY,
        UiColor(150, 158, 172, 160), L"laiue " LAIUE_VERSION_TEXT);

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
        + 30.0f * s + 14.0f * s;
}

static float AdminTabHeight(const UiContext* ui)
{
    float s = ui->scale;
    float line = ui->font.lineHeight;
    return (line + 6.0f * s) + (20.0f * s + 4.0f * s) + (line + 12.0f * s)
        + (line + 6.0f * s)
        + 30.0f * s * 4.0f + 4.0f * s * 3.0f + 14.0f * s
        + 36.0f * s + 10.0f * s;
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
    return y + rowHeight + 14.0f * s;
}

static float DrawAdminTab(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, float dayLengthMinutes,
    float x, float width, float y)
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

    // Мир также сохраняется автоматически при выходе из игры.
    if (UiButton(ui, WIDGET_SAVE_WORLD, x, y, width, 36.0f * s,
            L"Сохранить мир"))
    {
        menu->saveRequested = true;
    }
    return y + 36.0f * s + 10.0f * s;
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

// === Вкладки «Текстуры» и «Шейдеры» (по паку на вкладку) ===

static float PackSectionHeight(const UiContext* ui)
{
    float s = ui->scale;
    float line = ui->font.lineHeight;
    float sectionHeader = line + 12.0f * s;
    float packRows = 30.0f * s * 6.0f + 4.0f * s * 5.0f;
    float applyButton = 36.0f * s + 10.0f * s;
    float resetButton = 30.0f * s + 8.0f * s;
    return sectionHeader + packRows + 4.0f * s + applyButton + resetButton;
}

static float TexturesTabHeight(const UiContext* ui)
{
    return PackSectionHeight(ui) + ui->font.lineHeight + 6.0f * ui->scale;
}

static float ShadersTabHeight(const UiContext* ui)
{
    float s = ui->scale;
    float line = ui->font.lineHeight;
    return PackSectionHeight(ui) + line + 6.0f * s
        + 8.0f * s
        + (22.0f * s + 12.0f * s)                      // каркасный режим
        + (line + 6.0f * s) + (20.0f * s + 12.0f * s); // гамма
}

static const wchar_t* ShaderStatusText(ShaderPackLoadStatus status)
{
    switch (status)
    {
        case SHADER_PACK_LOAD_OK: return L"Шейдерпак применён";
        case SHADER_PACK_LOAD_INVALID_MANIFEST: return L"Несовместимый pack.lm";
        case SHADER_PACK_LOAD_EMPTY: return L"В паке нет шейдеров";
        case SHADER_PACK_LOAD_ACTIVATION_ERROR: return L"Не удалось выбрать пак";
        case SHADER_PACK_LOAD_PIPELINE_ERROR: return L"Ошибка создания GPU pipeline";
        case SHADER_PACK_LOAD_IO_ERROR: return L"Ошибка чтения шейдерпака";
        default: return L"";
    }
}

static float DrawShadersTab(PauseMenu* menu, UiContext* ui, GameSettings* settings,
    Renderer* renderer, float x, float width, float y)
{
    float s = ui->scale;
    float lineHeight = ui->font.lineHeight;
    wchar_t text[MENU_TEXT_CAPACITY];

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
                    ShaderPackLoadStatus loadStatus;
                    if (ShaderPackLoadActiveBytecode(
                            &shaders[0], &lengths[0],
                            &shaders[1], &lengths[1],
                            &shaders[2], &lengths[2],
                            &shaders[3], &lengths[3],
                            &shaders[4], &lengths[4],
                            &shaders[5], &lengths[5], &loadStatus))
                    {
                        bool reloaded = RendererReloadShaders(renderer,
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
                        menu->shaderPackStatus = reloaded
                            ? SHADER_PACK_LOAD_OK
                            : SHADER_PACK_LOAD_PIPELINE_ERROR;
                    }
                    else
                    {
                        menu->shaderPackStatus = loadStatus;
                        ShaderPackActivate(NULL);
                        RendererReloadShaders(renderer,
                            NULL,0, NULL,0, NULL,0, NULL,0, NULL,0, NULL,0);
                    }
                    settings->selectedShaderPack = -1;
                }
                else
                {
                    menu->shaderPackStatus = SHADER_PACK_LOAD_ACTIVATION_ERROR;
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
        menu->shaderPackStatus = SHADER_PACK_LOAD_NOT_ATTEMPTED;
    }
    y += 30.0f * s + 8.0f * s;

    const wchar_t* statusText = ShaderStatusText(menu->shaderPackStatus);
    if (statusText[0] != L'\0')
    {
        uint32_t color = menu->shaderPackStatus == SHADER_PACK_LOAD_OK
            ? UiColor(80, 200, 80, 255) : UiColor(216, 96, 80, 255);
        UiText(ui, x, y, color, statusText);
    }
    y += lineHeight + 6.0f * s;

    y += 8.0f * s;

    // ─── Параметры шейдера ───
    float toggleRowHeight = 22.0f * s;
    float toggleRowTop = y + (toggleRowHeight - UiScaled(ui, 22.0f)) * 0.5f;
    UiText(ui, x, y + (toggleRowHeight - lineHeight) * 0.5f,
        UI_COLOR_TEXT, L"Каркасный режим");
    bool wireframe = settings->wireframe;
    if (UiToggle(ui, WIDGET_WIREFRAME, x + width - UiScaled(ui, 40.0f),
            toggleRowTop, &wireframe))
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
    return y + 20.0f * s + 12.0f * s;
}

static const wchar_t* TextureStatusText(RendererContentStatus status)
{
    switch (status)
    {
        case RENDERER_CONTENT_OK: return L"Текстурпак применён";
        case RENDERER_CONTENT_INVALID: return L"Повреждённый формат LTP";
        case RENDERER_CONTENT_GPU_ERROR: return L"Ошибка загрузки текстуры на GPU";
        case RENDERER_CONTENT_ACTIVATION_ERROR: return L"Не удалось выбрать пак";
        case RENDERER_CONTENT_IO_ERROR: return L"Ошибка чтения текстурпака";
        default: return L"";
    }
}

static float DrawTexturesTab(PauseMenu* menu, UiContext* ui, GameSettings* settings,
    Renderer* renderer, float x, float width, float y)
{
    float s = ui->scale;
    float lineHeight = ui->font.lineHeight;

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
                    bool reloaded = RendererReloadTexturePack(renderer);
                    RendererContentStatus status =
                        RendererGetTexturePackLoadStatus(renderer);
                    if (!reloaded) status = RENDERER_CONTENT_GPU_ERROR;
                    if (status != RENDERER_CONTENT_OK)
                    {
                        TexturePackActivate(NULL);
                        RendererReloadTexturePack(renderer);
                    }
                    menu->texturePackStatus = status;
                    settings->selectedTexturePack = -1;
                }
                else
                {
                    menu->texturePackStatus =
                        RENDERER_CONTENT_ACTIVATION_ERROR;
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
        menu->texturePackStatus = RENDERER_CONTENT_NOT_ATTEMPTED;
    }
    y += 30.0f * s + 8.0f * s;

    const wchar_t* statusText = TextureStatusText(menu->texturePackStatus);
    if (statusText[0] != L'\0')
    {
        uint32_t color = menu->texturePackStatus == RENDERER_CONTENT_OK
            ? UiColor(80, 200, 80, 255) : UiColor(216, 96, 80, 255);
        UiText(ui, x, y, color, statusText);
    }
    y += lineHeight + 6.0f * s;

    return y;
}

// === Вкладка «Моды» ===

static float ModsTabHeight(const UiContext* ui, const ModsState* mods)
{
    float s = ui->scale;
    float line = ui->font.lineHeight;
    float rows = mods->count > 0
        ? (34.0f * s + 6.0f * s) * (float)mods->count
        : line + 6.0f * s;
    return (line + 12.0f * s) + rows + 8.0f * s + line * 2.0f + 6.0f * s;
}

static float DrawModsTab(UiContext* ui, ModsState* mods,
    float x, float width, float y)
{
    float s = ui->scale;
    float lineHeight = ui->font.lineHeight;

    UiText(ui, x, y, UI_COLOR_TEXT, L"Моды");
    UiText(ui, x + width - UiTextWidth(ui, L".lmp"), y,
        UI_COLOR_ACCENT, L".lmp");
    y += lineHeight + 12.0f * s;

    if (mods->count == 0)
    {
        UiText(ui, x, y, UI_COLOR_TEXT_DIM, L"Каталог mods пуст");
        y += lineHeight + 6.0f * s;
    }

    for (uint32_t i = 0; i < mods->count; ++i)
    {
        ModEntry* entry = &mods->entries[i];
        float rowHeight = 34.0f * s;
        float textTop = y + (rowHeight - lineHeight) * 0.5f;

        uint32_t nameColor = entry->compatible
            ? (entry->enabled ? UI_COLOR_TEXT : UiColor(196, 202, 214, 255))
            : UiColor(120, 126, 138, 255);
        UiText(ui, x, textTop, nameColor, entry->displayName);

        if (entry->version[0] != L'\0')
        {
            UiText(ui, x + UiTextWidth(ui, entry->displayName) + 8.0f * s,
                textTop, UiColor(150, 158, 172, 160), entry->version);
        }

        const wchar_t* sideText = entry->side == MOD_SIDE_CLIENT
            ? L"клиент" : (entry->side == MOD_SIDE_SERVER ? L"сервер" : L"клиент+сервер");
        UiText(ui, x + width - UiScaled(ui, 130.0f)
                - UiTextWidth(ui, sideText), textTop,
            UI_COLOR_TEXT_DIM, sideText);

        if (entry->compatible)
        {
            bool enabled = entry->enabled;
            float toggleTop = y + (rowHeight - UiScaled(ui, 22.0f)) * 0.5f;
            if (UiToggle(ui, WIDGET_MOD_TOGGLE_FIRST + i,
                    x + width - UiScaled(ui, 40.0f), toggleTop, &enabled))
            {
                ModsSetEnabled(mods, i, enabled);
            }

            // Фактический статус от хоста: включённый мод, который
            // не загрузился, честно показывает причину.
            if (entry->enabled
                && entry->runtimeStatus != MOD_RUNTIME_LOADED
                && entry->runtimeStatus != MOD_RUNTIME_SIDE_INACTIVE)
            {
                wchar_t reason[MENU_TEXT_CAPACITY];
                UiTextBuilder builder;
                UiTextBuilderInit(&builder, reason, MENU_TEXT_CAPACITY);
                if (entry->runtimeStatus == MOD_RUNTIME_INIT_FAILED)
                {
                    UiTextBuilderAppend(&builder, L"отказ Init, код ");
                    UiTextBuilderAppendUnsigned(&builder,
                        entry->initResult < 0
                            ? (uint32_t)(-entry->initResult)
                            : (uint32_t)entry->initResult);
                }
                else
                {
                    UiTextBuilderAppend(&builder, L"DLL не загрузилась");
                }
                UiText(ui,
                    x + width - UiScaled(ui, 48.0f)
                        - UiTextWidth(ui, reason),
                    textTop, UiColor(214, 130, 118, 255), reason);
            }
        }
        else
        {
            wchar_t needed[MENU_TEXT_CAPACITY];
            UiTextBuilder builder;
            UiTextBuilderInit(&builder, needed, MENU_TEXT_CAPACITY);
            if (entry->requiredGame[0] != L'\0')
            {
                UiTextBuilderAppend(&builder, L"нужна laiue ");
                UiTextBuilderAppend(&builder, entry->requiredGame);
            }
            else
            {
                UiTextBuilderAppend(&builder, L"манифест неполон");
            }
            UiText(ui, x + width - UiTextWidth(ui, needed), textTop,
                UiColor(214, 130, 118, 255), needed);
        }

        y += rowHeight + 6.0f * s;
    }

    y += 8.0f * s;
    UiText(ui, x, y, UI_COLOR_TEXT_DIM,
        L"DLL загружаются в порядке включения;");
    y += lineHeight;
    UiText(ui, x, y, UI_COLOR_TEXT_DIM,
        L"библиотеки включайте раньше их потребителей.");
    return y + lineHeight + 6.0f * s;
}

static void UpdateSettingsScreen(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer, Window* window,
    ModsState* mods, float dayLengthMinutes, int32_t width, int32_t height)
{
    float s = ui->scale;
    float panelWidth = 400.0f * s;
    float padding = 20.0f * s;
    float lineHeight = ui->font.lineHeight;
    float tabsHeight = 36.0f * s * 2.0f + 6.0f * s;  // два ряда сегментов
    float tabRowHeight = 36.0f * s;

    float buttonHeight = 36.0f * s;

    float contentHeight;
    switch (menu->settingsTab)
    {
        case SETTINGS_TAB_TEXTURES: contentHeight = TexturesTabHeight(ui); break;
        case SETTINGS_TAB_SHADERS:  contentHeight = ShadersTabHeight(ui); break;
        case SETTINGS_TAB_MODS:     contentHeight = ModsTabHeight(ui, mods); break;
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

    // Два ряда вкладок: клик в ряду выбирает вкладку своего диапазона.
    int32_t topTab = menu->settingsTab < 3 ? menu->settingsTab : -1;
    int32_t bottomTab = menu->settingsTab >= 3 ? menu->settingsTab - 3 : -1;
    if (UiSegmented(ui, WIDGET_TABS, contentX, cursorY,
            contentWidth, tabRowHeight, TAB_LABELS_TOP, 3, &topTab))
    {
        menu->settingsTab = topTab;
        menu->settingsScroll = 0.0f;
    }
    if (UiSegmented(ui, WIDGET_TABS + 4, contentX,
            cursorY + tabRowHeight + 6.0f * s,
            contentWidth, tabRowHeight, TAB_LABELS_BOTTOM, 3, &bottomTab))
    {
        menu->settingsTab = bottomTab + 3;
        menu->settingsScroll = 0.0f;
    }
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
        case SETTINGS_TAB_TEXTURES:
            DrawTexturesTab(menu, ui, settings, renderer,
                contentX, contentWidth, scrolledY);
            break;
        case SETTINGS_TAB_SHADERS:
            DrawShadersTab(menu, ui, settings, renderer,
                contentX, contentWidth, scrolledY);
            break;
        case SETTINGS_TAB_MODS:
            DrawModsTab(ui, mods, contentX, contentWidth, scrolledY);
            break;
        case SETTINGS_TAB_ADMIN:
            DrawAdminTab(menu, ui, settings, dayLengthMinutes,
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
        menu->screen = menu->settingsReturnScreen;
    }
}

PauseMenuAction PauseMenuUpdate(PauseMenu* menu, UiContext* ui,
    GameSettings* settings, Renderer* renderer, Window* window,
    ModsState* mods, float dayLengthMinutes, int32_t width, int32_t height,
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
            menu->screen = menu->settingsReturnScreen;
        }
        else if (menu->screen == PAUSE_MENU_SINGLEPLAYER
            || menu->screen == PAUSE_MENU_MULTIPLAYER)
        {
            menu->screen = PAUSE_MENU_TITLE;
        }
        else if (menu->screen == PAUSE_MENU_TITLE)
        {
            return PAUSE_MENU_ACTION_QUIT;
        }
        else if (menu->screen == PAUSE_MENU_MOD_COMPATIBILITY)
        {
            menu->networkConnecting = false;
            menu->screen = PAUSE_MENU_MULTIPLAYER;
            return PAUSE_MENU_ACTION_CANCEL_CONNECT;
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

    if (menu->screen == PAUSE_MENU_TITLE)
    {
        return UpdateTitleScreen(menu, ui, width, height);
    }
    if (menu->screen == PAUSE_MENU_SINGLEPLAYER)
    {
        return UpdateSingleplayerScreen(menu, ui, width, height);
    }
    if (menu->screen == PAUSE_MENU_MULTIPLAYER)
    {
        return UpdateMultiplayerScreen(menu, ui, width, height);
    }
    if (menu->screen == PAUSE_MENU_MOD_COMPATIBILITY)
    {
        return UpdateModCompatibilityScreen(menu, ui, width, height);
    }

    UpdateSettingsScreen(menu, ui, settings, renderer, window,
        mods, dayLengthMinutes, width, height);
    return PAUSE_MENU_ACTION_NONE;
}
