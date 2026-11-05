#pragma once

#include <stddef.h>
#include <stdint.h>

// Android показывает образы цепочки в естественной ориентации экрана. Кадр,
// нарисованный так, как лежит окно, система поворачивает при каждом показе, часто
// ещё одним проходом видеокарты по всему экрану. Поэтому кадр рисуется сразу
// повёрнутым: цель кадра лежит на turn четвертей оборота по часовой стрелке от
// окна, turn — от 0 до 3.

// Куда в клип-пространстве цели ведут оси окна: x и y оси X, затем x и y оси Y.
// Ось Y в обоих пространствах смотрит вверх.
static const float FRAME_TURN_CLIP_AXES[4][4] = {
    {1.0f, 0.0f, 0.0f, 1.0f},
    {0.0f, -1.0f, 1.0f, 0.0f},
    {-1.0f, 0.0f, 0.0f, -1.0f},
    {0.0f, 1.0f, -1.0f, 0.0f},
};

// Поворачивает то, что даёт матрица вида и проекции. Шейдеры умножают на неё вектор
// слева, поэтому x и y отсечения — её столбцы 0 и 1.
static inline void FrameTurnMatrix(float matrix[16], uint32_t turn)
{
    const float *axes = FRAME_TURN_CLIP_AXES[turn];
    for (float *row = matrix; row != matrix + 16; row += 4)
    {
        const float x = row[0];
        const float y = row[1];
        row[0] = x * axes[0] + y * axes[2];
        row[1] = x * axes[1] + y * axes[3];
    }
}

// Номер пикселя цели кадра, в котором лежит пиксель (x, y) окна width на height.
static inline size_t FrameTurnPixel(uint32_t turn, uint32_t width, uint32_t height, uint32_t x,
                                    uint32_t y)
{
    switch (turn)
    {
    case 1u:
        return (size_t)x * height + (height - 1u - y);
    case 2u:
        return (size_t)(height - 1u - y) * width + (width - 1u - x);
    case 3u:
        return (size_t)(width - 1u - x) * height + y;
    default:
        return (size_t)y * width + x;
    }
}
