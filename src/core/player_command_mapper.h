#pragma once

#include "game/camera.h"
#include "gameplay/player_controller.h"
#include "input/input.h"

void PlayerCommandMapperBuild(Input* input, const Camera* camera,
    PlayerControllerCommand* outCommand);
