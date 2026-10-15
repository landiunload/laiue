#include "scene/math.h"
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

static bool Near(float value, float expected)
{
    const float difference = value - expected;
    return difference < 1e-4f && difference > -1e-4f;
}

/* Row vector times matrix, then the perspective divide. */
static void Transform(const float matrix[16], const float point[3], float out[4])
{
    for (int column = 0; column < 4; ++column)
        out[column] = point[0] * matrix[column] + point[1] * matrix[4 + column] +
                      point[2] * matrix[8 + column] + matrix[12 + column];
}

static bool IsIdentity(const float matrix[16])
{
    for (int index = 0; index < 16; ++index)
        if (matrix[index] != (index % 5 == 0 ? 1.0f : 0.0f))
            return false;
    return true;
}

static void TestPerspective(void)
{
    float projection[16];
    Expect(Matrix4Perspective(1.5707963f, 2.0f, 0.5f, 100.0f, projection), "perspective builds");
    float clip[4];
    const float nearPoint[3] = {0.0f, 0.0f, 0.5f};
    Transform(projection, nearPoint, clip);
    Expect(Near(clip[2] / clip[3], 0.0f), "the near plane maps to depth 0");
    const float farPoint[3] = {0.0f, 0.0f, 100.0f};
    Transform(projection, farPoint, clip);
    Expect(Near(clip[2] / clip[3], 1.0f), "the far plane maps to depth 1");
    /* A 90 degree vertical view at aspect 2: the top edge at distance 10 is
     * y = 10, the right edge x = 20. */
    const float corner[3] = {20.0f, 10.0f, 10.0f};
    Transform(projection, corner, clip);
    Expect(Near(clip[0] / clip[3], 1.0f) && Near(clip[1] / clip[3], 1.0f),
           "the field of view and aspect ratio frame the corner");

    Expect(!Matrix4Perspective(0.0f, 1.0f, 0.1f, 10.0f, projection) && IsIdentity(projection),
           "a zero field of view is rejected");
    Expect(!Matrix4Perspective(1.0f, 1.0f, 10.0f, 10.0f, projection) &&
               !Matrix4Perspective(1.0f, 0.0f, 0.1f, 10.0f, projection),
           "degenerate planes and aspect are rejected");
}

static void TestLookAt(void)
{
    const float up[3] = {0.0f, 0.0f, 1.0f};
    const float eye[3] = {3.0f, -10.0f, 5.0f};
    const float target[3] = {3.0f, 0.0f, 0.0f};
    float view[16];
    Expect(Matrix4LookAt(eye, target, up, view), "look-at builds");
    float local[4];
    Transform(view, target, local);
    const float distance = 11.180340f;
    Expect(Near(local[0], 0.0f) && Near(local[1], 0.0f) && Near(local[2], distance),
           "the target lies straight ahead");
    const float right[3] = {4.0f, 0.0f, 0.0f};
    Transform(view, right, local);
    Expect(Near(local[0], 1.0f) && Near(local[1], 0.0f), "world +X is to the right");
    const float above[3] = {3.0f, 0.0f, 1.0f};
    Transform(view, above, local);
    Expect(local[1] > 0.0f, "world up is up on screen");

    const float straightDown[3] = {3.0f, 0.0f, -5.0f};
    Expect(!Matrix4LookAt(target, straightDown, up, view) && IsIdentity(view),
           "looking along the up vector is rejected");
    Expect(!Matrix4LookAt(eye, eye, up, view), "a zero-length view is rejected");
}

LAIUE_TEST_ENTRY(SceneMathTestEntryPoint)
{
    TestPerspective();
    TestLookAt();
    LAIUE_TEST_SUCCESS();
}
