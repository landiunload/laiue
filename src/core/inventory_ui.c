#include "core/inventory_ui.h"

#include "core/ui_format.h"

#define INVENTORY_WIDGET_FIRST 1200U

static const wchar_t* ItemShortName(InventoryItemId item)
{
    if (item == 1) return L"Земля";
    if (item == 2) return L"Трава";
    return L"";
}

static void FormatSlot(const InventorySlot* slot, bool creative,
    wchar_t* text, uint32_t capacity)
{
    if (slot == NULL || slot->item == INVENTORY_ITEM_NONE
        || slot->count == 0)
    {
        text[0] = L'\0';
        return;
    }
    UiTextBuilder builder;
    UiTextBuilderInit(&builder, text, capacity);
    UiTextBuilderAppend(&builder, ItemShortName(slot->item));
    UiTextBuilderAppend(&builder, creative ? L"  ∞" : L"  ");
    if (!creative) UiTextBuilderAppendUnsigned(&builder, slot->count);
}

static void DrawHotbar(UiContext* ui, Inventory* inventory,
    bool creative, int32_t viewportWidth, int32_t viewportHeight)
{
    float slotWidth = UiScaled(ui, 58.0f);
    float slotHeight = UiScaled(ui, 42.0f);
    float gap = UiScaled(ui, 4.0f);
    float totalWidth = slotWidth * INVENTORY_HOTBAR_SLOT_COUNT
        + gap * (INVENTORY_HOTBAR_SLOT_COUNT - 1U);
    float x = ((float)viewportWidth - totalWidth) * 0.5f;
    float y = (float)viewportHeight - slotHeight - UiScaled(ui, 14.0f);
    wchar_t label[24];
    for (uint32_t i = 0; i < INVENTORY_HOTBAR_SLOT_COUNT; ++i)
    {
        FormatSlot(&inventory->slots[i], creative, label, 24);
        uint32_t color = i == inventory->selectedHotbarSlot
            ? UiColor(82, 112, 170, 238) : UiColor(22, 26, 34, 220);
        UiRect(ui, x, y, slotWidth, slotHeight, UiScaled(ui, 5.0f), color);
        UiTextCentered(ui, x + slotWidth * 0.5f,
            y + (slotHeight - ui->font.lineHeight) * 0.5f,
            UI_COLOR_TEXT, label);
        x += slotWidth + gap;
    }
}

static void DrawCrosshairAndProgress(UiContext* ui, GameMode gameMode,
    float progress, int32_t viewportWidth, int32_t viewportHeight)
{
    float centerX = (float)viewportWidth * 0.5f;
    float centerY = (float)viewportHeight * 0.5f;
    float thickness = UiScaled(ui, 2.0f);
    float arm = UiScaled(ui, 7.0f);
    UiRect(ui, centerX - arm, centerY - thickness * 0.5f,
        arm * 2.0f, thickness, 0.0f, UiColor(238, 242, 250, 210));
    UiRect(ui, centerX - thickness * 0.5f, centerY - arm,
        thickness, arm * 2.0f, 0.0f, UiColor(238, 242, 250, 210));

    if (gameMode != GAME_MODE_SURVIVAL || progress <= 0.0f) return;
    if (progress > 1.0f) progress = 1.0f;
    float width = UiScaled(ui, 96.0f);
    float height = UiScaled(ui, 5.0f);
    float y = centerY + UiScaled(ui, 18.0f);
    UiRect(ui, centerX - width * 0.5f, y, width, height,
        height * 0.5f, UiColor(12, 14, 20, 210));
    UiRect(ui, centerX - width * 0.5f, y, width * progress, height,
        height * 0.5f, UiColor(118, 158, 255, 245));
}

static void DrawInventoryPanel(UiContext* ui, Inventory* inventory,
    bool creative, bool allowRearrange,
    int32_t viewportWidth, int32_t viewportHeight)
{
    float slotWidth = UiScaled(ui, 64.0f);
    float slotHeight = UiScaled(ui, 44.0f);
    float gap = UiScaled(ui, 5.0f);
    float padding = UiScaled(ui, 20.0f);
    float titleGap = UiScaled(ui, 14.0f);
    float gridWidth = slotWidth * INVENTORY_HOTBAR_SLOT_COUNT
        + gap * (INVENTORY_HOTBAR_SLOT_COUNT - 1U);
    float gridHeight = slotHeight * 4.0f + gap * 3.0f;
    float panelWidth = gridWidth + padding * 2.0f;
    float panelHeight = padding + ui->font.lineHeight + titleGap
        + gridHeight + padding;
    float panelX = ((float)viewportWidth - panelWidth) * 0.5f;
    float panelY = ((float)viewportHeight - panelHeight) * 0.5f;
    UiPanel(ui, panelX, panelY, panelWidth, panelHeight);
    UiTextCentered(ui, panelX + panelWidth * 0.5f, panelY + padding,
        UI_COLOR_TEXT, creative ? L"Инвентарь · креатив"
                               : L"Инвентарь · выживание");

    float startX = panelX + padding;
    float startY = panelY + padding + ui->font.lineHeight + titleGap;
    wchar_t label[24];
    for (uint32_t i = 0; i < INVENTORY_SLOT_COUNT; ++i)
    {
        uint32_t row = i / INVENTORY_HOTBAR_SLOT_COUNT;
        uint32_t column = i % INVENTORY_HOTBAR_SLOT_COUNT;
        float x = startX + (slotWidth + gap) * column;
        float y = startY + (slotHeight + gap) * row;
        FormatSlot(&inventory->slots[i], creative, label, 24);
        if (UiButton(ui, INVENTORY_WIDGET_FIRST + i, x, y,
                slotWidth, slotHeight, label))
        {
            if (i < INVENTORY_HOTBAR_SLOT_COUNT)
            {
                InventorySelectHotbar(inventory, i);
            }
            else if (allowRearrange)
            {
                uint32_t hotbar = inventory->selectedHotbarSlot;
                InventorySlot temporary = inventory->slots[hotbar];
                inventory->slots[hotbar] = inventory->slots[i];
                inventory->slots[i] = temporary;
            }
        }
    }
}

void InventoryUiDraw(UiContext* ui, Inventory* inventory, GameMode gameMode,
    bool inventoryOpen, bool allowRearrange, float breakingProgress,
    int32_t viewportWidth, int32_t viewportHeight)
{
    if (ui == NULL || inventory == NULL) return;
    bool creative = gameMode == GAME_MODE_CREATIVE;
    if (!inventoryOpen)
    {
        DrawCrosshairAndProgress(ui, gameMode, breakingProgress,
            viewportWidth, viewportHeight);
    }
    DrawHotbar(ui, inventory, creative, viewportWidth, viewportHeight);
    if (inventoryOpen)
    {
        UiRect(ui, 0.0f, 0.0f, (float)viewportWidth,
            (float)viewportHeight, 0.0f, UiColor(8, 10, 16, 150));
        DrawInventoryPanel(ui, inventory, creative, allowRearrange,
            viewportWidth, viewportHeight);
    }
}
