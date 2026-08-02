#include "gameplay/inventory.h"
#include "test_runtime.h"

#include <stdint.h>

static void InventoryExpect(bool condition, const char* message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static void TestItemValidation(void)
{
    InventoryExpect(!InventoryItemIsValid(INVENTORY_ITEM_NONE),
        "empty inventory item was accepted as valid");
    InventoryExpect(InventoryItemIsValid((InventoryItemId)1),
        "earth inventory item was rejected");
    InventoryExpect(InventoryItemIsValid((InventoryItemId)2),
        "grass inventory item was rejected");
    InventoryExpect(InventoryItemIsValid(INVENTORY_ITEM_PHYSICS_LEVER),
        "physics lever inventory item was rejected");
    InventoryExpect(!InventoryItemIsValid((InventoryItemId)4),
        "unknown inventory item was accepted");
    InventoryExpect(!InventoryItemIsValid(UINT16_MAX),
        "out-of-range inventory item was accepted");
}

static void TestLeverStackLifecycle(void)
{
    Inventory inventory;
    InventoryClear(&inventory);

    InventoryExpect(InventoryAdd(&inventory,
            INVENTORY_ITEM_PHYSICS_LEVER,
            INVENTORY_STACK_LIMIT + 1U) == 0U,
        "physics lever stack was not added");
    InventoryExpect(
        inventory.slots[0].item == INVENTORY_ITEM_PHYSICS_LEVER
            && inventory.slots[0].count == INVENTORY_STACK_LIMIT
            && inventory.slots[1].item == INVENTORY_ITEM_PHYSICS_LEVER
            && inventory.slots[1].count == 1U,
        "physics lever stack did not split at the stack limit");

    InventoryItemId consumed = INVENTORY_ITEM_NONE;
    InventoryExpect(InventoryConsumeSelected(
            &inventory, 1U, &consumed)
            && consumed == INVENTORY_ITEM_PHYSICS_LEVER
            && inventory.slots[0].count == INVENTORY_STACK_LIMIT - 1U,
        "selected physics lever was not consumed");
    InventoryExpect(InventoryConsume(&inventory,
            INVENTORY_ITEM_PHYSICS_LEVER, INVENTORY_STACK_LIMIT),
        "remaining physics levers were not consumed");
    InventoryExpect(
        inventory.slots[0].item == INVENTORY_ITEM_NONE
            && inventory.slots[0].count == 0U
            && inventory.slots[1].item == INVENTORY_ITEM_NONE
            && inventory.slots[1].count == 0U,
        "empty physics lever stacks retained an item id");
}

static void TestInvalidItemsDoNotMutateInventory(void)
{
    Inventory inventory;
    InventoryClear(&inventory);

    const InventoryItemId invalid = (InventoryItemId)4;
    InventoryExpect(InventoryAdd(&inventory, invalid, 7U) == 7U,
        "invalid inventory item did not return the unadded count");
    InventoryExpect(inventory.slots[0].item == INVENTORY_ITEM_NONE
            && inventory.slots[0].count == 0U,
        "invalid inventory item mutated an empty inventory");
    InventoryExpect(!InventoryConsume(&inventory, invalid, 1U),
        "invalid inventory item was consumed");

    inventory.slots[0].item = invalid;
    inventory.slots[0].count = 1U;
    InventoryItemId consumed = INVENTORY_ITEM_PHYSICS_LEVER;
    InventoryExpect(!InventoryConsumeSelected(&inventory, 1U, &consumed)
            && consumed == INVENTORY_ITEM_PHYSICS_LEVER
            && inventory.slots[0].item == invalid
            && inventory.slots[0].count == 1U,
        "selected invalid inventory item changed state");
}

LAIUE_TEST_ENTRY(InventoryTestEntryPoint)
{
    TestItemValidation();
    TestLeverStackLifecycle();
    TestInvalidItemsDoNotMutateInventory();
    LaiueTestRuntimeWrite("Inventory items: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
