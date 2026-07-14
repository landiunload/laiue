#include "core/player_command_mapper.h"

#include "core/math.h"

void PlayerCommandMapperBuild(Input* input, const Camera* camera,
    PlayerControllerCommand* outCommand)
{
    float forward =
        (InputIsKeyDown(input, INPUT_KEY_W) ? 1.0f : 0.0f)
        - (InputIsKeyDown(input, INPUT_KEY_S) ? 1.0f : 0.0f);
    float right =
        (InputIsKeyDown(input, INPUT_KEY_D) ? 1.0f : 0.0f)
        - (InputIsKeyDown(input, INPUT_KEY_A) ? 1.0f : 0.0f);

    if (forward != 0.0f && right != 0.0f)
    {
        forward *= 0.70710678f;
        right *= 0.70710678f;
    }

    float sinYaw = ScalarSin(camera->yaw);
    float cosYaw = ScalarCos(camera->yaw);
    outCommand->movementX =
        (double)(sinYaw * forward + cosYaw * right);
    outCommand->movementY =
        (double)(cosYaw * forward - sinYaw * right);
    outCommand->jumpPressed =
        InputConsumeKeyPress(input, INPUT_KEY_SPACE);
    outCommand->crouchHeld =
        InputIsKeyDown(input, INPUT_KEY_SHIFT);
}
