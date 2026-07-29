#include "core/player_command_mapper.h"

#include "core/math.h"

void PlayerCommandMapperBuildWorldDirection(
    float forward, float right, float yaw, double outDirection[2])
{
    float inputLengthSquared =
        forward * forward + right * right;
    if (inputLengthSquared <= 0.0f)
    {
        outDirection[0] = 0.0;
        outDirection[1] = 0.0;
        return;
    }

    float sinYaw = ScalarSin(yaw);
    float cosYaw = ScalarCos(yaw);
    float worldX = sinYaw * forward + cosYaw * right;
    float worldY = cosYaw * forward - sinYaw * right;
    float worldLengthSquared = worldX * worldX + worldY * worldY;
    if (worldLengthSquared <= 0.0f)
    {
        outDirection[0] = 0.0;
        outDirection[1] = 0.0;
        return;
    }

    float scale = ScalarSqrt(inputLengthSquared)
        / ScalarSqrt(worldLengthSquared);
    outDirection[0] = (double)(worldX * scale);
    outDirection[1] = (double)(worldY * scale);
}

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

    if (forward == 0.0f && right == 0.0f)
    {
        outCommand->movementX = 0.0;
        outCommand->movementY = 0.0;
    }
    else
    {
        double direction[2];
        PlayerCommandMapperBuildWorldDirection(
            forward, right, camera->yaw, direction);
        outCommand->movementX = direction[0];
        outCommand->movementY = direction[1];
    }
    outCommand->jumpPressed =
        InputConsumeKeyPress(input, INPUT_KEY_SPACE);
    outCommand->jumpHeld =
        InputIsKeyDown(input, INPUT_KEY_SPACE);
    outCommand->sprintHeld =
        InputIsKeyDown(input, INPUT_KEY_CONTROL);
    outCommand->crouchHeld =
        InputIsKeyDown(input, INPUT_KEY_SHIFT);
}
