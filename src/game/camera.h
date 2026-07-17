#pragma once

typedef struct Camera
{
    double position[3];    // [0]=X, [1]=Y (вторая горизонталь), [2]=Z (высота)
    float yaw;
    float pitch;
} Camera;
