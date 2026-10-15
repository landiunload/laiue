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

static void TestInvert(void)
{
    const float up[3] = {0.0f, 0.0f, 1.0f};
    const float eye[3] = {3.0f, -10.0f, 5.0f};
    const float target[3] = {3.0f, 0.0f, 0.0f};
    float view[16];
    float projection[16];
    float viewProjection[16];
    float inverse[16];
    float product[16];
    Expect(Matrix4LookAt(eye, target, up, view) &&
               Matrix4Perspective(1.0f, 1.5f, 0.1f, 1000.0f, projection),
           "a camera builds");
    Matrix4Multiply(view, projection, viewProjection);
    Expect(Matrix4Invert(viewProjection, inverse), "a camera inverts");
    Matrix4Multiply(viewProjection, inverse, product);
    for (int index = 0; index < 16; ++index)
        Expect(Near(product[index], index % 5 == 0 ? 1.0f : 0.0f),
               "a matrix times its inverse is the identity");

    const float singular[16] = {1.0f, 2.0f, 3.0f, 4.0f, 2.0f, 4.0f, 6.0f, 8.0f,
                                0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    Expect(!Matrix4Invert(singular, inverse) && IsIdentity(inverse),
           "a singular matrix is rejected");
}

static void TestViewRay(void)
{
    const float up[3] = {0.0f, 0.0f, 1.0f};
    const float eye[3] = {0.0f, -10.0f, 0.0f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    float view[16];
    float projection[16];
    float viewProjection[16];
    Expect(Matrix4LookAt(eye, target, up, view) &&
               Matrix4Perspective(1.5707963f, 2.0f, 0.5f, 80000.0f, projection),
           "a camera builds");
    Matrix4Multiply(view, projection, viewProjection);

    float origin[3];
    float direction[3];
    Expect(ViewRayThroughScreenPoint(viewProjection, 0.0f, 0.0f, origin, direction),
           "a ray through the centre");
    Expect(Near(origin[0], 0.0f) && Near(origin[1], -9.5f) && Near(origin[2], 0.0f),
           "the ray starts on the near plane");
    Expect(Near(direction[0], 0.0f) && Near(direction[1], 1.0f) && Near(direction[2], 0.0f),
           "the centre ray looks at the target");

    /* A 90 degree vertical view at aspect 2: the top right corner lies at 45
     * degrees up and atan(2) to the right. */
    Expect(ViewRayThroughScreenPoint(viewProjection, 1.0f, 1.0f, origin, direction),
           "a ray through a corner");
    Expect(Near(direction[0], 2.0f / 2.4494897f) && Near(direction[1], 1.0f / 2.4494897f) &&
               Near(direction[2], 1.0f / 2.4494897f),
           "the corner ray follows the field of view and aspect ratio");

    const float zero[16] = {0.0f};
    Expect(!ViewRayThroughScreenPoint(zero, 0.0f, 0.0f, origin, direction),
           "a degenerate camera gives no ray");
}

LAIUE_TEST_ENTRY(SceneMathTestEntryPoint)
{
    TestPerspective();
    TestLookAt();
    TestInvert();
    TestViewRay();
    LAIUE_TEST_SUCCESS();
}
