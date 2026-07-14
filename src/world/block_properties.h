#pragma once

#include <stdbool.h>

#include "world/world.h"

// Игровые свойства типа блока не зависят от его текстуры и способа
// размещения в мире. Коэффициент трения 1.0 соответствует обычной опоре.
typedef struct BlockProperties
{
    float friction;
    bool solid;
} BlockProperties;

LAIUE_WORLD_API BlockProperties BlockGetProperties(BlockType block);
