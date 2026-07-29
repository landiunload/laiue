#pragma once

#include "mod/mod_host.h"

// Внутренняя точка применения policy. Вынесена из загрузчика библиотек,
// чтобы fail-closed контракт можно было проверять отдельным unit-тестом.
bool ModHostApplyBlockMutation(const ModHostBindings *bindings, int64_t x, int64_t y, int64_t z,
                               uint8_t block);
