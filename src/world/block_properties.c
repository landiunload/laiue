#include "world/block_properties.h"

BlockProperties BlockGetProperties(BlockType block)
{
    if (block == BLOCK_AIR)
    {
        return (BlockProperties) {
            .friction = 0.0f,
            .solid = false,
        };
    }

    // Пока земля, трава и неизвестные ненулевые типы сохраняют прежнее
    // поведение: это твёрдые блоки с обычным трением. Новые материалы
    // (например, лёд) добавляются сюда, не меняя physics/gameplay.
    return (BlockProperties) {
        .friction = 1.0f,
        .solid = true,
    };
}
