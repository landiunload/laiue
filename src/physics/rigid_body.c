#include "physics/rigid_body.h"

#include "math/scalar.h"
#include "physics/fp_environment.h"

#include <float.h>
#include <string.h>

// Скорость сближения, ниже которой отскок не применяется. Без порога тело
// на опоре вечно подпрыгивает на численном шуме.
#define RIGID_RESTITUTION_THRESHOLD 1.0
#define RIGID_STEP_SECONDS (1.0 / 128.0)
#define RIGID_CONTACTS_PER_BODY VOXEL_RIGID_CONTACTS_PER_BODY

// Пустая ячейка и конец цепочки широкого отбора.
#define RIGID_HASH_EMPTY 0xFFFFFFFFu

// === Скаляры ===

static double AbsoluteDouble(double value)
{
    return value < 0.0 ? -value : value;
}

static bool IsFiniteDouble(double value)
{
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return ((bits >> 52) & 0x7ffu) != 0x7ffu;
}

// Квадратный корень двойной точности поверх аппаратного float-корня: две
// итерации Ньютона доводят его до полной точности double. libm в движке нет,
// а math_support считает во float.
static double SquareRoot(double value)
{
    if (!(value > 0.0))
    {
        return 0.0;
    }
    double estimate = (double)ScalarSqrt((float)value);
    if (!(estimate > 0.0))
    {
        return 0.0;
    }
    estimate = 0.5 * (estimate + value / estimate);
    estimate = 0.5 * (estimate + value / estimate);
    return estimate;
}

static double Dot3(const double left[3], const double right[3])
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

static void Cross3(const double left[3], const double right[3], double out[3])
{
    double x = left[1] * right[2] - left[2] * right[1];
    double y = left[2] * right[0] - left[0] * right[2];
    double z = left[0] * right[1] - left[1] * right[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

static bool TryFloorToInt64(double value, int64_t *outValue)
{
    // (double)INT64_MAX rounds to 2^63, therefore the upper bound is
    // intentionally exclusive.  Keeping the cast behind this check avoids
    // undefined behaviour for finite but enormous local coordinates.
    if (!IsFiniteDouble(value) || value < -9223372036854775808.0 ||
        value >= 9223372036854775808.0)
    {
        return false;
    }
    int64_t truncated = (int64_t)value;
    *outValue = (double)truncated > value ? truncated - 1 : truncated;
    return true;
}

// === Кватернионы ===

static void QuaternionNormalize(double quaternion[4])
{
    double lengthSquared = quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
                           quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3];
    if (!(lengthSquared > 0.0) || !IsFiniteDouble(lengthSquared))
    {
        quaternion[0] = 0.0;
        quaternion[1] = 0.0;
        quaternion[2] = 0.0;
        quaternion[3] = 1.0;
        return;
    }
    double inverse = 1.0 / SquareRoot(lengthSquared);
    for (int32_t index = 0; index < 4; ++index)
    {
        quaternion[index] *= inverse;
    }
}

static void QuaternionMultiply(const double left[4], const double right[4], double out[4])
{
    double x = left[3] * right[0] + left[0] * right[3] + left[1] * right[2] - left[2] * right[1];
    double y = left[3] * right[1] - left[0] * right[2] + left[1] * right[3] + left[2] * right[0];
    double z = left[3] * right[2] + left[0] * right[1] - left[1] * right[0] + left[2] * right[3];
    double w = left[3] * right[3] - left[0] * right[0] - left[1] * right[1] - left[2] * right[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

// Столбцы матрицы поворота: column[k] — образ орта k в мире.
static void QuaternionToColumns(const double quaternion[4], double columns[3][3])
{
    double x = quaternion[0];
    double y = quaternion[1];
    double z = quaternion[2];
    double w = quaternion[3];

    columns[0][0] = 1.0 - 2.0 * (y * y + z * z);
    columns[0][1] = 2.0 * (x * y + z * w);
    columns[0][2] = 2.0 * (x * z - y * w);

    columns[1][0] = 2.0 * (x * y - z * w);
    columns[1][1] = 1.0 - 2.0 * (x * x + z * z);
    columns[1][2] = 2.0 * (y * z + x * w);

    columns[2][0] = 2.0 * (x * z + y * w);
    columns[2][1] = 2.0 * (y * z - x * w);
    columns[2][2] = 1.0 - 2.0 * (x * x + y * y);
}

// === Мост между произвольной точностью и локальными double ===

static double FixedToDouble(const InfiniteCoord *value)
{
    double raw = InfiniteCoordToDoubleSaturating(value);
    // Деление на 2^32 точное: это лишь сдвиг экспоненты.
    return raw / 4294967296.0;
}

static bool TryAddDoubleToFixed(InfiniteCoord *outValue, const InfiniteCoord *value,
                                double delta)
{
    if (outValue == NULL || value == NULL || !IsFiniteDouble(delta))
    {
        return false;
    }
    if (delta == 0.0)
    {
        return InfiniteCoordTryCopyAddInt64(outValue, value, 0);
    }

    // Через целое и сдвиг, а не умножением на 2^32: так добавка любой
    // величины ложится точно, а не упирается в диапазон int64.
    InfiniteCoord whole;
    InfiniteCoordInit(&whole);
    if (!InfiniteCoordTrySetFromDouble(&whole, delta))
    {
        return false;
    }
    InfiniteCoord scaled;
    InfiniteCoordInit(&scaled);
    bool ok = InfiniteCoordTryCopyShiftLeft(&scaled, &whole, VOXEL_RIGID_VELOCITY_SHIFT);
    InfiniteCoordDestroy(&whole);
    if (!ok)
    {
        return false;
    }

    // Дробная часть добавки укладывается в int64 после масштабирования и
    // добавляется отдельно: без неё тело не разгонялось бы вовсе на малых
    // ускорениях — шаг гравитации меньше единицы.
    double fraction = 0.0;
    // Every binary64 value with magnitude >= 2^53 is already integral.  The
    // guarded cast handles only the range where it is both defined by C17 and
    // needed to recover a fractional part.
    if (delta > -9007199254740992.0 && delta < 9007199254740992.0)
    {
        double integerPart = (double)(int64_t)delta;
        fraction = delta - integerPart;
    }
    int64_t fractionFixed = (int64_t)(fraction * 4294967296.0);

    InfiniteCoord sum;
    InfiniteCoordInit(&sum);
    ok = InfiniteCoordTryAdd(&sum, value, &scaled);
    InfiniteCoordDestroy(&scaled);
    if (!ok)
    {
        return false;
    }

    InfiniteCoord total;
    InfiniteCoordInit(&total);
    ok = InfiniteCoordTryCopyAddInt64(&total, &sum, fractionFixed);
    InfiniteCoordDestroy(&sum);
    if (!ok)
    {
        return false;
    }

    *outValue = total;
    return true;
}

static bool AddDoubleToFixed(InfiniteCoord *value, double delta)
{
    if (!IsFiniteDouble(delta) || delta == 0.0)
    {
        return IsFiniteDouble(delta);
    }
    InfiniteCoord updated;
    InfiniteCoordInit(&updated);
    if (!TryAddDoubleToFixed(&updated, value, delta))
    {
        return false;
    }
    InfiniteCoordDestroy(value);
    *value = updated;
    return true;
}

// === Тело ===

void VoxelRigidStepSettingsDefault(VoxelRigidStepSettings *outSettings)
{
    if (outSettings == NULL)
    {
        return;
    }
    outSettings->gravity[0] = 0.0;
    outSettings->gravity[1] = 0.0;
    outSettings->gravity[2] = -24.0;
    outSettings->solverIterations = 8u;
    outSettings->penetrationCorrection = 0.35;
    outSettings->penetrationSlop = 0.005;
}

void VoxelRigidBodyRelease(VoxelRigidBody *body)
{
    if (body == NULL)
    {
        return;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&body->position[axis]);
        InfiniteCoordDestroy(&body->linearVelocity[axis]);
        InfiniteCoordDestroy(&body->angularVelocity[axis]);
    }
    body->active = false;
}

bool VoxelRigidBodyInitialize(VoxelRigidBody *body, uint64_t stableId,
                              const VoxelRigidBodyDescription *description)
{
    if (body == NULL)
    {
        return false;
    }

    // Тело приводится в пригодное состояние до любой проверки: заголовок
    // обещает, что после неудачи его можно освободить, а необнулённая
    // память со стека вызывающего этого не позволяет.
    memset(body, 0, sizeof(*body));
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&body->position[axis]);
        InfiniteCoordInit(&body->linearVelocity[axis]);
        InfiniteCoordInit(&body->angularVelocity[axis]);
    }
    body->orientation[3] = 1.0;

    if (description == NULL || stableId == 0u)
    {
        return false;
    }
    if (!(description->mass > 0.0) || !IsFiniteDouble(description->mass))
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!(description->halfExtent[axis] > 0.0) ||
            !IsFiniteDouble(description->halfExtent[axis]) ||
            !IsFiniteDouble(description->position[axis]))
        {
            return false;
        }
        body->halfExtent[axis] = description->halfExtent[axis];
    }
    if (!(description->restitution >= 0.0 && description->restitution <= 1.0) ||
        !(description->friction >= 0.0 && description->friction <= 1.0))
    {
        return false;
    }

    body->inverseMass = 1.0 / description->mass;
    // Прямоугольный параллелепипед: I_k = m (a_j^2 + a_l^2) / 3 для полурёбер a.
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double first = body->halfExtent[(axis + 1) % 3];
        double second = body->halfExtent[(axis + 2) % 3];
        double inertia = description->mass * (first * first + second * second) / 3.0;
        if (!(inertia > 0.0) || !IsFiniteDouble(inertia))
        {
            return false;
        }
        body->inverseInertia[axis] = 1.0 / inertia;
    }
    body->restitution = description->restitution;
    body->friction = description->friction;
    body->stableId = stableId;

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!AddDoubleToFixed(&body->position[axis], description->position[axis]))
        {
            return false;
        }
    }
    body->active = true;
    return true;
}

bool VoxelRigidBodyLocalPosition(const VoxelRigidBody *body, double outPosition[3])
{
    if (body == NULL || outPosition == NULL)
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double value = FixedToDouble(&body->position[axis]);
        if (AbsoluteDouble(value) >= DBL_MAX / 4294967296.0)
        {
            return false;
        }
        outPosition[axis] = value;
    }
    return true;
}

void VoxelRigidBodyOrientationMatrix(const VoxelRigidBody *body, float outMatrix[9])
{
    if (body == NULL || outMatrix == NULL)
    {
        return;
    }
    double columns[3][3];
    QuaternionToColumns(body->orientation, columns);
    for (int32_t column = 0; column < 3; ++column)
    {
        for (int32_t row = 0; row < 3; ++row)
        {
            outMatrix[column * 3 + row] = (float)columns[column][row];
        }
    }
}

bool VoxelRigidBodyAddLinearVelocity(VoxelRigidBody *body, const double delta[3])
{
    if (body == NULL || delta == NULL)
    {
        return false;
    }

    InfiniteCoord updated[3];
    bool changed[3] = {false, false, false};
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&updated[axis]);
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!IsFiniteDouble(delta[axis]))
        {
            for (int32_t cleanup = 0; cleanup < 3; ++cleanup)
            {
                InfiniteCoordDestroy(&updated[cleanup]);
            }
            return false;
        }
        if (delta[axis] == 0.0)
        {
            continue;
        }
        changed[axis] = true;
        if (!TryAddDoubleToFixed(&updated[axis], &body->linearVelocity[axis], delta[axis]))
        {
            for (int32_t cleanup = 0; cleanup < 3; ++cleanup)
            {
                InfiniteCoordDestroy(&updated[cleanup]);
            }
            return false;
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (changed[axis])
        {
            InfiniteCoordDestroy(&body->linearVelocity[axis]);
            body->linearVelocity[axis] = updated[axis];
        }
    }
    return true;
}

bool VoxelRigidBodyAddAngularVelocity(VoxelRigidBody *body, const double delta[3])
{
    if (body == NULL || delta == NULL)
    {
        return false;
    }

    InfiniteCoord updated[3];
    bool changed[3] = {false, false, false};
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&updated[axis]);
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!IsFiniteDouble(delta[axis]))
        {
            for (int32_t cleanup = 0; cleanup < 3; ++cleanup)
            {
                InfiniteCoordDestroy(&updated[cleanup]);
            }
            return false;
        }
        if (delta[axis] == 0.0)
        {
            continue;
        }
        changed[axis] = true;
        if (!TryAddDoubleToFixed(&updated[axis], &body->angularVelocity[axis], delta[axis]))
        {
            for (int32_t cleanup = 0; cleanup < 3; ++cleanup)
            {
                InfiniteCoordDestroy(&updated[cleanup]);
            }
            return false;
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (changed[axis])
        {
            InfiniteCoordDestroy(&body->angularVelocity[axis]);
            body->angularVelocity[axis] = updated[axis];
        }
    }
    return true;
}

bool VoxelRigidBodyLinearVelocity(const VoxelRigidBody *body, double outVelocity[3])
{
    if (body == NULL || outVelocity == NULL)
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outVelocity[axis] = FixedToDouble(&body->linearVelocity[axis]);
    }
    return true;
}

bool VoxelRigidBodyAngularVelocity(const VoxelRigidBody *body, double outVelocity[3])
{
    if (body == NULL || outVelocity == NULL)
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outVelocity[axis] = FixedToDouble(&body->angularVelocity[axis]);
    }
    return true;
}

static double SaturatingLength(const InfiniteCoord value[3])
{
    double components[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        components[axis] = FixedToDouble(&value[axis]);
    }
    // Крупнейшая составляющая выносится за корень: иначе квадрат переполнил
    // бы double задолго до того, как сама скорость перестала быть числом.
    double largest = 0.0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double magnitude = AbsoluteDouble(components[axis]);
        if (magnitude > largest)
        {
            largest = magnitude;
        }
    }
    if (!(largest > 0.0))
    {
        return 0.0;
    }
    double total = 0.0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double ratio = components[axis] / largest;
        total += ratio * ratio;
    }
    return largest * SquareRoot(total);
}

double VoxelRigidBodyLinearSpeed(const VoxelRigidBody *body)
{
    return body == NULL ? 0.0 : SaturatingLength(body->linearVelocity);
}

double VoxelRigidBodyAngularSpeed(const VoxelRigidBody *body)
{
    return body == NULL ? 0.0 : SaturatingLength(body->angularVelocity);
}

bool VoxelRigidBodyTranslateBlocks(VoxelRigidBody *body, const int64_t blockShift[3])
{
    if (body == NULL || blockShift == NULL)
    {
        return false;
    }
    InfiniteCoord moved[3];
    bool changed[3] = {false, false, false};
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordInit(&moved[axis]);
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (blockShift[axis] == 0)
        {
            continue;
        }
        changed[axis] = true;
        InfiniteCoord shift;
        InfiniteCoordInit(&shift);
        InfiniteCoord zero;
        InfiniteCoordInit(&zero);
        bool ok = InfiniteCoordTryCopyAddInt64(&shift, &zero, blockShift[axis]);
        InfiniteCoordDestroy(&zero);
        if (!ok)
        {
            InfiniteCoordDestroy(&shift);
            goto failure;
        }
        InfiniteCoord negated;
        InfiniteCoordInit(&negated);
        ok = InfiniteCoordTryCopyNegate(&negated, &shift);
        InfiniteCoordDestroy(&shift);
        if (!ok)
        {
            InfiniteCoordDestroy(&negated);
            goto failure;
        }
        InfiniteCoord scaled;
        InfiniteCoordInit(&scaled);
        ok = InfiniteCoordTryCopyShiftLeft(&scaled, &negated, VOXEL_RIGID_VELOCITY_SHIFT);
        InfiniteCoordDestroy(&negated);
        if (!ok)
        {
            InfiniteCoordDestroy(&scaled);
            goto failure;
        }
        ok = InfiniteCoordTryAdd(&moved[axis], &body->position[axis], &scaled);
        InfiniteCoordDestroy(&scaled);
        if (!ok)
        {
            goto failure;
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (changed[axis])
        {
            InfiniteCoordDestroy(&body->position[axis]);
            body->position[axis] = moved[axis];
        }
    }
    return true;

failure:
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&moved[axis]);
    }
    return false;
}

bool VoxelRigidBodyPointVelocity(const VoxelRigidBody *body, const double point[3],
                                 double outVelocity[3])
{
    if (body == NULL || point == NULL || outVelocity == NULL)
    {
        return false;
    }
    double centre[3];
    if (!VoxelRigidBodyLocalPosition(body, centre))
    {
        return false;
    }

    double lever[3];
    double angular[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        lever[axis] = point[axis] - centre[axis];
        angular[axis] = FixedToDouble(&body->angularVelocity[axis]);
    }
    double rotational[3];
    Cross3(angular, lever, rotational);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outVelocity[axis] = FixedToDouble(&body->linearVelocity[axis]) + rotational[axis];
    }
    return true;
}

// === Шаг ===

typedef struct RigidBodyCache
{
    double position[3];
    double linear[3];
    double angular[3];
    // Столбцы матрицы поворота и мировая диагональ обратной инерции.
    double columns[3][3];
    double corners[8][3];
    bool collidable;
} RigidBodyCache;

typedef struct RigidContact
{
    uint32_t bodyIndex;
    // UINT32_MAX означает неподвижный мир.
    uint32_t otherIndex;
    double point[3];
    double normal[3];
    double depth;
    double normalImpulse;
    double restitution;
    double friction;
} RigidContact;

typedef struct RigidStepScratch
{
    RigidBodyCache *caches;
    RigidContact *contacts;
    // Индексы активных тел, отсортированные по stableId. Все обходы,
    // влияющие на порядок импульсов, идут только через этот массив.
    uint32_t *order;
    // Широкий отбор: цепочки тел по ячейкам равномерной сетки.
    uint32_t *next;
    uint32_t *buckets;
    uint32_t bucketCount;
    uint32_t activeCount;
    double cellSize;
    uint32_t contactCount;
    uint32_t contactCapacity;
} RigidStepScratch;

static void SiftStableOrderDown(const VoxelRigidBody *bodies, uint32_t *order,
                                uint32_t root, uint32_t count)
{
    for (;;)
    {
        uint32_t child = root * 2u + 1u;
        if (child >= count)
        {
            return;
        }
        if (child + 1u < count &&
            bodies[order[child]].stableId < bodies[order[child + 1u]].stableId)
        {
            ++child;
        }
        if (bodies[order[root]].stableId >= bodies[order[child]].stableId)
        {
            return;
        }
        uint32_t temporary = order[root];
        order[root] = order[child];
        order[child] = temporary;
        root = child;
    }
}

static bool BuildStableOrder(const VoxelRigidBody *bodies, uint32_t bodyCount,
                             RigidStepScratch *scratch)
{
    scratch->activeCount = 0u;
    for (uint32_t index = 0; index < bodyCount; ++index)
    {
        if (!bodies[index].active)
        {
            continue;
        }
        if (bodies[index].stableId == 0u)
        {
            return false;
        }
        scratch->order[scratch->activeCount++] = index;
    }

    uint32_t count = scratch->activeCount;
    for (uint32_t root = count / 2u; root > 0u; --root)
    {
        SiftStableOrderDown(bodies, scratch->order, root - 1u, count);
    }
    for (uint32_t remaining = count; remaining > 1u; --remaining)
    {
        uint32_t temporary = scratch->order[0];
        scratch->order[0] = scratch->order[remaining - 1u];
        scratch->order[remaining - 1u] = temporary;
        SiftStableOrderDown(bodies, scratch->order, 0u, remaining - 1u);
    }

    for (uint32_t index = 1u; index < count; ++index)
    {
        if (bodies[scratch->order[index - 1u]].stableId ==
            bodies[scratch->order[index]].stableId)
        {
            return false;
        }
    }
    return true;
}

// Число корзин — степень двойки не меньше удвоенного числа тел: остаток
// от деления заменяется маской, а цепочки остаются короткими.
static uint32_t BucketCountFor(uint32_t bodyCount)
{
    uint32_t buckets = 64u;
    while (buckets < bodyCount * 2u && buckets < (1u << 22))
    {
        buckets <<= 1;
    }
    return buckets;
}

uint32_t VoxelRigidBodyStepScratchBytes(uint32_t bodyCount)
{
    if (bodyCount == 0u || bodyCount > VOXEL_RIGID_MAX_BODIES)
    {
        return 0u;
    }
    uint64_t caches = (uint64_t)bodyCount * sizeof(RigidBodyCache);
    uint64_t contacts = (uint64_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidContact);
    uint64_t links = (uint64_t)bodyCount * sizeof(uint32_t) * 2u;
    uint64_t buckets = (uint64_t)BucketCountFor(bodyCount) * sizeof(uint32_t);
    uint64_t total = caches + contacts + links + buckets + 64u;
    return total > 0xFFFFFFFFull ? 0u : (uint32_t)total;
}

static bool BlockIsSolid(const VoxelCollisionSource *collision, int64_t x, int64_t y, int64_t z)
{
    VoxelBlockPhysics block;
    block.flags = 0u;
    block.friction = 0.0f;
    collision->queryBlockPhysics(collision->context, x, y, z, &block);
    return (block.flags & (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID) != 0u;
}

static void BuildCache(const VoxelRigidBody *body, RigidBodyCache *cache)
{
    cache->collidable = VoxelRigidBodyLocalPosition(body, cache->position);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        cache->linear[axis] = FixedToDouble(&body->linearVelocity[axis]);
        cache->angular[axis] = FixedToDouble(&body->angularVelocity[axis]);
        if (!IsFiniteDouble(cache->linear[axis]) || !IsFiniteDouble(cache->angular[axis]))
        {
            cache->collidable = false;
        }
    }
    QuaternionToColumns(body->orientation, cache->columns);

    // Тело, пролетающее за шаг больше, чем может значить столкновение,
    // считается баллистическим: контакты для него не строятся вовсе.
    double travel = 0.0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        travel += AbsoluteDouble(cache->linear[axis]) * RIGID_STEP_SECONDS;
    }
    if (travel > VOXEL_RIGID_BALLISTIC_BLOCKS)
    {
        cache->collidable = false;
    }
    if (!cache->collidable)
    {
        memset(cache->corners, 0, sizeof(cache->corners));
        return;
    }

    for (uint32_t corner = 0; corner < 8u; ++corner)
    {
        double sign[3] = {
            (corner & 1u) != 0u ? 1.0 : -1.0,
            (corner & 2u) != 0u ? 1.0 : -1.0,
            (corner & 4u) != 0u ? 1.0 : -1.0,
        };
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            cache->corners[corner][axis] =
                cache->position[axis] + cache->columns[0][axis] * sign[0] * body->halfExtent[0] +
                cache->columns[1][axis] * sign[1] * body->halfExtent[1] +
                cache->columns[2][axis] * sign[2] * body->halfExtent[2];
            int64_t ignored = 0;
            if (!TryFloorToInt64(cache->corners[corner][axis], &ignored))
            {
                cache->collidable = false;
            }
        }
    }
    if (!cache->collidable)
    {
        memset(cache->corners, 0, sizeof(cache->corners));
    }
}

static bool AppendContact(RigidStepScratch *scratch, const RigidContact *contact)
{
    if (scratch->contactCount >= scratch->contactCapacity)
    {
        // Манифест переполнен: лишние точки отбрасываются, а не искажают
        // решение. Тело останется чуть менее устойчивым, но не улетит.
        return false;
    }
    scratch->contacts[scratch->contactCount++] = *contact;
    return true;
}

// Ищет грань, через которую угол вышел бы из блока: минимальное погружение
// среди тех направлений, где соседний блок пуст. Без проверки соседа угол,
// попавший глубоко в пол, выталкивался бы вбок внутрь соседнего блока.
static bool ResolveBlockContact(const VoxelCollisionSource *collision, const double point[3],
                                double outNormal[3], double *outDepth)
{
    int64_t block[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!TryFloorToInt64(point[axis], &block[axis]))
        {
            return false;
        }
    }
    if (!BlockIsSolid(collision, block[0], block[1], block[2]))
    {
        return false;
    }

    double best = 0.0;
    int32_t bestAxis = -1;
    double bestSign = 0.0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double local = point[axis] - (double)block[axis];
        for (int32_t direction = 0; direction < 2; ++direction)
        {
            double sign = direction == 0 ? -1.0 : 1.0;
            double depth = direction == 0 ? local : 1.0 - local;
            if (depth < 0.0)
            {
                depth = 0.0;
            }
            int64_t neighbour[3] = {block[0], block[1], block[2]};
            neighbour[axis] += direction == 0 ? -1 : 1;
            if (BlockIsSolid(collision, neighbour[0], neighbour[1], neighbour[2]))
            {
                continue;
            }
            if (bestAxis < 0 || depth < best)
            {
                best = depth;
                bestAxis = axis;
                bestSign = sign;
            }
        }
    }
    if (bestAxis < 0)
    {
        return false;
    }

    outNormal[0] = 0.0;
    outNormal[1] = 0.0;
    outNormal[2] = 0.0;
    outNormal[bestAxis] = bestSign;
    *outDepth = best;
    return true;
}

static void CollectWorldContacts(const VoxelRigidBody *bodies, uint32_t bodyCount,
                                 const VoxelCollisionSource *collision, RigidStepScratch *scratch)
{
    (void)bodyCount;
    for (uint32_t ordered = 0; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        const RigidBodyCache *cache = &scratch->caches[index];
        if (!bodies[index].active || !cache->collidable)
        {
            continue;
        }
        for (uint32_t corner = 0; corner < 8u; ++corner)
        {
            RigidContact contact;
            memset(&contact, 0, sizeof(contact));
            if (!ResolveBlockContact(collision, cache->corners[corner], contact.normal,
                                     &contact.depth))
            {
                continue;
            }
            contact.bodyIndex = index;
            contact.otherIndex = UINT32_MAX;
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                contact.point[axis] = cache->corners[corner][axis];
            }
            contact.restitution = bodies[index].restitution;
            contact.friction = bodies[index].friction;
            if (!AppendContact(scratch, &contact))
            {
                return;
            }
        }
    }
}

// === Манифест двух коробок ===
//
// Точечная проверка «угол одного тела внутри другого» здесь не годится:
// два одинаковых куба, стоящих ровно друг над другом, касаются углами точно
// по границе, и ни один угол не оказывается строго внутри — стопка
// проваливается сама сквозь себя. Поэтому берётся SAT по 15 осям (три
// собственные оси каждой коробки и девять cross-осей рёбер), затем
// отсечение падающей грани опорной. Cross-оси используются для точного
// отсечения edge-edge раздельных пар; контактный manifold остаётся граневым,
// что даёт устойчивую точку опоры для решателя.

#define RIGID_MANIFOLD_POINTS 4u

typedef struct BoxManifold
{
    // Направление из второго тела в первое.
    double normal[3];
    double point[RIGID_MANIFOLD_POINTS][3];
    double depth[RIGID_MANIFOLD_POINTS];
    uint32_t count;
} BoxManifold;

// Полупротяжённость коробки вдоль произвольной оси.
// Полурёбра принимаются указателем, а не массивом фиксированной длины:
// сюда приходит и выбранная тернарным оператором ссылка, размер которой
// gcc доказать не может и предупреждает о чтении за границей.
static double BoxRadius(const RigidBodyCache *cache, const double *halfExtent,
                        const double axis[3])
{
    double radius = 0.0;
    for (int32_t index = 0; index < 3; ++index)
    {
        radius += halfExtent[index] * AbsoluteDouble(Dot3(cache->columns[index], axis));
    }
    return radius;
}

// Грань коробки, наиболее противоположная нормали: та, что действительно
// упирается в опору.
static void IncidentFace(const RigidBodyCache *cache, const double *halfExtent,
                         const double normal[3], double outVertices[4][3])
{
    int32_t bestAxis = 0;
    double bestAlignment = -DBL_MAX;
    double bestSign = 1.0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double alignment = Dot3(cache->columns[axis], normal);
        if (-alignment > bestAlignment)
        {
            bestAlignment = -alignment;
            bestAxis = axis;
            bestSign = 1.0;
        }
        if (alignment > bestAlignment)
        {
            bestAlignment = alignment;
            bestAxis = axis;
            bestSign = -1.0;
        }
    }

    int32_t first = (bestAxis + 1) % 3;
    int32_t second = (bestAxis + 2) % 3;
    static const double corner[4][2] = {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}};
    for (uint32_t index = 0; index < 4u; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            outVertices[index][axis] =
                cache->position[axis] +
                cache->columns[bestAxis][axis] * bestSign * halfExtent[bestAxis] +
                cache->columns[first][axis] * corner[index][0] * halfExtent[first] +
                cache->columns[second][axis] * corner[index][1] * halfExtent[second];
        }
    }
}

// Отсечение многоугольника плоскостью dot(point, axis) <= limit.
//
// Вход без const намеренно: в ISO C до C23 указатель на массив нельзя
// молча подставить под указатель на массив с добавленным квалификатором,
// и gcc с -Wpedantic это отвергает.
static uint32_t ClipAgainstPlane(double input[8][3], uint32_t inputCount, const double axis[3],
                                 double limit, double output[8][3])
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < inputCount && count < 8u; ++index)
    {
        const double *current = input[index];
        const double *next = input[(index + 1u) % inputCount];
        double currentDistance = Dot3(current, axis) - limit;
        double nextDistance = Dot3(next, axis) - limit;

        if (currentDistance <= 0.0)
        {
            for (int32_t component = 0; component < 3; ++component)
            {
                output[count][component] = current[component];
            }
            ++count;
        }
        if (currentDistance * nextDistance < 0.0 && count < 8u)
        {
            double fraction = currentDistance / (currentDistance - nextDistance);
            for (int32_t component = 0; component < 3; ++component)
            {
                output[count][component] =
                    current[component] + (next[component] - current[component]) * fraction;
            }
            ++count;
        }
    }
    return count;
}

static bool BuildBoxManifold(const RigidBodyCache *first, const double *firstHalf,
                             const RigidBodyCache *second, const double *secondHalf,
                             BoxManifold *outManifold)
{
    double separation[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        separation[axis] = first->position[axis] - second->position[axis];
    }

    double bestOverlap = DBL_MAX;
    double bestAxis[3] = {0.0, 0.0, 1.0};
    bool referenceIsSecond = true;
    for (int32_t which = 0; which < 6; ++which)
    {
        const RigidBodyCache *owner = which < 3 ? first : second;
        const double *axis = owner->columns[which % 3];
        double reach = BoxRadius(first, firstHalf, axis) + BoxRadius(second, secondHalf, axis);
        double distance = Dot3(separation, axis);
        double overlap = reach - AbsoluteDouble(distance);
        if (overlap <= 0.0)
        {
            return false;
        }
        if (overlap < bestOverlap)
        {
            bestOverlap = overlap;
            // Нормаль всегда смотрит из второго тела в первое.
            double sign = distance < 0.0 ? -1.0 : 1.0;
            for (int32_t component = 0; component < 3; ++component)
            {
                bestAxis[component] = axis[component] * sign;
            }
            referenceIsSecond = which >= 3;
        }
    }

    // Полный SAT также обязан проверять пары рёбер. У почти параллельных
    // рёбер cross-ось вырождается и не является разделяющим направлением.
    for (int32_t firstAxis = 0; firstAxis < 3; ++firstAxis)
    {
        for (int32_t secondAxis = 0; secondAxis < 3; ++secondAxis)
        {
            double crossAxis[3];
            Cross3(first->columns[firstAxis], second->columns[secondAxis], crossAxis);
            double lengthSquared = Dot3(crossAxis, crossAxis);
            if (!(lengthSquared > 1.0e-12))
            {
                continue;
            }
            double inverseLength = 1.0 / SquareRoot(lengthSquared);
            for (int32_t component = 0; component < 3; ++component)
            {
                crossAxis[component] *= inverseLength;
            }
            double reach = BoxRadius(first, firstHalf, crossAxis) +
                           BoxRadius(second, secondHalf, crossAxis);
            double overlap = reach - AbsoluteDouble(Dot3(separation, crossAxis));
            if (overlap <= 0.0)
            {
                return false;
            }
        }
    }

    const RigidBodyCache *reference = referenceIsSecond ? second : first;
    const double *referenceHalf = referenceIsSecond ? secondHalf : firstHalf;
    const RigidBodyCache *incident = referenceIsSecond ? first : second;
    const double *incidentHalf = referenceIsSecond ? firstHalf : secondHalf;

    // Нормаль опорной грани смотрит в сторону падающего тела.
    double referenceNormal[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        referenceNormal[axis] = referenceIsSecond ? bestAxis[axis] : -bestAxis[axis];
    }

    double incidentVertices[4][3];
    IncidentFace(incident, incidentHalf, referenceNormal, incidentVertices);

    double polygon[8][3];
    double scratch[8][3];
    uint32_t count = 4u;
    for (uint32_t index = 0; index < 4u; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            polygon[index][axis] = incidentVertices[index][axis];
        }
    }

    // Боковые плоскости опорной грани: те две оси, что не совпадают с нормалью.
    int32_t normalAxis = 0;
    double bestAlignment = -DBL_MAX;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double alignment = AbsoluteDouble(Dot3(reference->columns[axis], referenceNormal));
        if (alignment > bestAlignment)
        {
            bestAlignment = alignment;
            normalAxis = axis;
        }
    }
    for (int32_t side = 1; side < 3; ++side)
    {
        int32_t axisIndex = (normalAxis + side) % 3;
        const double *axis = reference->columns[axisIndex];
        double centre = Dot3(reference->position, axis);
        double half = referenceHalf[axisIndex];

        count = ClipAgainstPlane(polygon, count, axis, centre + half, scratch);
        if (count == 0u)
        {
            return false;
        }
        double negated[3] = {-axis[0], -axis[1], -axis[2]};
        count = ClipAgainstPlane(scratch, count, negated, -(centre - half), polygon);
        if (count == 0u)
        {
            return false;
        }
    }

    // Плоскость опорной грани: точки глубже неё и есть контакты.
    double planeDistance =
        Dot3(reference->position, referenceNormal) + referenceHalf[normalAxis];

    outManifold->count = 0u;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outManifold->normal[axis] = bestAxis[axis];
    }
    for (uint32_t index = 0; index < count && outManifold->count < RIGID_MANIFOLD_POINTS; ++index)
    {
        double depth = planeDistance - Dot3(polygon[index], referenceNormal);
        if (depth <= 0.0)
        {
            continue;
        }
        uint32_t slot = outManifold->count++;
        outManifold->depth[slot] = depth;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            outManifold->point[slot][axis] = polygon[index][axis];
        }
    }
    return outManifold->count != 0u;
}

// Ячейка тела в равномерной сетке. Размер ячейки — диаметр самого
// крупного тела, поэтому пересекаться могут только соседи через одну.
static void BodyCell(const RigidBodyCache *cache, double cellSize, int64_t outCell[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double scaled = cache->position[axis] / cellSize;
        // Тело за пределами разумных координат кладётся в нулевую ячейку:
        // столкновений у него всё равно не будет, а арифметика не сорвётся.
        outCell[axis] = 0;
        if (scaled > -9.0e15 && scaled < 9.0e15)
        {
            (void)TryFloorToInt64(scaled, &outCell[axis]);
        }
    }
}

static uint32_t CellHash(const int64_t cell[3], uint32_t mask)
{
    uint64_t hash = (uint64_t)cell[0] * 73856093ull ^ (uint64_t)cell[1] * 19349663ull ^
                    (uint64_t)cell[2] * 83492791ull;
    hash ^= hash >> 33;
    return (uint32_t)hash & mask;
}

// Раскладывает тела по сетке. Без этого пары перебирались бы сплошь, и
// стоимость шага росла бы квадратично: на четырёх сотнях тел это уже
// весь бюджет кадра.
static void BuildBroadphase(const VoxelRigidBody *bodies, uint32_t bodyCount,
                            RigidStepScratch *scratch)
{
    (void)bodyCount;
    double largest = 0.0;
    for (uint32_t ordered = 0; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        if (!scratch->caches[index].collidable)
        {
            continue;
        }
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            if (bodies[index].halfExtent[axis] > largest)
            {
                largest = bodies[index].halfExtent[axis];
            }
        }
    }
    scratch->cellSize = largest > 0.0 ? largest * 2.0 : 1.0;

    for (uint32_t index = 0; index < scratch->bucketCount; ++index)
    {
        scratch->buckets[index] = RIGID_HASH_EMPTY;
    }

    uint32_t mask = scratch->bucketCount - 1u;
    // Вставляем в обратном stable-порядке: начало каждой цепочки после
    // этого обходится по возрастанию stableId независимо от порядка массива.
    for (uint32_t ordered = scratch->activeCount; ordered > 0u; --ordered)
    {
        uint32_t index = scratch->order[ordered - 1u];
        scratch->next[index] = RIGID_HASH_EMPTY;
        if (!bodies[index].active || !scratch->caches[index].collidable)
        {
            continue;
        }
        int64_t cell[3];
        BodyCell(&scratch->caches[index], scratch->cellSize, cell);
        uint32_t bucket = CellHash(cell, mask);
        scratch->next[index] = scratch->buckets[bucket];
        scratch->buckets[bucket] = index;
    }
}

static void AppendPairContacts(const VoxelRigidBody *bodies, uint32_t first, uint32_t second,
                               RigidStepScratch *scratch)
{
    const RigidBodyCache *firstCache = &scratch->caches[first];
    const RigidBodyCache *secondCache = &scratch->caches[second];

    BoxManifold manifold;
    if (!BuildBoxManifold(firstCache, bodies[first].halfExtent, secondCache,
                          bodies[second].halfExtent, &manifold))
    {
        return;
    }

    double restitution = bodies[first].restitution < bodies[second].restitution
                             ? bodies[first].restitution
                             : bodies[second].restitution;
    double friction = bodies[first].friction < bodies[second].friction ? bodies[first].friction
                                                                       : bodies[second].friction;
    for (uint32_t index = 0; index < manifold.count; ++index)
    {
        RigidContact contact;
        memset(&contact, 0, sizeof(contact));
        contact.bodyIndex = first;
        contact.otherIndex = second;
        contact.depth = manifold.depth[index];
        contact.restitution = restitution;
        contact.friction = friction;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            contact.normal[axis] = manifold.normal[axis];
            contact.point[axis] = manifold.point[index][axis];
        }
        if (!AppendContact(scratch, &contact))
        {
            return;
        }
    }
}

static void CollectBodyContacts(const VoxelRigidBody *bodies, uint32_t bodyCount,
                                RigidStepScratch *scratch)
{
    BuildBroadphase(bodies, bodyCount, scratch);
    uint32_t mask = scratch->bucketCount - 1u;

    for (uint32_t ordered = 0; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t first = scratch->order[ordered];
        const RigidBodyCache *firstCache = &scratch->caches[first];
        if (!bodies[first].active || !firstCache->collidable)
        {
            continue;
        }
        int64_t cell[3];
        BodyCell(firstCache, scratch->cellSize, cell);

        for (int32_t dx = -1; dx <= 1; ++dx)
        {
            for (int32_t dy = -1; dy <= 1; ++dy)
            {
                for (int32_t dz = -1; dz <= 1; ++dz)
                {
                    int64_t neighbour[3] = {cell[0] + dx, cell[1] + dy, cell[2] + dz};
                    uint32_t bucket = CellHash(neighbour, mask);
                    for (uint32_t second = scratch->buckets[bucket]; second != RIGID_HASH_EMPTY;
                         second = scratch->next[second])
                    {
                        // Хеш не является координатой: несколько из 27
                        // соседних ключей часто попадают в одну корзину. Без
                        // точной проверки ячейки одна пара добавлялась бы
                        // несколько раз и получала бы лишний импульс.
                        int64_t secondCell[3];
                        BodyCell(&scratch->caches[second], scratch->cellSize, secondCell);
                        if (secondCell[0] != neighbour[0] || secondCell[1] != neighbour[1] ||
                            secondCell[2] != neighbour[2])
                        {
                            continue;
                        }
                        // Пара берётся один раз в порядке стабильных ID.
                        if (bodies[second].stableId <= bodies[first].stableId)
                        {
                            continue;
                        }
                        AppendPairContacts(bodies, first, second, scratch);
                    }
                }
            }
        }
    }
}

// Обратная инерция в мире применяется к вектору: R diag(invI) R^T v.
static void ApplyInverseInertia(const RigidBodyCache *cache, const double inverseInertia[3],
                                const double vector[3], double out[3])
{
    double body[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        body[axis] = Dot3(vector, cache->columns[axis]) * inverseInertia[axis];
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        out[axis] = cache->columns[0][axis] * body[0] + cache->columns[1][axis] * body[1] +
                    cache->columns[2][axis] * body[2];
    }
}

static void ContactVelocity(const RigidBodyCache *cache, const double point[3], double out[3])
{
    double lever[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        lever[axis] = point[axis] - cache->position[axis];
    }
    double rotational[3];
    Cross3(cache->angular, lever, rotational);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        out[axis] = cache->linear[axis] + rotational[axis];
    }
}

static double EffectiveMass(const VoxelRigidBody *body, const RigidBodyCache *cache,
                            const double point[3], const double direction[3])
{
    double lever[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        lever[axis] = point[axis] - cache->position[axis];
    }
    double torque[3];
    Cross3(lever, direction, torque);
    double angular[3];
    ApplyInverseInertia(cache, body->inverseInertia, torque, angular);
    double back[3];
    Cross3(angular, lever, back);
    return body->inverseMass + Dot3(direction, back);
}

static void ApplyImpulse(const VoxelRigidBody *body, RigidBodyCache *cache, const double point[3],
                         const double direction[3], double magnitude)
{
    double lever[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        lever[axis] = point[axis] - cache->position[axis];
        cache->linear[axis] += direction[axis] * magnitude * body->inverseMass;
    }
    double torque[3];
    Cross3(lever, direction, torque);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        torque[axis] *= magnitude;
    }
    double angular[3];
    ApplyInverseInertia(cache, body->inverseInertia, torque, angular);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        cache->angular[axis] += angular[axis];
    }
}

// Два орта, перпендикулярных нормали: направления трения.
static void BuildTangents(const double normal[3], double first[3], double second[3])
{
    double reference[3] = {1.0, 0.0, 0.0};
    if (AbsoluteDouble(normal[0]) > 0.7)
    {
        reference[0] = 0.0;
        reference[1] = 1.0;
    }
    Cross3(normal, reference, first);
    double length = SquareRoot(Dot3(first, first));
    if (!(length > 0.0))
    {
        first[0] = 1.0;
        first[1] = 0.0;
        first[2] = 0.0;
        length = 1.0;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        first[axis] /= length;
    }
    Cross3(normal, first, second);
}

static void SolveContacts(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                          const VoxelRigidStepSettings *settings)
{
    for (uint32_t iteration = 0; iteration < settings->solverIterations; ++iteration)
    {
        for (uint32_t index = 0; index < scratch->contactCount; ++index)
        {
            RigidContact *contact = &scratch->contacts[index];
            const VoxelRigidBody *body = &bodies[contact->bodyIndex];
            RigidBodyCache *cache = &scratch->caches[contact->bodyIndex];
            bool paired = contact->otherIndex != UINT32_MAX;
            const VoxelRigidBody *other = paired ? &bodies[contact->otherIndex] : NULL;
            RigidBodyCache *otherCache = paired ? &scratch->caches[contact->otherIndex] : NULL;

            double velocity[3];
            ContactVelocity(cache, contact->point, velocity);
            if (paired)
            {
                double otherVelocity[3];
                ContactVelocity(otherCache, contact->point, otherVelocity);
                for (int32_t axis = 0; axis < 3; ++axis)
                {
                    velocity[axis] -= otherVelocity[axis];
                }
            }

            double normalSpeed = Dot3(velocity, contact->normal);
            double mass = EffectiveMass(body, cache, contact->point, contact->normal);
            if (paired)
            {
                mass += EffectiveMass(other, otherCache, contact->point, contact->normal);
            }
            if (!(mass > 0.0) || !IsFiniteDouble(mass))
            {
                continue;
            }

            double penetration = contact->depth - settings->penetrationSlop;
            double bias = penetration > 0.0 ? settings->penetrationCorrection * penetration /
                                                  RIGID_STEP_SECONDS
                                            : 0.0;
            double bounce = normalSpeed < -RIGID_RESTITUTION_THRESHOLD
                                ? -contact->restitution * normalSpeed
                                : 0.0;

            double magnitude = (-normalSpeed + bias + bounce) / mass;
            // Накопленный импульс не бывает отрицательным: контакт умеет
            // только отталкивать. Клипуется сумма, а не шаг, иначе решатель
            // не сходится на стопке.
            double previous = contact->normalImpulse;
            double accumulated = previous + magnitude;
            if (accumulated < 0.0)
            {
                accumulated = 0.0;
            }
            magnitude = accumulated - previous;
            contact->normalImpulse = accumulated;

            if (magnitude != 0.0 && IsFiniteDouble(magnitude))
            {
                ApplyImpulse(body, cache, contact->point, contact->normal, magnitude);
                if (paired)
                {
                    ApplyImpulse(other, otherCache, contact->point, contact->normal, -magnitude);
                }
            }

            if (!(contact->friction > 0.0) || !(contact->normalImpulse > 0.0))
            {
                continue;
            }

            double tangents[2][3];
            BuildTangents(contact->normal, tangents[0], tangents[1]);
            for (int32_t which = 0; which < 2; ++which)
            {
                ContactVelocity(cache, contact->point, velocity);
                if (paired)
                {
                    double otherVelocity[3];
                    ContactVelocity(otherCache, contact->point, otherVelocity);
                    for (int32_t axis = 0; axis < 3; ++axis)
                    {
                        velocity[axis] -= otherVelocity[axis];
                    }
                }
                double tangentSpeed = Dot3(velocity, tangents[which]);
                double tangentMass = EffectiveMass(body, cache, contact->point, tangents[which]);
                if (paired)
                {
                    tangentMass +=
                        EffectiveMass(other, otherCache, contact->point, tangents[which]);
                }
                if (!(tangentMass > 0.0) || !IsFiniteDouble(tangentMass))
                {
                    continue;
                }
                double friction = -tangentSpeed / tangentMass;
                double limit = contact->friction * contact->normalImpulse;
                if (friction > limit)
                {
                    friction = limit;
                }
                if (friction < -limit)
                {
                    friction = -limit;
                }
                if (friction == 0.0 || !IsFiniteDouble(friction))
                {
                    continue;
                }
                ApplyImpulse(body, cache, contact->point, tangents[which], friction);
                if (paired)
                {
                    ApplyImpulse(other, otherCache, contact->point, tangents[which], -friction);
                }
            }
        }
    }
}

// Поворот за шаг. Составляющие приводятся к (-pi, pi] точным остатком от
// деления: при обычной скорости это само значение, а при невероятной —
// единственный осмысленный ответ, потому что оборот кратный 2pi неотличим
// от его отсутствия.
static bool AngularStepRadians(const InfiniteCoord *component, double *outRadians)
{
    InfiniteCoord perStep;
    InfiniteCoordInit(&perStep);
    if (!InfiniteCoordTryCopyShiftRight(&perStep, component, VOXEL_RIGID_STEP_SHIFT))
    {
        InfiniteCoordDestroy(&perStep);
        return false;
    }

    // 2*pi в масштабе 2^32.
    const uint64_t fullTurn = 26986075409ull;
    uint64_t remainder = 0;
    (void)InfiniteCoordDivFloorSmallLow(&perStep, fullTurn, &remainder);
    InfiniteCoordDestroy(&perStep);

    double radians = (double)remainder / 4294967296.0;
    const double twoPi = 6.283185307179586;
    if (radians > twoPi * 0.5)
    {
        radians -= twoPi;
    }
    *outRadians = radians;
    return true;
}

static bool IntegrateBody(VoxelRigidBody *body, const RigidBodyCache *cache)
{
    // Скорость возвращается в произвольную точность добавкой разницы:
    // решатель считал в double, а хранится величина без потолка.
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (cache->collidable)
        {
            double linearDelta = cache->linear[axis] - FixedToDouble(&body->linearVelocity[axis]);
            double angularDelta = cache->angular[axis] - FixedToDouble(&body->angularVelocity[axis]);
            if (!AddDoubleToFixed(&body->linearVelocity[axis], linearDelta) ||
                !AddDoubleToFixed(&body->angularVelocity[axis], angularDelta))
            {
                return false;
            }
        }

        InfiniteCoord travel;
        InfiniteCoordInit(&travel);
        if (!InfiniteCoordTryCopyShiftRight(&travel, &body->linearVelocity[axis],
                                            VOXEL_RIGID_STEP_SHIFT))
        {
            return false;
        }
        InfiniteCoord moved;
        InfiniteCoordInit(&moved);
        bool ok = InfiniteCoordTryAdd(&moved, &body->position[axis], &travel);
        InfiniteCoordDestroy(&travel);
        if (!ok)
        {
            return false;
        }
        InfiniteCoordDestroy(&body->position[axis]);
        body->position[axis] = moved;
    }

    double rotation[3];
    double angle = 0.0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!AngularStepRadians(&body->angularVelocity[axis], &rotation[axis]))
        {
            return false;
        }
        angle += rotation[axis] * rotation[axis];
    }
    angle = SquareRoot(angle);
    if (angle > 1e-12)
    {
        double half = angle * 0.5;
        double sine = (double)ScalarSin((float)half);
        double cosine = (double)ScalarCos((float)half);
        double increment[4];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            increment[axis] = rotation[axis] / angle * sine;
        }
        increment[3] = cosine;
        double updated[4];
        QuaternionMultiply(increment, body->orientation, updated);
        for (int32_t index = 0; index < 4; ++index)
        {
            body->orientation[index] = updated[index];
        }
        QuaternionNormalize(body->orientation);
    }
    return true;
}

bool VoxelRigidBodyStep(VoxelRigidBody *bodies, uint32_t bodyCount,
                        const VoxelCollisionSource *collision,
                        const VoxelRigidStepSettings *settings, void *scratch,
                        uint32_t scratchBytes)
{
    if (bodies == NULL || settings == NULL || scratch == NULL || bodyCount == 0u ||
        bodyCount > VOXEL_RIGID_MAX_BODIES || collision == NULL ||
        collision->queryBlockPhysics == NULL)
    {
        return false;
    }
    if (!(settings->penetrationCorrection >= 0.0 && settings->penetrationCorrection <= 1.0) ||
        !(settings->penetrationSlop >= 0.0) || settings->solverIterations == 0u)
    {
        return false;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!IsFiniteDouble(settings->gravity[axis]))
        {
            return false;
        }
    }
    uint32_t required = VoxelRigidBodyStepScratchBytes(bodyCount);
    if (required == 0u || scratchBytes < required)
    {
        return false;
    }

    VoxelPhysicsConfigureThread();

    RigidStepScratch state;
    uintptr_t scratchAddress = (uintptr_t)scratch;
    uintptr_t alignedAddress = (scratchAddress + 63u) & ~(uintptr_t)63u;
    uint8_t *cursor = (uint8_t *)(void *)alignedAddress;
    state.caches = (RigidBodyCache *)(void *)cursor;
    cursor += (size_t)bodyCount * sizeof(RigidBodyCache);
    state.contacts = (RigidContact *)(void *)cursor;
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidContact);
    state.next = (uint32_t *)(void *)cursor;
    cursor += (size_t)bodyCount * sizeof(uint32_t);
    state.order = (uint32_t *)(void *)cursor;
    cursor += (size_t)bodyCount * sizeof(uint32_t);
    state.buckets = (uint32_t *)(void *)cursor;
    state.bucketCount = BucketCountFor(bodyCount);
    state.cellSize = 1.0;
    state.contactCount = 0u;
    state.contactCapacity = bodyCount * RIGID_CONTACTS_PER_BODY;
    if (!BuildStableOrder(bodies, bodyCount, &state))
    {
        return false;
    }

    // Гравитация до построения контактов: решатель обязан видеть скорость,
    // с которой тело действительно подходит к опоре, иначе оно продавливает
    // её на величину шага и всплывает обратно на следующем.
    for (uint32_t ordered = 0; ordered < state.activeCount; ++ordered)
    {
        uint32_t index = state.order[ordered];
        double delta[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            delta[axis] = settings->gravity[axis] * RIGID_STEP_SECONDS;
        }
        if (!VoxelRigidBodyAddLinearVelocity(&bodies[index], delta))
        {
            return false;
        }
    }

    memset(state.caches, 0, (size_t)bodyCount * sizeof(RigidBodyCache));
    for (uint32_t ordered = 0; ordered < state.activeCount; ++ordered)
    {
        uint32_t index = state.order[ordered];
        BuildCache(&bodies[index], &state.caches[index]);
    }

    CollectWorldContacts(bodies, bodyCount, collision, &state);
    CollectBodyContacts(bodies, bodyCount, &state);
    SolveContacts(bodies, &state, settings);

    for (uint32_t ordered = 0; ordered < state.activeCount; ++ordered)
    {
        uint32_t index = state.order[ordered];
        if (!IntegrateBody(&bodies[index], &state.caches[index]))
        {
            return false;
        }
    }
    return true;
}
