#include "render/frame_turn.h"
#include "test_runtime.h"

#include <stdbool.h>

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

/* The way the renderer takes a window pixel: to the window's clip space, along the
 * axes, then through a viewport of negative height over the target. */
static size_t PixelThroughClipSpace(uint32_t turn, uint32_t width, uint32_t height, uint32_t x,
                                    uint32_t y)
{
    const float *axes = FRAME_TURN_CLIP_AXES[turn];
    const float windowX = ((float)x + 0.5f) * 2.0f / (float)width - 1.0f;
    const float windowY = 1.0f - ((float)y + 0.5f) * 2.0f / (float)height;
    const float clipX = windowX * axes[0] + windowY * axes[2];
    const float clipY = windowX * axes[1] + windowY * axes[3];
    const uint32_t targetWidth = (turn & 1u) != 0u ? height : width;
    const uint32_t targetHeight = (turn & 1u) != 0u ? width : height;
    const uint32_t targetX = (uint32_t)((clipX + 1.0f) * 0.5f * (float)targetWidth);
    const uint32_t targetY = (uint32_t)((1.0f - clipY) * 0.5f * (float)targetHeight);
    return (size_t)targetY * targetWidth + targetX;
}

/* A capture reads pixels where the frame was drawn, so the two must agree. */
static void TestPixelsFollowClipSpace(void)
{
    enum
    {
        WIDTH = 5u,
        HEIGHT = 3u
    };
    for (uint32_t turn = 0u; turn < 4u; ++turn)
    {
        bool taken[WIDTH * HEIGHT] = {false};
        for (uint32_t y = 0u; y < HEIGHT; ++y)
        {
            for (uint32_t x = 0u; x < WIDTH; ++x)
            {
                const size_t pixel = FrameTurnPixel(turn, WIDTH, HEIGHT, x, y);
                Expect(pixel == PixelThroughClipSpace(turn, WIDTH, HEIGHT, x, y),
                       "a capture reads the pixel the frame drew");
                Expect(pixel < WIDTH * HEIGHT && !taken[pixel],
                       "a pixel maps to a pixel of its own");
                taken[pixel] = true;
            }
        }
    }
}

/* Vulkan's ROTATE_90 is clockwise: the top left corner of the window goes to the
 * top right corner of the target, and the top row runs down its right column. */
static void TestTurnsAreClockwise(void)
{
    enum
    {
        WIDTH = 4u,
        HEIGHT = 2u
    };
    const uint32_t right = HEIGHT - 1u;
    Expect(FrameTurnPixel(1u, WIDTH, HEIGHT, 0u, 0u) == right,
           "a quarter turn keeps the top right");
    Expect(FrameTurnPixel(1u, WIDTH, HEIGHT, 1u, 0u) == right + HEIGHT, "the top row runs down");
    Expect(FrameTurnPixel(2u, WIDTH, HEIGHT, 0u, 0u) == WIDTH * HEIGHT - 1u,
           "a half turn ends in the far corner");
    Expect(FrameTurnPixel(3u, WIDTH, HEIGHT, 0u, 0u) == (WIDTH - 1u) * HEIGHT,
           "three quarters end in the bottom left corner");
}

/* A row vector times the turned matrix is the old clip position along the axes. */
static void TestMatrixFollowsAxes(void)
{
    const float matrix[16] = {
        2.0f,  3.0f,  5.0f,  7.0f,  11.0f, 13.0f, 17.0f, 19.0f,
        23.0f, 29.0f, 31.0f, 37.0f, 41.0f, 43.0f, 47.0f, 53.0f,
    };
    const float point[4] = {0.5f, -2.0f, 3.0f, 1.0f};
    float plain[4] = {0.0f};
    for (uint32_t row = 0u; row < 4u; ++row)
        for (uint32_t column = 0u; column < 4u; ++column)
            plain[column] += point[row] * matrix[row * 4u + column];

    for (uint32_t turn = 0u; turn < 4u; ++turn)
    {
        float turned[16];
        for (uint32_t index = 0u; index < 16u; ++index)
            turned[index] = matrix[index];
        FrameTurnMatrix(turned, turn);
        float clip[4] = {0.0f};
        for (uint32_t row = 0u; row < 4u; ++row)
            for (uint32_t column = 0u; column < 4u; ++column)
                clip[column] += point[row] * turned[row * 4u + column];
        const float *axes = FRAME_TURN_CLIP_AXES[turn];
        Expect(clip[0] == plain[0] * axes[0] + plain[1] * axes[2] &&
                   clip[1] == plain[0] * axes[1] + plain[1] * axes[3],
               "the matrix turns its clip position along the axes");
        Expect(clip[2] == plain[2] && clip[3] == plain[3], "depth and w stay");
    }
}

LAIUE_TEST_ENTRY(FrameTurnTestEntryPoint)
{
    TestPixelsFollowClipSpace();
    TestTurnsAreClockwise();
    TestMatrixFollowsAxes();
    LAIUE_TEST_SUCCESS();
}
