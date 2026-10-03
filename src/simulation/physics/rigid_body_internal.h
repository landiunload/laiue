#ifndef LAIUE_PHYSICS_RIGID_BODY_INTERNAL_H
#define LAIUE_PHYSICS_RIGID_BODY_INTERNAL_H

#include "physics/rigid_body.h"

#include <stdbool.h>

// Скорость точки тела по уже известному плечу lever = point - centre. Это
// та же арифметика, что у VoxelRigidBodyPointVelocity после перевода
// центра; решатель суставов вызывает её много раз при неизменной позиции
// и не переводит центр из произвольной точности заново на каждой оси.
// Внутренний контракт модуля: в SDK не устанавливается и не экспортируется.
void VoxelRigidBodyPointVelocityAtLever(const VoxelRigidBody *body, const double lever[3],
                                        double outVelocity[3]);

#endif
