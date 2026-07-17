#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

#define INVENTORY_SLOT_COUNT 36U
#define INVENTORY_HOTBAR_SLOT_COUNT 9U
#define INVENTORY_STACK_LIMIT 64U

typedef uint16_t InventoryItemId;

#define INVENTORY_ITEM_NONE ((InventoryItemId)0)

typedef struct InventorySlot
{
    InventoryItemId item;
    uint16_t count;
} InventorySlot;

// Компактная value-модель: без указателей и выделений памяти, поэтому её
// можно безопасно хранить на клиенте/сервере и сериализовать снимком.
typedef struct Inventory
{
    InventorySlot slots[INVENTORY_SLOT_COUNT];
    uint8_t selectedHotbarSlot;
} Inventory;

LAIUE_GAMEPLAY_API void InventoryClear(Inventory* inventory);
LAIUE_GAMEPLAY_API uint32_t InventoryAdd(Inventory* inventory,
    InventoryItemId item, uint32_t count);
LAIUE_GAMEPLAY_API bool InventoryConsume(Inventory* inventory,
    InventoryItemId item, uint32_t count);
LAIUE_GAMEPLAY_API bool InventoryConsumeSelected(Inventory* inventory,
    uint32_t count, InventoryItemId* outItem);
LAIUE_GAMEPLAY_API void InventorySelectHotbar(Inventory* inventory,
    uint32_t slot);
LAIUE_GAMEPLAY_API const InventorySlot* InventorySelectedSlot(
    const Inventory* inventory);
