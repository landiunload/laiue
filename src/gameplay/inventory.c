#include "gameplay/inventory.h"

#include <stddef.h>
#include <string.h>

void InventoryClear(Inventory* inventory)
{
    if (inventory == NULL) return;
    memset(inventory, 0, sizeof(*inventory));
}

uint32_t InventoryAdd(Inventory* inventory, InventoryItemId item,
    uint32_t count)
{
    if (inventory == NULL || item == INVENTORY_ITEM_NONE || count == 0)
        return count;

    for (uint32_t i = 0; i < INVENTORY_SLOT_COUNT && count != 0; ++i)
    {
        InventorySlot* slot = &inventory->slots[i];
        if (slot->item != item || slot->count >= INVENTORY_STACK_LIMIT)
            continue;
        uint32_t space = INVENTORY_STACK_LIMIT - slot->count;
        uint32_t added = count < space ? count : space;
        slot->count = (uint16_t)(slot->count + added);
        count -= added;
    }
    for (uint32_t i = 0; i < INVENTORY_SLOT_COUNT && count != 0; ++i)
    {
        InventorySlot* slot = &inventory->slots[i];
        if (slot->count != 0) continue;
        uint32_t added = count < INVENTORY_STACK_LIMIT
            ? count : INVENTORY_STACK_LIMIT;
        slot->item = item;
        slot->count = (uint16_t)added;
        count -= added;
    }
    return count;
}

bool InventoryConsume(Inventory* inventory, InventoryItemId item,
    uint32_t count)
{
    if (inventory == NULL || item == INVENTORY_ITEM_NONE || count == 0)
        return false;
    uint32_t available = 0;
    for (uint32_t i = 0; i < INVENTORY_SLOT_COUNT; ++i)
    {
        if (inventory->slots[i].item == item)
            available += inventory->slots[i].count;
    }
    if (available < count) return false;

    for (uint32_t i = 0; i < INVENTORY_SLOT_COUNT && count != 0; ++i)
    {
        InventorySlot* slot = &inventory->slots[i];
        if (slot->item != item) continue;
        uint32_t removed = count < slot->count ? count : slot->count;
        slot->count = (uint16_t)(slot->count - removed);
        count -= removed;
        if (slot->count == 0) slot->item = INVENTORY_ITEM_NONE;
    }
    return true;
}

bool InventoryConsumeSelected(Inventory* inventory, uint32_t count,
    InventoryItemId* outItem)
{
    if (inventory == NULL || count == 0
        || inventory->selectedHotbarSlot >= INVENTORY_HOTBAR_SLOT_COUNT)
        return false;
    InventorySlot* slot =
        &inventory->slots[inventory->selectedHotbarSlot];
    if (slot->item == INVENTORY_ITEM_NONE || slot->count < count)
        return false;
    if (outItem != NULL) *outItem = slot->item;
    slot->count = (uint16_t)(slot->count - count);
    if (slot->count == 0) slot->item = INVENTORY_ITEM_NONE;
    return true;
}

void InventorySelectHotbar(Inventory* inventory, uint32_t slot)
{
    if (inventory != NULL && slot < INVENTORY_HOTBAR_SLOT_COUNT)
        inventory->selectedHotbarSlot = (uint8_t)slot;
}

const InventorySlot* InventorySelectedSlot(const Inventory* inventory)
{
    if (inventory == NULL
        || inventory->selectedHotbarSlot >= INVENTORY_HOTBAR_SLOT_COUNT)
        return NULL;
    return &inventory->slots[inventory->selectedHotbarSlot];
}
