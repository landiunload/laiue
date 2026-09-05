#pragma once

#include "api.h"
#include "numeric/infinite_coord.h"
#include "physics/voxel_body.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Твёрдое тело-коробка с вращением.
 *
 * VoxelBody рядом — контроллер персонажа: он не вращается, движется по одной
 * оси за раз и намеренно ничего не знает о массе. Здесь наоборот: полноценная
 * динамика с ориентацией, тензором инерции и импульсами в точках контакта.
 *
 * Скорость и угловая скорость хранятся числами произвольной точности в
 * фиксированной точке, а не в double. Причина не в точности, а в отсутствии
 * потолка: тело, которое разгоняют без конца, обязано разгоняться без конца.
 * Позиция по той же причине тоже произвольной точности — иначе первая же
 * невероятная скорость превратила бы её в бесконечность за один шаг.
 *
 * Локальные величины (ориентация, импульсы, геометрия контакта) остаются
 * double: они ограничены по своей природе. Мост между двумя мирами —
 * насыщающее преобразование, поэтому огромная скорость не отравляет решатель,
 * а лишь выводит тело из режима столкновений (см. VOXEL_RIGID_BALLISTIC_BLOCKS).
 */

// Масштаб фиксированной точки: единица скорости — блок в секунду.
#define VOXEL_RIGID_VELOCITY_SHIFT 32u
// Шаг симуляции — 1/128 секунды. Степень двойки не ради скорости, а ради
// точности: умножение на шаг становится сдвигом и не теряет ни бита.
#define VOXEL_RIGID_STEP_SHIFT 7u

// Верхняя граница здесь не про вкус, а про арифметику: размер буфера
// шага возвращается в uint32, и при большем числе тел он бы переполнился.
// Ничего меньше этого модуль не запрещает.
#define VOXEL_RIGID_MAX_BODIES 2097152u
// Сколько точек контакта отводится одному телу: восемь углов против мира
// плюс запас на соседей. Дальше манифест всё равно вырожден, а память
// растёт линейно по числу тел.
#define VOXEL_RIGID_CONTACTS_PER_BODY 16u

// За один шаг тело сдвигается настолько, что никакое столкновение уже не
// имеет смысла: за такое расстояние оно пролетает мир целиком. Столкновения
// для него отключаются, остаётся чистая баллистика. Это не ограничение
// скорости, а признание того, что на таких скоростях контактов не бывает.
#define VOXEL_RIGID_BALLISTIC_BLOCKS 1048576.0

typedef struct VoxelRigidBodyDescription
{
    // Полурёбра коробки. Все три положительны.
    double halfExtent[3];
    // Начальная позиция центра масс в локальных координатах.
    double position[3];
    double mass;
    // Упругость [0, 1] и трение [0, 1].
    double restitution;
    double friction;
} VoxelRigidBodyDescription;

typedef struct VoxelRigidBody
{
    // Позиция центра масс: блоки, умноженные на 2^VOXEL_RIGID_VELOCITY_SHIFT.
    InfiniteCoord position[3];
    // Скорость: блоков в секунду в том же масштабе.
    InfiniteCoord linearVelocity[3];
    // Угловая скорость: радиан в секунду в том же масштабе.
    InfiniteCoord angularVelocity[3];
    // Единичный кватернион (x, y, z, w).
    double orientation[4];
    double halfExtent[3];
    double inverseMass;
    // Диагональ обратного тензора инерции в системе тела.
    double inverseInertia[3];
    double restitution;
    double friction;
    // Ненулевой уникальный идентификатор: контакты обязаны быть
    // воспроизводимыми, а порядок обхода массива — нет.
    uint64_t stableId;
    bool active;
} VoxelRigidBody;

typedef struct VoxelRigidStepSettings
{
    // Ускорение свободного падения, блоков в секунду за секунду.
    double gravity[3];
    // Итераций решателя контактов за шаг. Меньше трёх куча не держит.
    uint32_t solverIterations;
    // Доля проникновения, устраняемая за шаг [0, 1]. Единица толкает резко
    // и раскачивает стопку, ноль оставляет тела утопленными.
    double penetrationCorrection;
    // Проникновение, которое считается допустимым: без него тела дрожат,
    // бесконечно выталкивая друг друга из численного шума.
    double penetrationSlop;
} VoxelRigidStepSettings;

LAIUE_PHYSICS_API void VoxelRigidStepSettingsDefault(VoxelRigidStepSettings *outSettings);

// Готовит тело. Возвращает false при неверном описании или нехватке памяти;
// в этом случае тело остаётся пригодным для Release.
LAIUE_PHYSICS_API bool VoxelRigidBodyInitialize(VoxelRigidBody *body, uint64_t stableId,
                                                const VoxelRigidBodyDescription *description);
LAIUE_PHYSICS_API void VoxelRigidBodyRelease(VoxelRigidBody *body);

// Позиция центра масс в локальных координатах. false означает, что тело
// улетело за пределы double: рисовать и сталкивать его уже нельзя.
LAIUE_PHYSICS_API bool VoxelRigidBodyLocalPosition(const VoxelRigidBody *body,
                                                   double outPosition[3]);
// Матрица поворота 3x3 по столбцам, пригодная для инстанса рендера.
LAIUE_PHYSICS_API void VoxelRigidBodyOrientationMatrix(const VoxelRigidBody *body,
                                                       float outMatrix[9]);

LAIUE_PHYSICS_API bool VoxelRigidBodyAddLinearVelocity(VoxelRigidBody *body,
                                                       const double delta[3]);
LAIUE_PHYSICS_API bool VoxelRigidBodyAddAngularVelocity(VoxelRigidBody *body,
                                                        const double delta[3]);
// Составляющие скоростей с насыщением до конечного double. В отличие от
// PointVelocity это именно скорость центра масс и она не зависит от позиции.
LAIUE_PHYSICS_API bool VoxelRigidBodyLinearVelocity(const VoxelRigidBody *body,
                                                    double outVelocity[3]);
LAIUE_PHYSICS_API bool VoxelRigidBodyAngularVelocity(const VoxelRigidBody *body,
                                                     double outVelocity[3]);
// Модуль скорости с насыщением: для интерфейса и решений вызывающего.
LAIUE_PHYSICS_API double VoxelRigidBodyLinearSpeed(const VoxelRigidBody *body);
LAIUE_PHYSICS_API double VoxelRigidBodyAngularSpeed(const VoxelRigidBody *body);

// Сдвигает тело вместе с началом локальных координат (rebasing).
LAIUE_PHYSICS_API bool VoxelRigidBodyTranslateBlocks(VoxelRigidBody *body,
                                                     const int64_t blockShift[3]);

// Скорость поверхности тела в точке: то, что подхватывает стоящий на нём.
LAIUE_PHYSICS_API bool VoxelRigidBodyPointVelocity(const VoxelRigidBody *body,
                                                   const double point[3], double outVelocity[3]);

// Сколько памяти нужно шагу. Physics ничего не выделяет сам: буфер даёт
// вызывающий, как и всюду в этом модуле.
LAIUE_PHYSICS_API uint32_t VoxelRigidBodyStepScratchBytes(uint32_t bodyCount);

// Один шаг симуляции для всего набора тел. collision обязан отдавать
// свойства блоков; queryDynamicColliders не используется — тела берутся из
// массива. Возвращает false при неверных аргументах, малом буфере или
// нехватке памяти внутри чисел произвольной точности. Каждая операция
// арифметики остаётся транзакционной и не оставляет повреждённых лимбов;
// при отказе после начала шага уже обработанные тела могут быть продвинуты,
// поэтому вызывающий, которому нужен all-or-nothing шаг, хранит свой снимок.
LAIUE_PHYSICS_API bool VoxelRigidBodyStep(VoxelRigidBody *bodies, uint32_t bodyCount,
                                          const VoxelCollisionSource *collision,
                                          const VoxelRigidStepSettings *settings, void *scratch,
                                          uint32_t scratchBytes);
