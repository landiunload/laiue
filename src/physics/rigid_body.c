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
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return ((representation.bits >> 52) & 0x7ffu) != 0x7ffu;
}

// Binary64 hardware sqrt is correctly rounded in the configured FP mode.
// A float seed over/underflows for perfectly finite double effective masses.
static double SquareRoot(double value)
{
    return ScalarSqrtDouble(value);
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
    if (!IsFiniteDouble(value) || value < -9223372036854775808.0 || value >= 9223372036854775808.0)
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
    if (value->limbCount == 0u || value->sign == 0)
    {
        return 0.0;
    }
    if (value->limbCount == 1u && value->limbs != NULL)
    {
        double magnitude = (double)value->limbs[0] / 4294967296.0;
        return value->sign < 0 ? -magnitude : magnitude;
    }
    double raw = InfiniteCoordToDoubleSaturating(value);
    // Деление на 2^32 точное: это лишь сдвиг экспоненты.
    return raw / 4294967296.0;
}

// Most bodies stay in the signed 64-bit fixed-point window.  Keep that hot
// path allocation-free: InfiniteCoord is still the authoritative public
// representation, but its single limb is updated in place.  Wider values
// fall back to the arbitrary-precision path below.
static bool TryFixedInt64(const InfiniteCoord *value, int64_t *outValue)
{
    if (value == NULL || outValue == NULL)
    {
        return false;
    }
    if (value->limbCount == 0u)
    {
        *outValue = 0;
        return true;
    }
    if (value->limbCount != 1u || value->limbs == NULL)
    {
        return false;
    }
    uint64_t magnitude = value->limbs[0];
    if (value->sign > 0)
    {
        if (magnitude > (uint64_t)INT64_MAX)
        {
            return false;
        }
        *outValue = (int64_t)magnitude;
        return true;
    }
    if (value->sign < 0)
    {
        if (magnitude > (1ull << 63))
        {
            return false;
        }
        *outValue = magnitude == (1ull << 63) ? INT64_MIN : -(int64_t)magnitude;
        return true;
    }
    *outValue = 0;
    return magnitude == 0u;
}

static bool TryDoubleToFixedInt64(double value, int64_t *outValue)
{
    if (outValue == NULL || !IsFiniteDouble(value))
    {
        return false;
    }
    double scaled = value * 4294967296.0;
    if (!IsFiniteDouble(scaled) || scaled < -9223372036854775808.0 ||
        scaled >= 9223372036854775808.0)
    {
        return false;
    }
    int64_t converted = (int64_t)scaled;
    if ((double)converted != scaled)
    {
        return false;
    }
    *outValue = converted;
    return true;
}

static bool TryShiftRightInt64TowardZero(int64_t value, uint32_t shift, int64_t *outValue)
{
    if (outValue == NULL || shift >= 64u)
    {
        return false;
    }
    if (value >= 0)
    {
        *outValue = value >> shift;
        return true;
    }
    uint64_t magnitude = 0u - (uint64_t)value;
    uint64_t shifted = magnitude >> shift;
    if (shifted > (1ull << 63))
    {
        return false;
    }
    *outValue = shifted == (1ull << 63) ? INT64_MIN : -(int64_t)shifted;
    return true;
}

static bool TryAddDoubleToFixed(InfiniteCoord *outValue, const InfiniteCoord *value, double delta)
{
    if (outValue == NULL || value == NULL || !IsFiniteDouble(delta))
    {
        return false;
    }
    if (delta == 0.0)
    {
        return InfiniteCoordTryCopyAddInt64(outValue, value, 0);
    }

    int64_t fixedDelta = 0;
    if (TryDoubleToFixedInt64(delta, &fixedDelta))
    {
        if (outValue == value)
        {
            return InfiniteCoordTryAddInt64InPlace(outValue, fixedDelta);
        }
        return InfiniteCoordTryCopyAddInt64(outValue, value, fixedDelta);
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
    int64_t fixedDelta = 0;
    if (TryDoubleToFixedInt64(delta, &fixedDelta))
    {
        return InfiniteCoordTryAddInt64InPlace(value, fixedDelta);
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
    outSettings->sleepLinearSpeed = 0.02;
    outSettings->sleepAngularSpeed = 0.05;
    outSettings->sleepFrames = 30u;
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

void VoxelRigidBodyWake(VoxelRigidBody *body)
{
    if (body == NULL)
    {
        return;
    }
    body->sleeping = false;
    body->sleepCounter = 0u;
}

bool VoxelRigidBodyInitialize(VoxelRigidBody *body, uint64_t stableId,
                              const VoxelRigidBodyDescription *description)
{
    if (body == NULL)
    {
        return false;
    }

    VoxelPhysicsConfigureThread();
    // Тело приводится в пригодное состояние до любой проверки: заголовок
    // обещает, что после неудачи его можно освободить, а необнулённая
    // память со стека вызывающего этого не позволяет.
    VoxelRigidBody empty = {0};
    *body = empty;
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
    if (!(body->inverseMass > 0.0) || !IsFiniteDouble(body->inverseMass))
    {
        return false;
    }
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
        if (!(body->inverseInertia[axis] > 0.0) || !IsFiniteDouble(body->inverseInertia[axis]))
        {
            return false;
        }
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

    VoxelPhysicsConfigureThread();
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
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (changed[axis])
        {
            body->sleeping = false;
            body->sleepCounter = 0u;
            break;
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

    VoxelPhysicsConfigureThread();
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
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (changed[axis])
        {
            body->sleeping = false;
            body->sleepCounter = 0u;
            break;
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
    // Mass is constant during this tick. Keep it beside the solver velocities
    // so applying impulses does not revisit the larger public body array.
    double inverseMass;
    // Столбцы матрицы поворота и мировая диагональ обратной инерции.
    double columns[3][3];
    double aabbMin[3];
    double aabbMax[3];
    int64_t cell[3];
    bool hasWorldContact;
    bool hasBodyContact;
    bool collidable;
    // Per-first-body narrowphase count; occupies former tail padding.
    uint32_t candidatePairs;
} RigidBodyCache;

// Geometry and inertia do not change during velocity iterations. Prepare the
// three constraint directions once instead of rotating inertia tensors again
// for every impulse on every iteration.
typedef struct RigidConstraintRow
{
    double direction[3];
    double angularResponse[2][3];
    double inverseEffectiveMass;
} RigidConstraintRow;

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
    RigidConstraintRow rows[3];
    double targetNormalSpeed;
    double tangentImpulse[2];
    double tangentCrossMass;
} RigidContact;

typedef struct RigidCachedContact
{
    uint64_t stableIds[2];
    double localAnchors[2][3];
    double worldImpulse[3];
    float normal[3];
    uint32_t next;
    bool used;
} RigidCachedContact;

#define RIGID_CACHE_ANCHOR_DISTANCE_SQUARED 0.0001
#define RIGID_CACHE_NORMAL_ALIGNMENT 0.995

#define RIGID_SOLVER_COLOR_COUNT 64u
#define RIGID_SOLVER_BATCH_COUNT (RIGID_SOLVER_COLOR_COUNT + 1u)

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
    // Wake traversal and contact-island union/find are separate from hash links.
    // The queue is reused for island flags once contact discovery has finished.
    uint32_t *wakeQueue;
    uint32_t *islandParents;
    uint64_t *bodyColors;
    uint8_t *contactColors;
    uint32_t *solveOrder;
    uint32_t batchOffsets[RIGID_SOLVER_BATCH_COUNT + 1u];
    const VoxelRigidStepOptions *options;
    uint32_t bucketCount;
    uint32_t candidateCapacity;
    VoxelRigidBroadphase *broadphase;
    uint32_t activeCount;
    double cellSize;
    uint32_t contactCount;
    uint32_t contactCapacity;
    bool contactOverflow;
    VoxelRigidStepStats *stats;
} RigidStepScratch;

static double ProfileNow(const RigidStepScratch *scratch)
{
    const VoxelRigidStepOptions *options = scratch->options;
    return options != NULL && options->profile != NULL && options->clockSeconds != NULL
               ? options->clockSeconds(options->clockContext)
               : 0.0;
}

static void ProfileFinish(const RigidStepScratch *scratch, VoxelRigidProfileStage stage,
                          double begin)
{
    const VoxelRigidStepOptions *options = scratch->options;
    if (options != NULL && options->profile != NULL && options->clockSeconds != NULL)
    {
        double elapsed = options->clockSeconds(options->clockContext) - begin;
        options->profile->seconds[stage] =
            IsFiniteDouble(elapsed) && elapsed >= 0.0 ? elapsed : 0.0;
    }
}

static void ExecuteRange(const RigidStepScratch *scratch, uint32_t count, uint32_t grain,
                         LaiueTaskRangeFunction function, void *context)
{
    const LaiueTaskExecutor *executor =
        scratch->options != NULL ? scratch->options->executor : NULL;
    if (executor != NULL && count > grain)
    {
        executor->run(executor->context, count, grain, function, context);
    }
    else
    {
        function(context, 0u, count);
    }
}

static bool BuildStableOrder(const VoxelRigidBody *bodies, uint32_t bodyCount,
                             RigidStepScratch *scratch)
{
    scratch->activeCount = 0u;
    bool alreadySorted = true;
    uint64_t previousId = 0u;
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
        if (scratch->activeCount != 0u && bodies[index].stableId <= previousId)
        {
            alreadySorted = false;
        }
        previousId = bodies[index].stableId;
        scratch->order[scratch->activeCount++] = index;
    }

    if (alreadySorted)
    {
        return true;
    }

    // Stable LSD radix sort is deterministic for arbitrary body-array order
    // and linear in the number of active bodies.  The old heap sort made a
    // 250k-body world pay roughly 18 comparisons per object on every tick.
    // `next` is not used by the broadphase until after this function, so it
    // doubles as the no-allocation scratch buffer for the eight byte passes.
    uint32_t count = scratch->activeCount;
    uint32_t *source = scratch->order;
    uint32_t *destination = scratch->next;
    uint32_t counts[256];
    for (uint32_t pass = 0u; pass < 8u; ++pass)
    {
        for (uint32_t bucket = 0u; bucket < 256u; ++bucket)
        {
            counts[bucket] = 0u;
        }
        uint32_t shift = pass * 8u;
        for (uint32_t index = 0u; index < scratch->activeCount; ++index)
        {
            uint64_t id = bodies[source[index]].stableId;
            ++counts[(uint32_t)((id >> shift) & 0xffu)];
        }
        uint32_t offset = 0u;
        for (uint32_t bucket = 0u; bucket < 256u; ++bucket)
        {
            uint32_t length = counts[bucket];
            counts[bucket] = offset;
            offset += length;
        }
        for (uint32_t index = 0u; index < scratch->activeCount; ++index)
        {
            uint64_t id = bodies[source[index]].stableId;
            uint32_t bucket = (uint32_t)((id >> shift) & 0xffu);
            destination[counts[bucket]++] = source[index];
        }
        uint32_t *temporary = source;
        source = destination;
        destination = temporary;
    }
    if (source != scratch->order)
    {
        for (uint32_t index = 0u; index < scratch->activeCount; ++index)
        {
            scratch->order[index] = source[index];
        }
    }

    for (uint32_t index = 1u; index < count; ++index)
    {
        if (bodies[scratch->order[index - 1u]].stableId == bodies[scratch->order[index]].stableId)
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

uint32_t VoxelRigidContactCacheBytes(uint32_t bodyCapacity)
{
    if (bodyCapacity == 0u || bodyCapacity > VOXEL_RIGID_MAX_BODIES)
    {
        return 0u;
    }
    uint64_t entries =
        (uint64_t)bodyCapacity * RIGID_CONTACTS_PER_BODY * sizeof(RigidCachedContact);
    uint64_t buckets = (uint64_t)BucketCountFor(bodyCapacity) * sizeof(uint32_t);
    uint64_t total = entries + buckets + 63u;
    return total > UINT32_MAX ? 0u : (uint32_t)total;
}

static RigidCachedContact *ContactCacheEntries(const VoxelRigidContactCache *cache)
{
    uintptr_t padding = (0u - (uintptr_t)cache->storage) & 63u;
    return (RigidCachedContact *)((uint8_t *)cache->storage + padding);
}

static uint32_t *ContactCacheBuckets(const VoxelRigidContactCache *cache)
{
    RigidCachedContact *entries = ContactCacheEntries(cache);
    return (uint32_t *)(entries + (size_t)cache->bodyCapacity * RIGID_CONTACTS_PER_BODY);
}

static bool ContactCacheValid(const VoxelRigidContactCache *cache, uint32_t bodyCount)
{
    if (cache == NULL || cache->storage == NULL || cache->bodyCapacity < bodyCount)
    {
        return false;
    }
    uint32_t required = VoxelRigidContactCacheBytes(cache->bodyCapacity);
    return required != 0u && cache->storageBytes >= required &&
           cache->contactCount <= cache->bodyCapacity * RIGID_CONTACTS_PER_BODY;
}

static bool MemoryRangesOverlap(const void *first, uint64_t firstBytes, const void *second,
                                uint64_t secondBytes)
{
    uintptr_t firstAddress = (uintptr_t)first;
    uintptr_t secondAddress = (uintptr_t)second;
    // Subtraction avoids overflow when a caller supplies an invalid large span.
    return firstBytes != 0u && secondBytes != 0u &&
           (firstAddress <= secondAddress ? secondAddress - firstAddress < firstBytes
                                          : firstAddress - secondAddress < secondBytes);
}

void VoxelRigidContactCacheReset(VoxelRigidContactCache *cache)
{
    if (cache == NULL)
    {
        return;
    }
    cache->contactCount = 0u;
    cache->matchedContactCount = 0u;
    if (!ContactCacheValid(cache, 0u))
    {
        return;
    }
    uint32_t *buckets = ContactCacheBuckets(cache);
    uint32_t bucketCount = BucketCountFor(cache->bodyCapacity);
    for (uint32_t index = 0u; index < bucketCount; ++index)
    {
        buckets[index] = RIGID_HASH_EMPTY;
    }
}

bool VoxelRigidContactCacheInitialize(VoxelRigidContactCache *cache, void *storage,
                                      uint32_t bodyCapacity, uint32_t storageBytes)
{
    if (cache == NULL || MemoryRangesOverlap(cache, sizeof(*cache), storage, storageBytes))
    {
        return false;
    }
    VoxelRigidContactCache empty = {0};
    *cache = empty;
    uint32_t required = VoxelRigidContactCacheBytes(bodyCapacity);
    if (storage == NULL || required == 0u || storageBytes < required)
    {
        return false;
    }
    cache->storage = storage;
    cache->storageBytes = storageBytes;
    cache->bodyCapacity = bodyCapacity;
    VoxelRigidContactCacheReset(cache);
    return true;
}

static uint32_t ContactCacheHash(uint64_t firstId, uint64_t secondId, uint32_t mask)
{
    uint64_t hash =
        firstId * UINT64_C(0x9e3779b185ebca87) ^ secondId * UINT64_C(0xc2b2ae3d27d4eb4f);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return (uint32_t)hash & mask;
}

uint32_t VoxelRigidBodyStepScratchBytes(uint32_t bodyCount)
{
    if (bodyCount == 0u || bodyCount > VOXEL_RIGID_MAX_BODIES)
    {
        return 0u;
    }
    uint64_t caches = (uint64_t)bodyCount * sizeof(RigidBodyCache);
    uint64_t contacts = (uint64_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidContact);
    uint64_t links = (uint64_t)bodyCount * sizeof(uint32_t) * 4u;
    uint64_t schedule =
        (uint64_t)bodyCount *
        (sizeof(uint64_t) + RIGID_CONTACTS_PER_BODY * (sizeof(uint8_t) + sizeof(uint32_t)));
    uint64_t buckets = (uint64_t)BucketCountFor(bodyCount) * sizeof(uint32_t);
    uint64_t total =
        caches + contacts + links + schedule + buckets + sizeof(VoxelRigidStepStats) + 64u;
    return total > 0xFFFFFFFFull ? 0u : (uint32_t)total;
}

static VoxelRigidStepStats *StepStatsPointer(void *scratch, uint32_t bodyCount)
{
    uintptr_t padding = (0u - (uintptr_t)scratch) & 63u;
    uint8_t *cursor = (uint8_t *)scratch + padding;
    cursor += (size_t)bodyCount * sizeof(RigidBodyCache);
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidContact);
    cursor += (size_t)bodyCount * sizeof(uint32_t) * 4u;
    cursor += (size_t)bodyCount *
              (sizeof(uint64_t) + RIGID_CONTACTS_PER_BODY * (sizeof(uint8_t) + sizeof(uint32_t)));
    cursor += (size_t)BucketCountFor(bodyCount) * sizeof(uint32_t);
    return (VoxelRigidStepStats *)cursor;
}

bool VoxelRigidBodyReadStepStats(const void *scratch, uint32_t bodyCount, uint32_t scratchBytes,
                                 VoxelRigidStepStats *outStats)
{
    uint32_t required = VoxelRigidBodyStepScratchBytes(bodyCount);
    if (scratch == NULL || outStats == NULL || required == 0u || scratchBytes < required)
    {
        return false;
    }
    *outStats = *StepStatsPointer((void *)scratch, bodyCount);
    return true;
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
    cache->inverseMass = body->inverseMass;
    cache->hasWorldContact = false;
    cache->hasBodyContact = false;
    cache->collidable = VoxelRigidBodyLocalPosition(body, cache->position);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        cache->linear[axis] = body->sleeping ? 0.0 : FixedToDouble(&body->linearVelocity[axis]);
        cache->angular[axis] = body->sleeping ? 0.0 : FixedToDouble(&body->angularVelocity[axis]);
        if (!IsFiniteDouble(cache->linear[axis]) || !IsFiniteDouble(cache->angular[axis]))
        {
            cache->collidable = false;
        }
    }
    bool axisAligned = body->orientation[0] == 0.0 && body->orientation[1] == 0.0 &&
                       body->orientation[2] == 0.0 &&
                       (body->orientation[3] == 1.0 || body->orientation[3] == -1.0);
    if (axisAligned)
    {
        cache->columns[0][0] = 1.0;
        cache->columns[0][1] = 0.0;
        cache->columns[0][2] = 0.0;
        cache->columns[1][0] = 0.0;
        cache->columns[1][1] = 1.0;
        cache->columns[1][2] = 0.0;
        cache->columns[2][0] = 0.0;
        cache->columns[2][1] = 0.0;
        cache->columns[2][2] = 1.0;
    }
    else
    {
        QuaternionToColumns(body->orientation, cache->columns);
    }

    // Legacy escape for unbounded speeds, not CCD or physically accurate
    // high-speed collision handling; see the public ballistic-mode contract.
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
        return;
    }

    // Cheap broadphase rejection for the SAT path.  The uniform grid only
    // bounds centres; most neighbouring cells still contain boxes whose AABBs
    // are disjoint, especially in sparse worlds.
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double radius = AbsoluteDouble(cache->columns[0][axis]) * body->halfExtent[0] +
                        AbsoluteDouble(cache->columns[1][axis]) * body->halfExtent[1] +
                        AbsoluteDouble(cache->columns[2][axis]) * body->halfExtent[2];
        cache->aabbMin[axis] = cache->position[axis] - radius;
        cache->aabbMax[axis] = cache->position[axis] + radius;
    }

    (void)axisAligned;
}

static bool AppendContact(RigidStepScratch *scratch, const RigidContact *contact)
{
    if (scratch->contactCount >= scratch->contactCapacity)
    {
        // Missing a physical constraint is not a successful simulation step.
        scratch->contactOverflow = true;
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
            if ((direction == 0 && block[axis] == INT64_MIN) ||
                (direction != 0 && block[axis] == INT64_MAX))
            {
                // An unrepresentable neighbour is not a known empty cell.
                continue;
            }
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
        if (!bodies[index].active || bodies[index].sleeping || !cache->collidable)
        {
            continue;
        }
        for (uint32_t corner = 0; corner < 8u; ++corner)
        {
            double sign[3] = {
                (corner & 1u) != 0u ? 1.0 : -1.0,
                (corner & 2u) != 0u ? 1.0 : -1.0,
                (corner & 4u) != 0u ? 1.0 : -1.0,
            };
            double point[3];
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                point[axis] = cache->position[axis] +
                              cache->columns[0][axis] * sign[0] * bodies[index].halfExtent[0] +
                              cache->columns[1][axis] * sign[1] * bodies[index].halfExtent[1] +
                              cache->columns[2][axis] * sign[2] * bodies[index].halfExtent[2];
            }
            RigidContact contact = {0};
            if (!ResolveBlockContact(collision, point, contact.normal, &contact.depth))
            {
                continue;
            }
            contact.bodyIndex = index;
            contact.otherIndex = UINT32_MAX;
            scratch->caches[index].hasWorldContact = true;
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                contact.point[axis] = point[axis];
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
static double BoxRadius(const RigidBodyCache *cache, const double *halfExtent, const double axis[3])
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

static double ClampEdgeDistance(double value, double halfLength)
{
    return value < -halfLength ? -halfLength : (value > halfLength ? halfLength : value);
}

static void SupportingEdgeCentre(const RigidBodyCache *box, const double *halfExtent, int32_t edge,
                                 const double direction[3], double centre[3])
{
    for (int32_t component = 0; component < 3; ++component)
    {
        centre[component] = box->position[component];
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (axis == edge)
        {
            continue;
        }
        double sign = Dot3(box->columns[axis], direction) < 0.0 ? -1.0 : 1.0;
        for (int32_t component = 0; component < 3; ++component)
        {
            centre[component] += box->columns[axis][component] * halfExtent[axis] * sign;
        }
    }
}

static void BuildEdgeContact(const RigidBodyCache *first, const double *firstHalf,
                             const RigidBodyCache *second, const double *secondHalf,
                             int32_t firstEdge, int32_t secondEdge, const double normal[3],
                             double depth, BoxManifold *manifold)
{
    double towardSecond[3] = {-normal[0], -normal[1], -normal[2]};
    double firstCentre[3];
    double secondCentre[3];
    SupportingEdgeCentre(first, firstHalf, firstEdge, towardSecond, firstCentre);
    SupportingEdgeCentre(second, secondHalf, secondEdge, normal, secondCentre);
    const double *firstDirection = first->columns[firstEdge];
    const double *secondDirection = second->columns[secondEdge];
    double separation[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        separation[axis] = firstCentre[axis] - secondCentre[axis];
    }
    double alignment = Dot3(firstDirection, secondDirection);
    double firstProjection = Dot3(firstDirection, separation);
    double secondProjection = Dot3(secondDirection, separation);
    double firstLength = Dot3(firstDirection, firstDirection);
    double secondLength = Dot3(secondDirection, secondDirection);
    double determinant = firstLength * secondLength - alignment * alignment;
    double firstDistance =
        determinant > 0.0
            ? (alignment * secondProjection - secondLength * firstProjection) / determinant
            : 0.0;
    firstDistance = ClampEdgeDistance(firstDistance, firstHalf[firstEdge]);
    double secondDistance = (alignment * firstDistance + secondProjection) / secondLength;
    double boundedSecond = ClampEdgeDistance(secondDistance, secondHalf[secondEdge]);
    if (boundedSecond != secondDistance)
    {
        firstDistance = ClampEdgeDistance(
            (alignment * boundedSecond - firstProjection) / firstLength, firstHalf[firstEdge]);
    }
    manifold->count = 1u;
    manifold->depth[0] = depth;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        manifold->normal[axis] = normal[axis];
        manifold->point[0][axis] = (firstCentre[axis] + firstDirection[axis] * firstDistance +
                                    secondCentre[axis] + secondDirection[axis] * boundedSecond) *
                                   0.5;
    }
}

static bool BuildBoxManifold(const RigidBodyCache *first, const double *firstHalf,
                             const RigidBodyCache *second, const double *secondHalf,
                             BoxManifold *outManifold)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (first->aabbMax[axis] <= second->aabbMin[axis] ||
            second->aabbMax[axis] <= first->aabbMin[axis])
        {
            return false;
        }
    }
    double separation[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        separation[axis] = first->position[axis] - second->position[axis];
    }

    double bestOverlap = DBL_MAX;
    double bestAxis[3] = {0.0, 0.0, 1.0};
    bool referenceIsSecond = true;
    int32_t bestFirstEdge = -1;
    int32_t bestSecondEdge = -1;
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
            // Test separation without normalization. Normalize only when an
            // edge axis wins the minimum-penetration comparison; rejecting on
            // edge axes but always resolving on a face gives incorrect torque.
            double reach =
                BoxRadius(first, firstHalf, crossAxis) + BoxRadius(second, secondHalf, crossAxis);
            double distance = Dot3(separation, crossAxis);
            double overlap = reach - AbsoluteDouble(distance);
            if (overlap <= 0.0)
            {
                return false;
            }
            if (overlap * overlap < bestOverlap * bestOverlap * lengthSquared)
            {
                double length = SquareRoot(lengthSquared);
                bestOverlap = overlap / length;
                double sign = distance < 0.0 ? -1.0 : 1.0;
                for (int32_t component = 0; component < 3; ++component)
                {
                    bestAxis[component] = crossAxis[component] * sign / length;
                }
                bestFirstEdge = firstAxis;
                bestSecondEdge = secondAxis;
            }
        }
    }

    if (bestFirstEdge >= 0)
    {
        BuildEdgeContact(first, firstHalf, second, secondHalf, bestFirstEdge, bestSecondEdge,
                         bestAxis, bestOverlap, outManifold);
        return true;
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
    double planeDistance = Dot3(reference->position, referenceNormal) + referenceHalf[normalAxis];

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

// Cell width bounds world-space AABB diameters, including rotated boxes.
static void BodyCell(const RigidBodyCache *cache, double cellSize, int64_t outCell[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double scaled = cache->position[axis] / cellSize;
        // Вне диапазона индекса используется нулевая ячейка: это защита
        // преобразования, не гарантия точных контактов при огромных double.
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
static void PrepareBroadphaseCells(const VoxelRigidBody *bodies, RigidStepScratch *scratch)
{
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
            double radius = AbsoluteDouble(scratch->caches[index].columns[0][axis]) *
                                bodies[index].halfExtent[0] +
                            AbsoluteDouble(scratch->caches[index].columns[1][axis]) *
                                bodies[index].halfExtent[1] +
                            AbsoluteDouble(scratch->caches[index].columns[2][axis]) *
                                bodies[index].halfExtent[2];
            if (radius > largest)
            {
                largest = radius;
            }
        }
    }
    scratch->cellSize = largest > 0.0 ? largest * 2.0 : 1.0;

    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        if (scratch->caches[index].collidable)
        {
            BodyCell(&scratch->caches[index], scratch->cellSize, scratch->caches[index].cell);
        }
    }
}

static bool BuildBroadphase(const VoxelRigidBody *bodies, uint32_t bodyCount,
                            RigidStepScratch *scratch)
{
    PrepareBroadphaseCells(bodies, scratch);
    VoxelRigidBroadphase *spatialIndex = scratch->broadphase;
    if (spatialIndex != NULL)
    {
        for (uint32_t slot = 0u; slot < bodyCount; ++slot)
        {
            const RigidBodyCache *cache = &scratch->caches[slot];
            if (bodies[slot].active && cache->collidable)
            {
                if (!RigidBroadphaseSetProxy(spatialIndex, slot, cache->aabbMin, cache->aabbMax))
                {
                    return false;
                }
            }
            else
            {
                RigidBroadphaseRemoveProxy(spatialIndex, slot);
            }
        }
        for (uint32_t slot = bodyCount; slot < spatialIndex->indexedBodyCount; ++slot)
        {
            RigidBroadphaseRemoveProxy(spatialIndex, slot);
        }
        spatialIndex->indexedBodyCount = bodyCount;
        return true;
    }

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
        uint32_t bucket = CellHash(scratch->caches[index].cell, mask);
        scratch->next[index] = scratch->buckets[bucket];
        scratch->buckets[bucket] = index;
    }
    return true;
}

static bool CandidateAfter(const VoxelRigidBody *bodies, const RigidStepScratch *scratch,
                           uint32_t first, uint32_t second)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        int64_t a = scratch->caches[first].cell[axis];
        int64_t b = scratch->caches[second].cell[axis];
        if (a != b)
            return a > b;
    }
    return bodies[first].stableId > bodies[second].stableId;
}

static void SiftCandidates(const VoxelRigidBody *bodies, RigidStepScratch *scratch, uint32_t root,
                           uint32_t count)
{
    uint32_t value = scratch->next[root];
    while (root < count / 2u)
    {
        uint32_t child = root * 2u + 1u;
        if (child + 1u < count &&
            CandidateAfter(bodies, scratch, scratch->next[child + 1u], scratch->next[child]))
        {
            ++child;
        }
        if (!CandidateAfter(bodies, scratch, scratch->next[child], value))
            break;
        scratch->next[root] = scratch->next[child];
        root = child;
    }
    scratch->next[root] = value;
}

static bool IndexedCandidates(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                              uint32_t first, bool waking, uint32_t *outCount)
{
    const RigidBodyCache *cache = &scratch->caches[first];
    uint32_t count = 0u;
    if (!RigidBroadphaseQuery(scratch->broadphase, cache->aabbMin, cache->aabbMax, scratch->next,
                              scratch->candidateCapacity, &count))
    {
        return false;
    }
    // Keep the legacy lexicographic neighbour-cell/stableId order. Tree shape,
    // allocation history, and body-array permutation must not order impulses.
    uint32_t kept = 0u;
    for (uint32_t candidate = 0u; candidate < count; ++candidate)
    {
        uint32_t slot = scratch->next[candidate];
        if (waking ? !bodies[slot].sleeping
                   : (bodies[slot].sleeping || bodies[slot].stableId <= bodies[first].stableId))
            continue;
        bool neighbour = true;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            int64_t difference = scratch->caches[slot].cell[axis] - cache->cell[axis];
            if (difference < -1 || difference > 1 ||
                cache->aabbMax[axis] <= scratch->caches[slot].aabbMin[axis] ||
                scratch->caches[slot].aabbMax[axis] <= cache->aabbMin[axis])
                neighbour = false;
        }
        if (neighbour)
            scratch->next[kept++] = slot;
    }
    for (uint32_t parent = kept / 2u; parent > 0u; --parent)
    {
        SiftCandidates(bodies, scratch, parent - 1u, kept);
    }
    for (uint32_t remaining = kept; remaining > 1u; --remaining)
    {
        uint32_t temporary = scratch->next[0];
        scratch->next[0] = scratch->next[remaining - 1u];
        scratch->next[remaining - 1u] = temporary;
        SiftCandidates(bodies, scratch, 0u, remaining - 1u);
    }
    *outCount = kept;
    return true;
}

static bool WakeContactPair(VoxelRigidBody *bodies, RigidStepScratch *scratch,
                            const VoxelRigidStepSettings *settings, uint32_t first, uint32_t second,
                            uint32_t *queued)
{
    if (!bodies[second].sleeping)
        return true;
    RigidBodyCache *secondCache = &scratch->caches[second];
    if (scratch->stats->candidatePairCount != UINT32_MAX)
    {
        ++scratch->stats->candidatePairCount;
    }
    BoxManifold manifold;
    if (!BuildBoxManifold(&scratch->caches[first], bodies[first].halfExtent, secondCache,
                          bodies[second].halfExtent, &manifold))
        return true;
    VoxelRigidBodyWake(&bodies[second]);
    ++scratch->stats->awakeBodyCount;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double delta = settings->gravity[axis] * RIGID_STEP_SECONDS;
        if (!AddDoubleToFixed(&bodies[second].linearVelocity[axis], delta))
            return false;
        secondCache->linear[axis] = FixedToDouble(&bodies[second].linearVelocity[axis]);
    }
    scratch->wakeQueue[(*queued)++] = second;
    return true;
}

static uint32_t IslandRoot(RigidStepScratch *scratch, uint32_t index)
{
    while (scratch->islandParents[index] != index)
    {
        scratch->islandParents[index] = scratch->islandParents[scratch->islandParents[index]];
        index = scratch->islandParents[index];
    }
    return index;
}

static void JoinContactIsland(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                              uint32_t first, uint32_t second)
{
    uint32_t firstRoot = IslandRoot(scratch, first);
    uint32_t secondRoot = IslandRoot(scratch, second);
    if (bodies[firstRoot].stableId < bodies[secondRoot].stableId)
    {
        scratch->islandParents[secondRoot] = firstRoot;
    }
    else
    {
        scratch->islandParents[firstRoot] = secondRoot;
    }
}

// A sleeping dynamic body retains its finite mass. Discover the entire
// touching component before collecting constraints: waking during the solve
// would omit contacts of an earlier stableId and omit the body's world support.
static bool WakeContactIslands(VoxelRigidBody *bodies, RigidStepScratch *scratch,
                               const VoxelRigidStepSettings *settings)
{
    uint32_t queued = 0u;
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        scratch->islandParents[index] = index;
        if (!bodies[index].sleeping)
        {
            scratch->wakeQueue[queued++] = index;
        }
    }
    uint32_t mask = scratch->bucketCount - 1u;
    for (uint32_t current = 0u; current < queued && queued < scratch->activeCount; ++current)
    {
        uint32_t first = scratch->wakeQueue[current];
        const RigidBodyCache *firstCache = &scratch->caches[first];
        if (!firstCache->collidable)
        {
            continue;
        }
        if (scratch->broadphase != NULL)
        {
            uint32_t count = 0u;
            if (!IndexedCandidates(bodies, scratch, first, true, &count))
                return false;
            for (uint32_t candidate = 0u; candidate < count; ++candidate)
            {
                if (!WakeContactPair(bodies, scratch, settings, first, scratch->next[candidate],
                                     &queued))
                    return false;
            }
            continue;
        }
        for (int32_t dx = -1; dx <= 1; ++dx)
        {
            for (int32_t dy = -1; dy <= 1; ++dy)
            {
                for (int32_t dz = -1; dz <= 1; ++dz)
                {
                    int64_t neighbour[3] = {firstCache->cell[0] + dx, firstCache->cell[1] + dy,
                                            firstCache->cell[2] + dz};
                    uint32_t bucket = CellHash(neighbour, mask);
                    for (uint32_t second = scratch->buckets[bucket]; second != RIGID_HASH_EMPTY;
                         second = scratch->next[second])
                    {
                        RigidBodyCache *secondCache = &scratch->caches[second];
                        if (!bodies[second].sleeping || secondCache->cell[0] != neighbour[0] ||
                            secondCache->cell[1] != neighbour[1] ||
                            secondCache->cell[2] != neighbour[2])
                        {
                            continue;
                        }
                        if (!WakeContactPair(bodies, scratch, settings, first, second, &queued))
                        {
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

static RigidContact PairContact(uint32_t first, uint32_t second, const BoxManifold *manifold,
                                uint32_t point, double restitution, double friction)
{
    RigidContact contact = {0};
    contact.bodyIndex = first;
    contact.otherIndex = second;
    contact.depth = manifold->depth[point];
    contact.restitution = restitution;
    contact.friction = friction;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        contact.normal[axis] = manifold->normal[axis];
        contact.point[axis] = manifold->point[point][axis];
    }
    return contact;
}

static void AppendPairContacts(VoxelRigidBody *bodies, uint32_t first, uint32_t second,
                               RigidStepScratch *scratch)
{
    if (scratch->stats->candidatePairCount != UINT32_MAX)
    {
        ++scratch->stats->candidatePairCount;
    }
    // Unreached sleeping islands need no narrowphase work. Every sleeper
    // touched by an awake body has already been woken by the frontier pass.
    if (bodies[first].sleeping && bodies[second].sleeping)
    {
        return;
    }

    const RigidBodyCache *firstCache = &scratch->caches[first];
    const RigidBodyCache *secondCache = &scratch->caches[second];

    BoxManifold manifold;
    if (!BuildBoxManifold(firstCache, bodies[first].halfExtent, secondCache,
                          bodies[second].halfExtent, &manifold))
    {
        return;
    }

    JoinContactIsland(bodies, scratch, first, second);
    scratch->caches[first].hasBodyContact = true;
    scratch->caches[second].hasBodyContact = true;

    double restitution = bodies[first].restitution < bodies[second].restitution
                             ? bodies[first].restitution
                             : bodies[second].restitution;
    double friction = bodies[first].friction < bodies[second].friction ? bodies[first].friction
                                                                       : bodies[second].friction;
    for (uint32_t index = 0; index < manifold.count; ++index)
    {
        RigidContact contact = PairContact(first, second, &manifold, index, restitution, friction);
        if (!AppendContact(scratch, &contact))
        {
            return;
        }
    }
}

typedef struct RigidNarrowphaseJob
{
    const VoxelRigidBody *bodies;
    RigidStepScratch *scratch;
    bool writing;
    uint32_t outputEnd;
} RigidNarrowphaseJob;

static void GridNarrowphaseRange(void *context, uint32_t begin, uint32_t end)
{
    RigidNarrowphaseJob *job = (RigidNarrowphaseJob *)context;
    const VoxelRigidBody *bodies = job->bodies;
    RigidStepScratch *scratch = job->scratch;
    VoxelPhysicsConfigureThread();
    uint32_t mask = scratch->bucketCount - 1u;
    for (uint32_t ordered = begin; ordered < end; ++ordered)
    {
        uint32_t first = scratch->order[ordered];
        const RigidBodyCache *firstCache = &scratch->caches[first];
        uint32_t count = 0u;
        uint32_t pairs = 0u;
        uint32_t output = job->writing ? scratch->wakeQueue[first] : 0u;
        uint32_t outputEnd = job->writing && ordered + 1u < scratch->activeCount
                                 ? scratch->wakeQueue[scratch->order[ordered + 1u]]
                                 : job->outputEnd;
        if (!bodies[first].sleeping && firstCache->collidable)
        {
            for (int32_t dx = -1; dx <= 1; ++dx)
            {
                for (int32_t dy = -1; dy <= 1; ++dy)
                {
                    for (int32_t dz = -1; dz <= 1; ++dz)
                    {
                        int64_t neighbour[3] = {firstCache->cell[0] + dx, firstCache->cell[1] + dy,
                                                firstCache->cell[2] + dz};
                        uint32_t bucket = CellHash(neighbour, mask);
                        for (uint32_t second = scratch->buckets[bucket]; second != RIGID_HASH_EMPTY;
                             second = scratch->next[second])
                        {
                            const RigidBodyCache *secondCache = &scratch->caches[second];
                            if (secondCache->cell[0] != neighbour[0] ||
                                secondCache->cell[1] != neighbour[1] ||
                                secondCache->cell[2] != neighbour[2] || bodies[second].sleeping ||
                                bodies[second].stableId <= bodies[first].stableId)
                                continue;
                            ++pairs;
                            BoxManifold manifold;
                            if (!BuildBoxManifold(firstCache, bodies[first].halfExtent, secondCache,
                                                  bodies[second].halfExtent, &manifold))
                                continue;
                            if (job->writing)
                            {
                                if (output > outputEnd || manifold.count > outputEnd - output)
                                {
                                    scratch->caches[first].candidatePairs = 1u;
                                    return;
                                }
                                double restitution =
                                    bodies[first].restitution < bodies[second].restitution
                                        ? bodies[first].restitution
                                        : bodies[second].restitution;
                                double friction = bodies[first].friction < bodies[second].friction
                                                      ? bodies[first].friction
                                                      : bodies[second].friction;
                                for (uint32_t point = 0u; point < manifold.count; ++point)
                                {
                                    scratch->contacts[output++] = PairContact(
                                        first, second, &manifold, point, restitution, friction);
                                }
                            }
                            count += manifold.count;
                        }
                    }
                }
            }
        }
        if (!job->writing)
        {
            // Other jobs read geometry, not these separate metadata fields.
            scratch->wakeQueue[first] = count;
            scratch->caches[first].candidatePairs = pairs;
        }
        else
        {
            // Count/fill disagreement is an error, never an out-of-range write.
            scratch->caches[first].candidatePairs = output == outputEnd ? 0u : 1u;
        }
    }
}

static bool CollectGridContactsParallel(const VoxelRigidBody *bodies, RigidStepScratch *scratch)
{
    RigidNarrowphaseJob job = {bodies, scratch, false, 0u};
    ExecuteRange(scratch, scratch->activeCount, 32u, GridNarrowphaseRange, &job);
    uint32_t worldCount = scratch->contactCount;
    uint32_t total = worldCount;
    // A canonical prefix sum assigns disjoint output ranges. SAT is regenerated
    // in the second pass, avoiding a per-pair temporary allocation and any fixed
    // per-body contact limit (one large box may touch hundreds of small boxes).
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t first = scratch->order[ordered];
        uint32_t count = scratch->wakeQueue[first];
        if (count > scratch->contactCapacity - total)
            return false;
        scratch->wakeQueue[first] = total;
        total += count;
        uint32_t pairs = scratch->caches[first].candidatePairs;
        uint32_t previous = scratch->stats->candidatePairCount;
        scratch->stats->candidatePairCount =
            pairs > UINT32_MAX - previous ? UINT32_MAX : previous + pairs;
    }
    job.writing = true;
    job.outputEnd = total;
    ExecuteRange(scratch, scratch->activeCount, 32u, GridNarrowphaseRange, &job);
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        if (scratch->caches[scratch->order[ordered]].candidatePairs != 0u)
            return false;
    }
    scratch->contactCount = total;
    // Island unions and endpoint contact flags stay ordered and serial. Neither
    // pass reads these flags; callbacks and waking finished before dispatch.
    uint32_t previousFirst = UINT32_MAX;
    uint32_t previousSecond = UINT32_MAX;
    for (uint32_t contact = worldCount; contact < total; ++contact)
    {
        uint32_t first = scratch->contacts[contact].bodyIndex;
        uint32_t second = scratch->contacts[contact].otherIndex;
        if (first != previousFirst || second != previousSecond)
        {
            JoinContactIsland(bodies, scratch, first, second);
            scratch->caches[first].hasBodyContact = true;
            scratch->caches[second].hasBodyContact = true;
            previousFirst = first;
            previousSecond = second;
        }
    }
    return true;
}

static bool CollectBodyContacts(VoxelRigidBody *bodies, uint32_t bodyCount,
                                RigidStepScratch *scratch)
{
    (void)bodyCount;
    if (scratch->broadphase == NULL && scratch->options != NULL &&
        scratch->options->executor != NULL && scratch->activeCount > 64u)
    {
        return CollectGridContactsParallel(bodies, scratch);
    }
    uint32_t mask = scratch->bucketCount - 1u;

    for (uint32_t ordered = 0; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t first = scratch->order[ordered];
        const RigidBodyCache *firstCache = &scratch->caches[first];
        // Unreached sleeping islands remain untouched, but no dynamic body
        // is converted to a static support for an awake contact.
        if (!bodies[first].active || bodies[first].sleeping || !firstCache->collidable)
        {
            continue;
        }
        if (scratch->broadphase != NULL)
        {
            uint32_t count = 0u;
            if (!IndexedCandidates(bodies, scratch, first, false, &count))
                return false;
            for (uint32_t candidate = 0u; candidate < count; ++candidate)
            {
                uint32_t second = scratch->next[candidate];
                if (!bodies[second].sleeping && bodies[second].stableId > bodies[first].stableId)
                {
                    AppendPairContacts(bodies, first, second, scratch);
                }
            }
            continue;
        }
        // StableId filters, rather than cell ordering, select each pair once.
        for (int32_t dx = -1; dx <= 1; ++dx)
        {
            for (int32_t dy = -1; dy <= 1; ++dy)
            {
                for (int32_t dz = -1; dz <= 1; ++dz)
                {
                    int64_t neighbour[3] = {firstCache->cell[0] + dx, firstCache->cell[1] + dy,
                                            firstCache->cell[2] + dz};
                    uint32_t bucket = CellHash(neighbour, mask);
                    for (uint32_t second = scratch->buckets[bucket]; second != RIGID_HASH_EMPTY;
                         second = scratch->next[second])
                    {
                        // Хеш не является координатой: несколько из 27
                        // соседних ключей часто попадают в одну корзину. Без
                        // точной проверки ячейки одна пара добавлялась бы
                        // несколько раз и получала бы лишний импульс.
                        const int64_t *secondCell = scratch->caches[second].cell;
                        if (secondCell[0] != neighbour[0] || secondCell[1] != neighbour[1] ||
                            secondCell[2] != neighbour[2])
                        {
                            continue;
                        }
                        if (second == first)
                        {
                            continue;
                        }
                        if (bodies[second].sleeping ||
                            bodies[second].stableId <= bodies[first].stableId)
                        {
                            continue;
                        }
                        AppendPairContacts(bodies, first, second, scratch);
                    }
                }
            }
        }
    }
    return true;
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

static double PrepareImpulseResponse(const VoxelRigidBody *body, const RigidBodyCache *cache,
                                     const double point[3], const double direction[3],
                                     double angularResponse[3])
{
    double lever[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        lever[axis] = point[axis] - cache->position[axis];
    }
    double torque[3];
    Cross3(lever, direction, torque);
    ApplyInverseInertia(cache, body->inverseInertia, torque, angularResponse);
    return body->inverseMass + Dot3(torque, angularResponse);
}

static void ApplyPreparedImpulse(RigidBodyCache *cache, const RigidConstraintRow *row,
                                 uint32_t endpoint, double magnitude)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        cache->linear[axis] += row->direction[axis] * magnitude * cache->inverseMass;
        cache->angular[axis] += row->angularResponse[endpoint][axis] * magnitude;
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

static void RelativeContactVelocity(const RigidContact *contact, const RigidStepScratch *scratch,
                                    double velocity[3])
{
    ContactVelocity(&scratch->caches[contact->bodyIndex], contact->point, velocity);
    if (contact->otherIndex != UINT32_MAX)
    {
        double otherVelocity[3];
        ContactVelocity(&scratch->caches[contact->otherIndex], contact->point, otherVelocity);
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            velocity[axis] -= otherVelocity[axis];
        }
    }
}

static double TangentCoupling(const RigidBodyCache *cache, const RigidContact *contact,
                              uint32_t endpoint)
{
    double lever[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        lever[axis] = contact->point[axis] - cache->position[axis];
    }
    double torque[3];
    Cross3(lever, contact->rows[1].direction, torque);
    return Dot3(torque, contact->rows[2].angularResponse[endpoint]);
}

static void PrepareContacts(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                            const VoxelRigidStepSettings *settings, uint32_t begin, uint32_t end)
{
    for (uint32_t index = begin; index < end; ++index)
    {
        RigidContact *contact = &scratch->contacts[index];
        const VoxelRigidBody *body = &bodies[contact->bodyIndex];
        const RigidBodyCache *cache = &scratch->caches[contact->bodyIndex];
        bool paired = contact->otherIndex != UINT32_MAX;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            contact->rows[0].direction[axis] = contact->normal[axis];
        }
        BuildTangents(contact->normal, contact->rows[1].direction, contact->rows[2].direction);
        double masses[3];
        for (uint32_t direction = 0; direction < 3u; ++direction)
        {
            RigidConstraintRow *row = &contact->rows[direction];
            double mass = PrepareImpulseResponse(body, cache, contact->point, row->direction,
                                                 row->angularResponse[0]);
            if (paired)
            {
                mass += PrepareImpulseResponse(
                    &bodies[contact->otherIndex], &scratch->caches[contact->otherIndex],
                    contact->point, row->direction, row->angularResponse[1]);
            }
            masses[direction] = mass;
            row->inverseEffectiveMass = mass > 0.0 && IsFiniteDouble(mass) ? 1.0 / mass : 0.0;
        }
        // Solve the two coupled tangential directions as a 2x2 block, then
        // project the TOTAL impulse onto the Coulomb disk (not a per-axis box).
        double coupling = TangentCoupling(cache, contact, 0u);
        if (paired)
        {
            coupling += TangentCoupling(&scratch->caches[contact->otherIndex], contact, 1u);
        }
        double determinant = masses[1] * masses[2] - coupling * coupling;
        contact->tangentCrossMass = 0.0;
        if (determinant > 0.0 && IsFiniteDouble(determinant))
        {
            contact->rows[1].inverseEffectiveMass = masses[2] / determinant;
            contact->rows[2].inverseEffectiveMass = masses[1] / determinant;
            contact->tangentCrossMass = -coupling / determinant;
        }

        double velocity[3];
        RelativeContactVelocity(contact, scratch, velocity);
        double initialNormalSpeed = Dot3(velocity, contact->normal);
        double penetration = contact->depth - settings->penetrationSlop;
        double bias = penetration > 0.0
                          ? settings->penetrationCorrection * penetration / RIGID_STEP_SECONDS
                          : 0.0;
        double bounce = initialNormalSpeed < -RIGID_RESTITUTION_THRESHOLD
                            ? -contact->restitution * initialNormalSpeed
                            : 0.0;
        // Restitution is based on the pre-solve impact speed. Recomputing it
        // each iteration cancels the bounce when the first iteration separates.
        contact->targetNormalSpeed = bias > bounce ? bias : bounce;
    }
}

static void ApplyContactImpulse(RigidStepScratch *scratch, const RigidContact *contact,
                                uint32_t direction, double magnitude)
{
    if (magnitude == 0.0 || !IsFiniteDouble(magnitude))
    {
        return;
    }
    const RigidConstraintRow *row = &contact->rows[direction];
    ApplyPreparedImpulse(&scratch->caches[contact->bodyIndex], row, 0u, magnitude);
    if (contact->otherIndex != UINT32_MAX)
    {
        ApplyPreparedImpulse(&scratch->caches[contact->otherIndex], row, 1u, -magnitude);
    }
}

static void ContactLocalAnchors(const RigidContact *contact, const RigidStepScratch *scratch,
                                double anchors[2][3])
{
    for (uint32_t endpoint = 0u; endpoint < 2u; ++endpoint)
    {
        uint32_t bodyIndex = endpoint == 0u ? contact->bodyIndex : contact->otherIndex;
        if (bodyIndex == UINT32_MAX)
        {
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                anchors[endpoint][axis] = 0.0;
            }
            continue;
        }
        const RigidBodyCache *bodyCache = &scratch->caches[bodyIndex];
        double relative[3];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            relative[axis] = contact->point[axis] - bodyCache->position[axis];
        }
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            anchors[endpoint][axis] = Dot3(relative, bodyCache->columns[axis]);
        }
    }
}

static double CacheAnchorDistance(const double first[3], const double second[3])
{
    double difference[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        difference[axis] = first[axis] - second[axis];
    }
    return Dot3(difference, difference);
}

static uint32_t FindCachedContact(const VoxelRigidBody *bodies, const RigidContact *contact,
                                  const RigidStepScratch *scratch,
                                  const RigidCachedContact *entries, const uint32_t *buckets,
                                  uint32_t bucketMask)
{
    uint64_t firstId = bodies[contact->bodyIndex].stableId;
    uint64_t secondId =
        contact->otherIndex == UINT32_MAX ? 0u : bodies[contact->otherIndex].stableId;
    uint32_t bucket = ContactCacheHash(firstId, secondId, bucketMask);
    double anchors[2][3];
    ContactLocalAnchors(contact, scratch, anchors);
    uint32_t best = RIGID_HASH_EMPTY;
    double bestDistance = DBL_MAX;
    for (uint32_t index = buckets[bucket]; index != RIGID_HASH_EMPTY; index = entries[index].next)
    {
        const RigidCachedContact *entry = &entries[index];
        if (entry->used || entry->stableIds[0] != firstId || entry->stableIds[1] != secondId)
        {
            continue;
        }
        double alignment = 0.0;
        bool finiteImpulse = true;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            alignment += contact->normal[axis] * (double)entry->normal[axis];
            finiteImpulse = finiteImpulse && IsFiniteDouble(entry->worldImpulse[axis]);
        }
        if (!finiteImpulse || !(alignment >= RIGID_CACHE_NORMAL_ALIGNMENT))
        {
            continue;
        }
        double firstDistance = CacheAnchorDistance(anchors[0], entry->localAnchors[0]);
        double secondDistance = CacheAnchorDistance(anchors[1], entry->localAnchors[1]);
        if (!(firstDistance <= RIGID_CACHE_ANCHOR_DISTANCE_SQUARED) ||
            !(secondDistance <= RIGID_CACHE_ANCHOR_DISTANCE_SQUARED))
        {
            continue;
        }
        double distance = firstDistance + secondDistance;
        // Equal distances retain the earlier canonical contact, never a
        // pointer address or array-storage-dependent tie breaker.
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = index;
        }
    }
    return best;
}

static void WarmStartContacts(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                              VoxelRigidContactCache *contactCache)
{
    contactCache->matchedContactCount = 0u;
    RigidCachedContact *entries = ContactCacheEntries(contactCache);
    const uint32_t *buckets = ContactCacheBuckets(contactCache);
    uint32_t bucketMask = BucketCountFor(contactCache->bodyCapacity) - 1u;
    for (uint32_t index = 0u; index < contactCache->contactCount; ++index)
    {
        entries[index].used = false;
    }
    for (uint32_t index = 0u; index < scratch->contactCount; ++index)
    {
        RigidContact *contact = &scratch->contacts[index];
        if (!(contact->rows[0].inverseEffectiveMass > 0.0))
        {
            continue;
        }
        uint32_t match = FindCachedContact(bodies, contact, scratch, entries, buckets, bucketMask);
        if (match == RIGID_HASH_EMPTY)
        {
            continue;
        }
        RigidCachedContact *entry = &entries[match];
        entry->used = true;
        double normalImpulse = Dot3(entry->worldImpulse, contact->normal);
        double firstImpulse = Dot3(entry->worldImpulse, contact->rows[1].direction);
        double secondImpulse = Dot3(entry->worldImpulse, contact->rows[2].direction);
        if (!(normalImpulse > 0.0) || !IsFiniteDouble(normalImpulse) ||
            !IsFiniteDouble(firstImpulse) || !IsFiniteDouble(secondImpulse))
        {
            continue;
        }
        double limit = contact->friction * normalImpulse;
        double largest = AbsoluteDouble(firstImpulse);
        if (AbsoluteDouble(secondImpulse) > largest)
        {
            largest = AbsoluteDouble(secondImpulse);
        }
        if (largest > 0.0 && limit < largest * 2.0)
        {
            double first = firstImpulse / largest;
            double second = secondImpulse / largest;
            double scaledLimit = limit / largest;
            double lengthSquared = first * first + second * second;
            if (lengthSquared > scaledLimit * scaledLimit)
            {
                double scale = scaledLimit / SquareRoot(lengthSquared);
                firstImpulse *= scale;
                secondImpulse *= scale;
            }
        }
        contact->normalImpulse = normalImpulse;
        contact->tangentImpulse[0] = firstImpulse;
        contact->tangentImpulse[1] = secondImpulse;
        ApplyContactImpulse(scratch, contact, 0u, normalImpulse);
        ApplyContactImpulse(scratch, contact, 1u, firstImpulse);
        ApplyContactImpulse(scratch, contact, 2u, secondImpulse);
        ++contactCache->matchedContactCount;
    }
}

static void StoreContactCache(const VoxelRigidBody *bodies, const RigidStepScratch *scratch,
                              VoxelRigidContactCache *contactCache)
{
    uint32_t *buckets = ContactCacheBuckets(contactCache);
    uint32_t bucketCount = BucketCountFor(contactCache->bodyCapacity);
    for (uint32_t index = 0u; index < bucketCount; ++index)
    {
        buckets[index] = RIGID_HASH_EMPTY;
    }
    RigidCachedContact *entries = ContactCacheEntries(contactCache);
    // Reverse insertion gives ascending canonical contact order in each chain.
    // The scratch poses still describe the pre-integration contact geometry.
    for (uint32_t remaining = scratch->contactCount; remaining > 0u; --remaining)
    {
        uint32_t index = remaining - 1u;
        const RigidContact *contact = &scratch->contacts[index];
        RigidCachedContact *entry = &entries[index];
        entry->stableIds[0] = bodies[contact->bodyIndex].stableId;
        entry->stableIds[1] =
            contact->otherIndex == UINT32_MAX ? 0u : bodies[contact->otherIndex].stableId;
        ContactLocalAnchors(contact, scratch, entry->localAnchors);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            entry->normal[axis] = (float)contact->normal[axis];
            entry->worldImpulse[axis] =
                contact->normal[axis] * contact->normalImpulse +
                contact->rows[1].direction[axis] * contact->tangentImpulse[0] +
                contact->rows[2].direction[axis] * contact->tangentImpulse[1];
        }
        uint32_t bucket =
            ContactCacheHash(entry->stableIds[0], entry->stableIds[1], bucketCount - 1u);
        entry->next = buckets[bucket];
        entry->used = false;
        buckets[bucket] = index;
    }
    contactCache->contactCount = scratch->contactCount;
}

static void SolveContact(RigidStepScratch *scratch, uint32_t index)
{
    RigidContact *contact = &scratch->contacts[index];
    if (!(contact->rows[0].inverseEffectiveMass > 0.0))
    {
        return;
    }
    double velocity[3];
    RelativeContactVelocity(contact, scratch, velocity);
    double normalSpeed = Dot3(velocity, contact->normal);
    double magnitude =
        (contact->targetNormalSpeed - normalSpeed) * contact->rows[0].inverseEffectiveMass;
    double previous = contact->normalImpulse;
    double accumulated = previous + magnitude;
    if (accumulated < 0.0)
    {
        accumulated = 0.0;
    }
    contact->normalImpulse = accumulated;
    ApplyContactImpulse(scratch, contact, 0u, accumulated - previous);

    if (!(contact->friction > 0.0))
    {
        return;
    }
    // With a zero normal impulse delta no velocity has changed.
    if (accumulated != previous)
    {
        RelativeContactVelocity(contact, scratch, velocity);
    }
    double firstSpeed = Dot3(velocity, contact->rows[1].direction);
    double secondSpeed = Dot3(velocity, contact->rows[2].direction);
    double firstImpulse = contact->tangentImpulse[0] -
                          contact->rows[1].inverseEffectiveMass * firstSpeed -
                          contact->tangentCrossMass * secondSpeed;
    double secondImpulse = contact->tangentImpulse[1] - contact->tangentCrossMass * firstSpeed -
                           contact->rows[2].inverseEffectiveMass * secondSpeed;
    double limit = contact->friction * contact->normalImpulse;
    double largest = AbsoluteDouble(firstImpulse);
    if (AbsoluteDouble(secondImpulse) > largest)
    {
        largest = AbsoluteDouble(secondImpulse);
    }
    // Scale before squaring: admitted finite masses can produce
    // impulses outside the squareable binary64 range.
    if (largest > 0.0 && limit < largest * 2.0)
    {
        double scaledFirst = firstImpulse / largest;
        double scaledSecond = secondImpulse / largest;
        double scaledLimit = limit / largest;
        double lengthSquared = scaledFirst * scaledFirst + scaledSecond * scaledSecond;
        if (lengthSquared > scaledLimit * scaledLimit)
        {
            double scale = scaledLimit / SquareRoot(lengthSquared);
            firstImpulse *= scale;
            secondImpulse *= scale;
        }
    }
    ApplyContactImpulse(scratch, contact, 1u, firstImpulse - contact->tangentImpulse[0]);
    ApplyContactImpulse(scratch, contact, 2u, secondImpulse - contact->tangentImpulse[1]);
    contact->tangentImpulse[0] = firstImpulse;
    contact->tangentImpulse[1] = secondImpulse;
}

typedef struct RigidJobContext
{
    const VoxelRigidBody *bodies;
    RigidStepScratch *scratch;
    const VoxelRigidStepSettings *settings;
    uint32_t offset;
} RigidJobContext;

static void PrepareContactRange(void *context, uint32_t begin, uint32_t end)
{
    RigidJobContext *job = (RigidJobContext *)context;
    VoxelPhysicsConfigureThread();
    PrepareContacts(job->bodies, job->scratch, job->settings, begin, end);
}

static void BuildCacheRange(void *context, uint32_t begin, uint32_t end)
{
    RigidJobContext *job = (RigidJobContext *)context;
    VoxelPhysicsConfigureThread();
    for (uint32_t ordered = begin; ordered < end; ++ordered)
    {
        uint32_t index = job->scratch->order[ordered];
        BuildCache(&job->bodies[index], &job->scratch->caches[index]);
    }
}

static uint32_t ContactRunEnd(const RigidStepScratch *scratch, uint32_t begin)
{
    const RigidContact *first = &scratch->contacts[begin];
    uint32_t end = begin + 1u;
    while (end < scratch->contactCount && scratch->contacts[end].bodyIndex == first->bodyIndex &&
           scratch->contacts[end].otherIndex == first->otherIndex)
        ++end;
    return end;
}

static void BuildSolverBatches(RigidStepScratch *scratch)
{
    uint32_t counts[RIGID_SOLVER_BATCH_COUNT] = {0};
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        scratch->bodyColors[scratch->order[ordered]] = 0u;
    }
    uint32_t overflowContacts = 0u;
    // Consecutive points of the same manifold stay together and retain their
    // internal order. Coloring individual points would need many more barriers.
    // Greedy choices follow canonical contact order, never worker completion.
    for (uint32_t begin = 0u; begin < scratch->contactCount;)
    {
        const RigidContact *contact = &scratch->contacts[begin];
        uint32_t end = ContactRunEnd(scratch, begin);
        uint64_t used = scratch->bodyColors[contact->bodyIndex];
        if (contact->otherIndex != UINT32_MAX)
            used |= scratch->bodyColors[contact->otherIndex];
        uint32_t color = 0u;
        while (color < RIGID_SOLVER_COLOR_COUNT && (used & (UINT64_C(1) << color)) != 0u)
        {
            ++color;
        }
        if (color < RIGID_SOLVER_COLOR_COUNT)
        {
            uint64_t bit = UINT64_C(1) << color;
            scratch->bodyColors[contact->bodyIndex] |= bit;
            if (contact->otherIndex != UINT32_MAX)
                scratch->bodyColors[contact->otherIndex] |= bit;
        }
        else
        {
            // No constraint is discarded. The overflow batch executes serially
            // after every colored sweep, in the original contact order.
            overflowContacts += end - begin;
        }
        scratch->contactColors[begin] = (uint8_t)color;
        ++counts[color];
        begin = end;
    }
    uint32_t cursors[RIGID_SOLVER_BATCH_COUNT];
    scratch->batchOffsets[0] = 0u;
    uint32_t batches = 0u;
    uint32_t maximum = 0u;
    for (uint32_t color = 0u; color < RIGID_SOLVER_BATCH_COUNT; ++color)
    {
        cursors[color] = scratch->batchOffsets[color];
        scratch->batchOffsets[color + 1u] = scratch->batchOffsets[color] + counts[color];
        if (counts[color] != 0u)
            ++batches;
        if (counts[color] > maximum)
            maximum = counts[color];
    }
    for (uint32_t begin = 0u; begin < scratch->contactCount; begin = ContactRunEnd(scratch, begin))
    {
        uint32_t color = scratch->contactColors[begin];
        scratch->solveOrder[cursors[color]++] = begin;
    }
    if (scratch->options->profile != NULL)
    {
        scratch->options->profile->solverBatchCount = batches;
        scratch->options->profile->solverMaxBatchSize = maximum;
        scratch->options->profile->solverOverflowContacts = overflowContacts;
    }
}

static void SolveBatchRange(void *context, uint32_t begin, uint32_t end)
{
    RigidJobContext *job = (RigidJobContext *)context;
    RigidStepScratch *scratch = job->scratch;
    VoxelPhysicsConfigureThread();
    for (uint32_t run = begin; run < end; ++run)
    {
        uint32_t first = scratch->solveOrder[job->offset + run];
        uint32_t last = ContactRunEnd(scratch, first);
        for (uint32_t index = first; index < last; ++index)
            SolveContact(scratch, index);
    }
}

static void SolveContacts(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                          const VoxelRigidStepSettings *settings,
                          VoxelRigidContactCache *contactCache)
{
    RigidJobContext job = {bodies, scratch, settings, 0u};
    double begin = ProfileNow(scratch);
    ExecuteRange(scratch, scratch->contactCount, 32u, PrepareContactRange, &job);
    ProfileFinish(scratch, VOXEL_RIGID_PROFILE_PREPARE, begin);
    begin = ProfileNow(scratch);
    // Complete ALL restitution targets before warm-start changes velocities.
    // Cache matching/used flags and bucket chains are intentionally serial.
    if (contactCache != NULL)
        WarmStartContacts(bodies, scratch, contactCache);
    ProfileFinish(scratch, VOXEL_RIGID_PROFILE_WARM_START, begin);

    bool colored =
        scratch->options != NULL && scratch->options->solverOrder == VOXEL_RIGID_SOLVER_COLORED;
    begin = ProfileNow(scratch);
    if (colored)
        BuildSolverBatches(scratch);
    ProfileFinish(scratch, VOXEL_RIGID_PROFILE_SCHEDULE, begin);
    begin = ProfileNow(scratch);
    for (uint32_t iteration = 0u; iteration < settings->solverIterations; ++iteration)
    {
        if (!colored)
        {
            for (uint32_t index = 0u; index < scratch->contactCount; ++index)
            {
                SolveContact(scratch, index);
            }
            continue;
        }
        for (uint32_t color = 0u; color < RIGID_SOLVER_BATCH_COUNT; ++color)
        {
            job.offset = scratch->batchOffsets[color];
            uint32_t count = scratch->batchOffsets[color + 1u] - job.offset;
            if (count == 0u)
                continue;
            if (color == RIGID_SOLVER_COLOR_COUNT)
                SolveBatchRange(&job, 0u, count);
            else
                ExecuteRange(scratch, count, 32u, SolveBatchRange, &job);
        }
    }
    ProfileFinish(scratch, VOXEL_RIGID_PROFILE_SOLVE, begin);
}

// Поворот за шаг. Составляющие приводятся к (-pi, pi] точным остатком от
// деления: при обычной скорости это само значение, а при невероятной —
// единственный осмысленный ответ, потому что оборот кратный 2pi неотличим
// от его отсутствия.
static bool AngularStepRadians(const InfiniteCoord *component, double *outRadians)
{
    // The common one-limb case needs only an integer shift and remainder;
    // avoid allocating a temporary bigint for every spinning body.
    int64_t fixed = 0;
    if (TryFixedInt64(component, &fixed))
    {
        const uint64_t fullTurn = 26986075409ull;
        uint64_t magnitude = fixed < 0 ? 0u - (uint64_t)fixed : (uint64_t)fixed;
        int64_t perStep = 0;
        if (!TryShiftRightInt64TowardZero(fixed, VOXEL_RIGID_STEP_SHIFT, &perStep))
        {
            return false;
        }
        uint64_t remainder = magnitude >> VOXEL_RIGID_STEP_SHIFT;
        remainder %= fullTurn;
        if (perStep < 0 && remainder != 0u)
        {
            remainder = fullTurn - remainder;
        }
        double radians = (double)remainder / 4294967296.0;
        const double twoPi = 6.283185307179586;
        if (radians > twoPi * 0.5)
        {
            radians -= twoPi;
        }
        *outRadians = radians;
        return true;
    }

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
            double angularDelta =
                cache->angular[axis] - FixedToDouble(&body->angularVelocity[axis]);
            if (!AddDoubleToFixed(&body->linearVelocity[axis], linearDelta) ||
                !AddDoubleToFixed(&body->angularVelocity[axis], angularDelta))
            {
                return false;
            }
        }

        int64_t fixedVelocity = 0;
        int64_t travel = 0;
        if (TryFixedInt64(&body->linearVelocity[axis], &fixedVelocity) &&
            TryShiftRightInt64TowardZero(fixedVelocity, VOXEL_RIGID_STEP_SHIFT, &travel))
        {
            // The in-place numeric operation preserves the public bigint
            // state and expands to the wide path only at the actual range
            // boundary.
            if (!InfiniteCoordTryAddInt64InPlace(&body->position[axis], travel))
            {
                return false;
            }
        }
        else
        {
            InfiniteCoord shiftedTravel;
            InfiniteCoordInit(&shiftedTravel);
            if (!InfiniteCoordTryCopyShiftRight(&shiftedTravel, &body->linearVelocity[axis],
                                                VOXEL_RIGID_STEP_SHIFT))
            {
                return false;
            }
            InfiniteCoord moved;
            InfiniteCoordInit(&moved);
            bool ok = InfiniteCoordTryAdd(&moved, &body->position[axis], &shiftedTravel);
            InfiniteCoordDestroy(&shiftedTravel);
            if (!ok)
            {
                return false;
            }
            InfiniteCoordDestroy(&body->position[axis]);
            body->position[axis] = moved;
        }
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

typedef struct RigidIntegrationJob
{
    VoxelRigidBody *bodies;
    RigidStepScratch *scratch;
} RigidIntegrationJob;

static void IntegrateRange(void *context, uint32_t begin, uint32_t end)
{
    RigidIntegrationJob *job = (RigidIntegrationJob *)context;
    VoxelPhysicsConfigureThread();
    for (uint32_t ordered = begin; ordered < end; ++ordered)
    {
        uint32_t index = job->scratch->order[ordered];
        bool succeeded = job->bodies[index].sleeping ||
                         IntegrateBody(&job->bodies[index], &job->scratch->caches[index]);
        job->scratch->wakeQueue[index] = succeeded ? 0u : 1u;
    }
}

static void PutBodyToSleep(VoxelRigidBody *body)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        InfiniteCoordDestroy(&body->linearVelocity[axis]);
        InfiniteCoordDestroy(&body->angularVelocity[axis]);
    }
    body->sleepCounter = 0u;
    body->sleeping = true;
}

static void UpdateSleepIslands(VoxelRigidBody *bodies, RigidStepScratch *scratch,
                               const VoxelRigidStepSettings *settings)
{
    const uint32_t quietFlag = 1u;
    const uint32_t supportedFlag = 2u;
    bool sleepEnabled = settings->sleepFrames != 0u && settings->sleepLinearSpeed > 0.0 &&
                        settings->sleepAngularSpeed > 0.0;
    bool hasGravity =
        settings->gravity[0] != 0.0 || settings->gravity[1] != 0.0 || settings->gravity[2] != 0.0;
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        scratch->wakeQueue[scratch->order[ordered]] = quietFlag;
    }
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        VoxelRigidBody *body = &bodies[index];
        if (body->sleeping)
        {
            continue;
        }
        const RigidBodyCache *cache = &scratch->caches[index];
        bool lowSpeed = sleepEnabled;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            if (AbsoluteDouble(cache->linear[axis]) > settings->sleepLinearSpeed ||
                AbsoluteDouble(cache->angular[axis]) > settings->sleepAngularSpeed)
            {
                lowSpeed = false;
            }
        }
        if (!lowSpeed)
        {
            body->sleepCounter = 0u;
        }
        else if (body->sleepCounter < settings->sleepFrames)
        {
            ++body->sleepCounter;
        }
        uint32_t root = IslandRoot(scratch, index);
        if (!sleepEnabled || body->sleepCounter < settings->sleepFrames)
        {
            scratch->wakeQueue[root] &= ~quietFlag;
        }
        if (cache->hasWorldContact || !hasGravity)
        {
            scratch->wakeQueue[root] |= supportedFlag;
        }
    }
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        if (!bodies[index].sleeping &&
            scratch->wakeQueue[IslandRoot(scratch, index)] == (quietFlag | supportedFlag))
        {
            PutBodyToSleep(&bodies[index]);
        }
    }
}

static bool StepOptionsValid(const VoxelRigidStepOptions *options, const VoxelRigidBody *bodies,
                             uint32_t bodyCount, const VoxelCollisionSource *collision,
                             const VoxelRigidStepSettings *settings, void *scratch,
                             uint32_t scratchBytes)
{
    if (options == NULL)
        return true;
    if (options->structSize < sizeof(*options) ||
        (options->solverOrder != VOXEL_RIGID_SOLVER_CANONICAL &&
         options->solverOrder != VOXEL_RIGID_SOLVER_COLORED))
        return false;
    const LaiueTaskExecutor *executor = options->executor;
    VoxelRigidStepProfile *profile = options->profile;
    if (executor != NULL && (executor->structSize < sizeof(*executor) || executor->run == NULL ||
                             executor->context == NULL))
        return false;
    if (profile != NULL && profile->structSize < sizeof(*profile))
        return false;
    const VoxelRigidContactCache *cache = options->contactCache;
    const VoxelRigidBroadphase *index = options->broadphase;
    const void *reserved[] = {bodies,   scratch,
                              settings, collision,
                              cache,    cache != NULL ? cache->storage : NULL,
                              index,    index != NULL ? index->storage : NULL};
    uint64_t lengths[] = {(uint64_t)bodyCount * sizeof(*bodies),
                          scratchBytes,
                          sizeof(*settings),
                          sizeof(*collision),
                          cache != NULL ? sizeof(*cache) : 0u,
                          cache != NULL ? cache->storageBytes : 0u,
                          index != NULL ? sizeof(*index) : 0u,
                          index != NULL ? index->storageBytes : 0u};
    const void *descriptors[] = {options, executor, profile};
    uint64_t sizes[] = {options->structSize, executor != NULL ? executor->structSize : 0u,
                        profile != NULL ? profile->structSize : 0u};
    for (uint32_t descriptor = 0u; descriptor < 3u; ++descriptor)
    {
        if (sizes[descriptor] > UINTPTR_MAX - (uintptr_t)descriptors[descriptor])
            return false;
        for (uint32_t range = 0u; range < sizeof(reserved) / sizeof(reserved[0]); ++range)
        {
            if (MemoryRangesOverlap(descriptors[descriptor], sizes[descriptor], reserved[range],
                                    lengths[range]))
                return false;
        }
        for (uint32_t other = 0u; other < descriptor; ++other)
        {
            if (MemoryRangesOverlap(descriptors[descriptor], sizes[descriptor], descriptors[other],
                                    sizes[other]))
                return false;
        }
    }
    return true;
}

static bool RigidBodyStepInternal(VoxelRigidBody *bodies, uint32_t bodyCount,
                                  const VoxelCollisionSource *collision,
                                  const VoxelRigidStepSettings *settings, void *scratch,
                                  uint32_t scratchBytes, VoxelRigidContactCache *contactCache,
                                  VoxelRigidBroadphase *broadphase,
                                  const VoxelRigidStepOptions *options)
{
    if (bodies == NULL || settings == NULL || scratch == NULL || bodyCount == 0u ||
        bodyCount > VOXEL_RIGID_MAX_BODIES || collision == NULL ||
        collision->queryBlockPhysics == NULL)
    {
        return false;
    }
    if (!(settings->penetrationCorrection >= 0.0 && settings->penetrationCorrection <= 1.0) ||
        !(settings->penetrationSlop >= 0.0) || !IsFiniteDouble(settings->penetrationSlop) ||
        settings->solverIterations == 0u || !IsFiniteDouble(settings->sleepLinearSpeed) ||
        !IsFiniteDouble(settings->sleepAngularSpeed) || settings->sleepLinearSpeed < 0.0 ||
        settings->sleepAngularSpeed < 0.0)
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
    if (!StepOptionsValid(options, bodies, bodyCount, collision, settings, scratch, scratchBytes))
        return false;
    if (contactCache != NULL)
    {
        uint64_t bodyBytes = (uint64_t)bodyCount * sizeof(*bodies);
        if (!ContactCacheValid(contactCache, bodyCount) ||
            MemoryRangesOverlap(contactCache->storage, contactCache->storageBytes, scratch,
                                scratchBytes) ||
            MemoryRangesOverlap(contactCache->storage, contactCache->storageBytes, bodies,
                                bodyBytes) ||
            MemoryRangesOverlap(contactCache->storage, contactCache->storageBytes, contactCache,
                                sizeof(*contactCache)) ||
            MemoryRangesOverlap(contactCache, sizeof(*contactCache), scratch, scratchBytes) ||
            MemoryRangesOverlap(contactCache, sizeof(*contactCache), bodies, bodyBytes))
        {
            return false;
        }
    }

    if (broadphase != NULL)
    {
        if (!RigidBroadphaseValid(broadphase, bodyCount) ||
            MemoryRangesOverlap(broadphase, sizeof(*broadphase), broadphase->storage,
                                broadphase->storageBytes))
            return false;
        const void *reserved[] = {
            bodies,    scratch,      settings,
            collision, contactCache, contactCache != NULL ? contactCache->storage : NULL};
        uint64_t lengths[] = {(uint64_t)bodyCount * sizeof(*bodies),
                              scratchBytes,
                              sizeof(*settings),
                              sizeof(*collision),
                              contactCache != NULL ? sizeof(*contactCache) : 0u,
                              contactCache != NULL ? contactCache->storageBytes : 0u};
        for (uint32_t range = 0u; range < sizeof(reserved) / sizeof(reserved[0]); ++range)
        {
            if (MemoryRangesOverlap(broadphase->storage, broadphase->storageBytes, reserved[range],
                                    lengths[range]) ||
                MemoryRangesOverlap(broadphase, sizeof(*broadphase), reserved[range],
                                    lengths[range]))
                return false;
        }
        broadphase->updatedProxyCount = 0u;
        broadphase->visitedNodeCount = 0u;
    }
    if (contactCache != NULL)
        contactCache->matchedContactCount = 0u;
    if (options != NULL && options->profile != NULL)
    {
        VoxelRigidStepProfile *profile = options->profile;
        for (uint32_t stage = 0u; stage < VOXEL_RIGID_PROFILE_STAGE_COUNT; ++stage)
        {
            profile->seconds[stage] = 0.0;
        }
        profile->solverBatchCount = 0u;
        profile->solverMaxBatchSize = 0u;
        profile->solverOverflowContacts = 0u;
    }

    VoxelPhysicsConfigureThread();
    VoxelRigidStepStats *stepStats = StepStatsPointer(scratch, bodyCount);
    stepStats->activeBodyCount = 0u;
    stepStats->awakeBodyCount = 0u;
    stepStats->candidatePairCount = 0u;
    stepStats->contactCount = 0u;

    // A fully sleeping scene skips sorting, cache construction and collision
    // queries. The scan below is still O(bodyCount), not a constant-time active
    // set. Callers that mutate the world under sleeping bodies must call
    // VoxelRigidBodyWake for the affected bodies.
    bool hasAwakeBody = false;
    for (uint32_t index = 0u; index < bodyCount; ++index)
    {
        if (!bodies[index].active)
        {
            continue;
        }
        ++stepStats->activeBodyCount;
        if (!bodies[index].sleeping)
        {
            hasAwakeBody = true;
            ++stepStats->awakeBodyCount;
        }
    }
    if (!hasAwakeBody)
    {
        return true;
    }

    RigidStepScratch state;
    uintptr_t padding = (0u - (uintptr_t)scratch) & 63u;
    uint8_t *cursor = (uint8_t *)scratch + padding;
    state.caches = (RigidBodyCache *)cursor;
    cursor += (size_t)bodyCount * sizeof(RigidBodyCache);
    state.contacts = (RigidContact *)cursor;
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidContact);
    state.next = (uint32_t *)cursor;
    cursor += (size_t)bodyCount * sizeof(uint32_t);
    state.order = (uint32_t *)cursor;
    cursor += (size_t)bodyCount * sizeof(uint32_t);
    state.wakeQueue = (uint32_t *)cursor;
    cursor += (size_t)bodyCount * sizeof(uint32_t);
    state.islandParents = (uint32_t *)cursor;
    cursor += (size_t)bodyCount * sizeof(uint32_t);
    state.bodyColors = (uint64_t *)cursor;
    cursor += (size_t)bodyCount * sizeof(uint64_t);
    state.contactColors = cursor;
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(uint8_t);
    state.solveOrder = (uint32_t *)cursor;
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(uint32_t);
    state.buckets = (uint32_t *)cursor;
    state.bucketCount = BucketCountFor(bodyCount);
    state.candidateCapacity = bodyCount;
    state.broadphase = broadphase;
    state.cellSize = 1.0;
    state.contactCount = 0u;
    state.contactCapacity = bodyCount * RIGID_CONTACTS_PER_BODY;
    state.contactOverflow = false;
    state.stats = stepStats;
    state.options = options;
    double stageBegin = ProfileNow(&state);
    if (!BuildStableOrder(bodies, bodyCount, &state))
    {
        return false;
    }
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_ORDER, stageBegin);
    stageBegin = ProfileNow(&state);

    // Гравитация до построения контактов: решатель обязан видеть скорость,
    // с которой тело действительно подходит к опоре, иначе оно продавливает
    // её на величину шага и всплывает обратно на следующем.
    for (uint32_t ordered = 0; ordered < state.activeCount; ++ordered)
    {
        uint32_t index = state.order[ordered];
        if (bodies[index].sleeping)
        {
            continue;
        }
        double delta[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            delta[axis] = settings->gravity[axis] * RIGID_STEP_SECONDS;
            // Gravity is an internal force, not an external wake event.  Do
            // not reset the sleep counter on every tick while contact forces
            // are balancing it.
            if (!AddDoubleToFixed(&bodies[index].linearVelocity[axis], delta[axis]))
            {
                return false;
            }
        }
    }

    ProfileFinish(&state, VOXEL_RIGID_PROFILE_FORCES, stageBegin);
    stageBegin = ProfileNow(&state);
    RigidJobContext cacheJob = {bodies, &state, settings, 0u};
    ExecuteRange(&state, state.activeCount, 64u, BuildCacheRange, &cacheJob);
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_BOUNDS, stageBegin);
    stageBegin = ProfileNow(&state);
    if (!BuildBroadphase(bodies, bodyCount, &state))
        return false;
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_BROADPHASE, stageBegin);
    stageBegin = ProfileNow(&state);
    if (!WakeContactIslands(bodies, &state, settings))
        return false;
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_WAKE, stageBegin);
    stageBegin = ProfileNow(&state);
    CollectWorldContacts(bodies, bodyCount, collision, &state);
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_WORLD_CONTACTS, stageBegin);
    stageBegin = ProfileNow(&state);
    if (!CollectBodyContacts(bodies, bodyCount, &state))
        return false;
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_BODY_CONTACTS, stageBegin);
    state.stats->contactCount = state.contactCount;
    if (state.contactOverflow)
    {
        return false;
    }
    SolveContacts(bodies, &state, settings, contactCache);
    stageBegin = ProfileNow(&state);
    RigidIntegrationJob integrationJob = {bodies, &state};
    ExecuteRange(&state, state.activeCount, 64u, IntegrateRange, &integrationJob);
    for (uint32_t ordered = 0u; ordered < state.activeCount; ++ordered)
    {
        uint32_t index = state.order[ordered];
        if (state.wakeQueue[index] != 0u)
            return false;
    }
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_INTEGRATE, stageBegin);
    stageBegin = ProfileNow(&state);
    UpdateSleepIslands(bodies, &state, settings);
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_SLEEP, stageBegin);
    stageBegin = ProfileNow(&state);
    if (contactCache != NULL)
    {
        StoreContactCache(bodies, &state, contactCache);
    }
    ProfileFinish(&state, VOXEL_RIGID_PROFILE_STORE, stageBegin);
    return true;
}

bool VoxelRigidBodyStep(VoxelRigidBody *bodies, uint32_t bodyCount,
                        const VoxelCollisionSource *collision,
                        const VoxelRigidStepSettings *settings, void *scratch,
                        uint32_t scratchBytes)
{
    return RigidBodyStepInternal(bodies, bodyCount, collision, settings, scratch, scratchBytes,
                                 NULL, NULL, NULL);
}

bool VoxelRigidBodyStepCached(VoxelRigidBody *bodies, uint32_t bodyCount,
                              const VoxelCollisionSource *collision,
                              const VoxelRigidStepSettings *settings, void *scratch,
                              uint32_t scratchBytes, VoxelRigidContactCache *contactCache)
{
    if (contactCache == NULL)
    {
        return false;
    }
    return RigidBodyStepInternal(bodies, bodyCount, collision, settings, scratch, scratchBytes,
                                 contactCache, NULL, NULL);
}

bool VoxelRigidBodyStepIndexed(VoxelRigidBody *bodies, uint32_t bodyCount,
                               const VoxelCollisionSource *collision,
                               const VoxelRigidStepSettings *settings, void *scratch,
                               uint32_t scratchBytes, VoxelRigidContactCache *contactCache,
                               VoxelRigidBroadphase *broadphase)
{
    if (broadphase == NULL)
        return false;
    return RigidBodyStepInternal(bodies, bodyCount, collision, settings, scratch, scratchBytes,
                                 contactCache, broadphase, NULL);
}

bool VoxelRigidBodyStepEx(VoxelRigidBody *bodies, uint32_t bodyCount,
                          const VoxelCollisionSource *collision,
                          const VoxelRigidStepSettings *settings, void *scratch,
                          uint32_t scratchBytes, const VoxelRigidStepOptions *options)
{
    if (options == NULL || options->structSize < sizeof(*options))
        return false;
    return RigidBodyStepInternal(bodies, bodyCount, collision, settings, scratch, scratchBytes,
                                 options->contactCache, options->broadphase, options);
}
