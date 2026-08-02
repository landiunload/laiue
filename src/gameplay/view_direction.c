#include "gameplay/view_direction.h"

#include <stddef.h>
#include <stdint.h>

#define VIEW_PITCH_LIMIT 1.570796f
#define VIEW_HALF_PI 1.5707963267948966f
#define VIEW_PI 3.1415926535897932f

static float ViewWrap(float radians)
{
    if (radians != radians || radians > 100000000.0f ||
        radians < -100000000.0f)
    {
        return 0.0f;
    }
    // Perform the reduction in double precision. At the accepted 1e8 input
    // limit a float multiply/subtract can lose an entire revolution, leaving
    // the polynomial far outside its intended interval.
    double value = (double)radians;
    double revolutions = value * 0.15915494309189533577;
    int32_t whole = (int32_t)(revolutions +
        (revolutions >= 0.0 ? 0.5 : -0.5));
    return (float)(value - (double)whole * 6.2831853071795864769);
}

static void ViewSinCos(float radians, float* outSin, float* outCos)
{
    float x = ViewWrap(radians);
    float cosineSign = 1.0f;
    // The original Taylor polynomials were evaluated across [-pi, pi]. Near
    // pi the cosine overshot -1 (about -1.001829), which made otherwise valid
    // view rays fail the interaction direction bounds. Quadrant reduction
    // keeps both polynomials inside their accurate [-pi/2, pi/2] interval.
    if (x > VIEW_HALF_PI)
    {
        x = VIEW_PI - x;
        cosineSign = -1.0f;
    }
    else if (x < -VIEW_HALF_PI)
    {
        x = -VIEW_PI - x;
        cosineSign = -1.0f;
    }

    float x2 = x * x;
    float sineSeries = 1.60590438e-10f;
    sineSeries = sineSeries * x2 - 2.50521084e-08f;
    sineSeries = sineSeries * x2 + 2.75573192e-06f;
    sineSeries = sineSeries * x2 - 1.98412698e-04f;
    sineSeries = sineSeries * x2 + 8.33333333e-03f;
    sineSeries = sineSeries * x2 - 1.66666667e-01f;
    sineSeries = sineSeries * x2 + 1.0f;

    float cosineSeries = 2.08767570e-09f;
    cosineSeries = cosineSeries * x2 - 2.75573192e-07f;
    cosineSeries = cosineSeries * x2 + 2.48015873e-05f;
    cosineSeries = cosineSeries * x2 - 1.38888889e-03f;
    cosineSeries = cosineSeries * x2 + 4.16666667e-02f;
    cosineSeries = cosineSeries * x2 - 5.00000000e-01f;
    cosineSeries = cosineSeries * x2 + 1.0f;

    *outSin = x * sineSeries;
    *outCos = cosineSign * cosineSeries;
}

void GameplayViewForward(float yaw, float pitch, float outForward[3])
{
    if (outForward == NULL) return;
    if (pitch != pitch) pitch = 0.0f;
    if (pitch < -VIEW_PITCH_LIMIT) pitch = -VIEW_PITCH_LIMIT;
    if (pitch > VIEW_PITCH_LIMIT) pitch = VIEW_PITCH_LIMIT;
    float sinPitch;
    float cosPitch;
    float sinYaw;
    float cosYaw;
    ViewSinCos(pitch, &sinPitch, &cosPitch);
    ViewSinCos(yaw, &sinYaw, &cosYaw);
    outForward[0] = sinYaw * cosPitch;
    outForward[1] = cosYaw * cosPitch;
    outForward[2] = sinPitch;
}
