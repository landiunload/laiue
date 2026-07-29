#pragma once

#include "game/camera.h"
#include "gameplay/player_controller.h"
#include "input/input.h"

// Поворачивает локальный input в плоскость мира и сохраняет его исходную
// длину. Это обязательно для сетевого пути: полиномиальные sin/cos имеют
// малую совместную погрешность и без нормализации могут дать length > 1,
// который production protocol корректно отклонит.
void PlayerCommandMapperBuildWorldDirection(
    float forward, float right, float yaw, double outDirection[2]);

void PlayerCommandMapperBuild(Input* input, const Camera* camera,
    PlayerControllerCommand* outCommand);
