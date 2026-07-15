#include "core/debug_overlay.h"

static void AppendText(wchar_t* destination, uint32_t capacity,
    uint32_t* length, const wchar_t* source)
{
    while (*source != L'\0' && *length + 1u < capacity)
    {
        destination[(*length)++] = *source++;
    }
    if (capacity > 0)
    {
        destination[*length] = L'\0';
    }
}

static void AppendUnsignedDecimal(wchar_t* destination, uint32_t capacity,
    uint32_t* length, uint32_t value)
{
    wchar_t reversedDigits[10];
    uint32_t digitCount = 0;
    do
    {
        reversedDigits[digitCount++] = (wchar_t)(L'0' + value % 10u);
        value /= 10u;
    }
    while (value != 0);

    while (digitCount > 0 && *length + 1u < capacity)
    {
        destination[(*length)++] = reversedDigits[--digitCount];
    }
    if (capacity > 0)
    {
        destination[*length] = L'\0';
    }
}

static void AppendTwoDigits(wchar_t* destination, uint32_t capacity,
    uint32_t* length, uint32_t value)
{
    if (*length + 2u < capacity)
    {
        destination[(*length)++] = (wchar_t)(L'0' + (value / 10u) % 10u);
        destination[(*length)++] = (wchar_t)(L'0' + value % 10u);
    }
    if (capacity > 0)
    {
        destination[*length] = L'\0';
    }
}

void DebugOverlayBuildText(World* world, const PlayerController* player,
    GameMode gameMode, uint32_t framesPerSecond, uint32_t timeMinutes,
    const int64_t cameraBlockPosition[3],
    wchar_t* destination, uint32_t capacity)
{
    wchar_t coordinate[3][32];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        WorldFormatAbsoluteBlockCoordinate(world, axis,
            cameraBlockPosition[axis], coordinate[axis], 32);
    }

    destination[0] = L'\0';
    uint32_t length = 0;
    AppendText(destination, capacity, &length, L"X: ");
    AppendText(destination, capacity, &length, coordinate[0]);
    AppendText(destination, capacity, &length, L"\r\nY: ");
    AppendText(destination, capacity, &length, coordinate[1]);
    AppendText(destination, capacity, &length, L"\r\nZ: ");
    AppendText(destination, capacity, &length, coordinate[2]);
    AppendText(destination, capacity, &length, L"\r\nFPS: ");
    AppendUnsignedDecimal(destination, capacity, &length, framesPerSecond);
    AppendText(destination, capacity, &length, L"\r\nTime: ");
    AppendTwoDigits(destination, capacity, &length, timeMinutes / 60u);
    AppendText(destination, capacity, &length, L":");
    AppendTwoDigits(destination, capacity, &length, timeMinutes % 60u);
    AppendText(destination, capacity, &length, L"\r\nMode: ");

    if (gameMode == GAME_MODE_FLY)
    {
        AppendText(destination, capacity, &length, L"FLY");
    }
    else if (PlayerControllerIsCrouching(player))
    {
        AppendText(destination, capacity, &length, L"CROUCH");
    }
    else
    {
        AppendText(destination, capacity, &length, L"WALK");
    }
}
