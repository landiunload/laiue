#include "physics/rigid_body.h"

#include "math/scalar.h"
#include "physics/fp_environment.h"

#include <float.h>
#include <string.h>

// Парный решатель: два независимых контакта одной полосы раскраски решаются
// одной 128-битной командой. SSE2 и NEON выполняют точные IEEE-операции над
// binary64, поэтому полоса повторяет скалярную последовательность шаг в шаг.
// FMA нигде не используется: слияние умножения-сложения меняло бы биты.
#if defined(_M_ARM64) || defined(__aarch64__)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#define LAIUE_RIGID_PAIRED_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) ||           \
    defined(__i386__)
#include <emmintrin.h>
#define LAIUE_RIGID_PAIRED_SSE2 1
#endif

// Скорость сближения, ниже которой отскок не применяется. Без порога тело
// на опоре вечно подпрыгивает на численном шуме.
#define RIGID_RESTITUTION_THRESHOLD 1.0
// Потолок скорости, с которой решатель выдавливает тело из проникновения.
// Без него глубоко утопленное тело получает скорость в десятки блоков в
// секунду и выстреливает из кучи: доля проникновения, делённая на шаг, тем
// больше, чем глубже тело сидит. Выталкивание обязано быть настойчивым, а
// не взрывным — блока за треть секунды хватает.
#define RIGID_MAX_RECOVERY_SPEED 3.0
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
    // Произвольный путь ниже в конечном счёте прибавляет ровно
    // trunc(delta * 2^32), если это целое помещается в int64: там целая и
    // дробная половины сдвигаются и усекаются по отдельности, а усечение
    // линейно по целой части. Дельта скорости, пришедшая от решателя, почти
    // никогда не кратна 2^-32, и без этого пути на каждую ось и тело каждый
    // тик заводятся и уничтожаются несколько bigint-объектов.
    double scaled = delta * 4294967296.0;
    if (scaled >= -9223372036854775808.0 && scaled < 9223372036854775808.0)
    {
        return InfiniteCoordTryAddInt64InPlace(value, (int64_t)scaled);
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

#define RIGID_REBASE_MAX_BLOCKS INT64_C(2147483647)

static bool RigidRebaseAddend(int64_t blockShift, int64_t *outAddend)
{
    if (blockShift > RIGID_REBASE_MAX_BLOCKS || blockShift < -RIGID_REBASE_MAX_BLOCKS)
    {
        return false;
    }
    *outAddend = -(blockShift * ((int64_t)1 << VOXEL_RIGID_VELOCITY_SHIFT));
    return true;
}

// Ляжет ли добавка на место, ничего не выделив. Выделение внутри числа
// произвольной точности нужно ровно в двух случаях: под значение ещё нет
// лимба, либо одинаковые знаки переносят разряд за старший лимб. Первое
// видно по знаку. Второе исключает свободный старший бит старшего лимба:
// тогда модуль меньше 2^(64k-1), добавка меньше 2^63, и сумма заведомо
// остаётся в прежнем числе лимбов. Разные знаки модуль только уменьшают,
// там выделять нечего и подавно.
//
// Смысл проверки не в скорости, а в отказоустойчивости: операция, которая
// ничего не выделяет, не может и отказать, поэтому три такие добавки подряд
// безопасны — частично применённого сдвига не получится.
static bool RigidRebaseFitsInPlace(const InfiniteCoord *value, int64_t addend)
{
    if (addend == 0)
    {
        return true;
    }
    if (value->sign == 0 || value->limbCount == 0u || value->limbs == NULL)
    {
        return false;
    }
    // Модуль считается через unsigned, иначе INT64_MIN дал бы переполнение.
    uint64_t magnitude = addend < 0 ? (uint64_t)(-(addend + 1)) + 1u : (uint64_t)addend;
    if (magnitude >= (UINT64_C(1) << 63))
    {
        return false;
    }
    if (value->sign != (addend < 0 ? -1 : 1))
    {
        return true;
    }
    return value->limbs[value->limbCount - 1u] < (UINT64_C(1) << 63);
}

bool VoxelRigidBodyTranslateBlocks(VoxelRigidBody *body, const int64_t blockShift[3])
{
    if (body == NULL || blockShift == NULL)
    {
        return false;
    }

    // Быстрый путь берётся только если он годится для всех трёх осей сразу.
    // Иначе часть осей уже сдвинулась бы, а часть нет.
    int64_t addends[3] = {0, 0, 0};
    bool inPlace = true;
    for (int32_t axis = 0; axis < 3 && inPlace; ++axis)
    {
        if (blockShift[axis] == 0)
        {
            continue;
        }
        inPlace = RigidRebaseAddend(blockShift[axis], &addends[axis]) &&
                  RigidRebaseFitsInPlace(&body->position[axis], addends[axis]);
    }
    if (inPlace)
    {
        bool ok = true;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            if (addends[axis] == 0)
            {
                continue;
            }
            // Проверка выше исключила оба места, где эта операция способна
            // выделить память, а других причин отказать у неё нет. Возврат
            // всё равно проверяется: молча продолжать по предположению хуже,
            // чем сообщить об отказе. Прогон с отказывающим allocator это
            // утверждение подтверждает — быстрый путь проходит целиком.
            ok = InfiniteCoordTryAddInt64InPlace(&body->position[axis], addends[axis]) && ok;
        }
        return ok;
    }

    // Общий путь: результат каждой оси собирается отдельно и публикуется
    // только после того, как посчитаны все три.
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
        int64_t addend = 0;
        if (RigidRebaseAddend(blockShift[axis], &addend))
        {
            // Сдвиг умещается в int64 — хватает одной копии со сложением
            // вместо четырёх промежуточных чисел.
            if (!InfiniteCoordTryCopyAddInt64(&moved[axis], &body->position[axis], addend))
            {
                goto failure;
            }
            continue;
        }

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

// Раскладка не случайна. Решатель за шаг трогает только скорости и
// обратную массу, зато трогает их десятки тысяч раз в произвольном порядке.
// Поэтому они лежат в начале и выровнены на кэш-линию: одно тело — одна
// линия вместо двух-трёх, разбросанных по структуре. Всё остальное нужно
// только на подготовке и интегрировании, где обход идёт подряд.
typedef struct RigidBodyCache
{
    _Alignas(64) double linear[3];
    double angular[3];
    // Mass is constant during this tick. Keep it beside the solver velocities
    // so applying impulses does not revisit the larger public body array.
    double inverseMass;
    double position[3];
    // Столбцы матрицы поворота и мировая диагональ обратной инерции.
    double columns[3][3];
    double aabbMin[3];
    double aabbMax[3];
    // Наибольший по осям радиус AABB. Заполняется на bounds и переиспользуется
    // широким отбором: читать его заново по столбцам поворота незачем.
    double radius;
    int64_t cell[3];
    bool hasWorldContact;
    bool hasBodyContact;
    bool collidable;
    // Per-first-body narrowphase count; occupies former tail padding.
    uint32_t candidatePairs;
} RigidBodyCache;

_Static_assert(sizeof(RigidBodyCache) == 256u, "body cache keeps its one-line-per-body layout");

// Geometry and inertia do not change during velocity iterations. Prepare the
// three constraint directions once instead of rotating inertia tensors again
// for every impulse on every iteration.
typedef struct RigidConstraintRow
{
    double direction[3];
    double angularResponse[2][3];
    double inverseEffectiveMass;
} RigidConstraintRow;

// Холодная часть контакта: то, что нужно только узкой фазе, подготовке и
// кэшу. Решатель за шаг обходит контакты восемь раз и к этим полям не
// обращается; держать их в одной структуре с горячими значит втаскивать
// лишнюю кэш-линию на каждый контакт каждую итерацию.
typedef struct RigidContact
{
    uint32_t bodyIndex;
    // UINT32_MAX означает неподвижный мир.
    uint32_t otherIndex;
    double point[3];
    double normal[3];
    double depth;
    double restitution;
    double friction;
} RigidContact;

// Горячая часть: ровно рабочее множество одной итерации решателя. normal
// здесь не хранится: после подготовки его побитово дублирует
// rows[0].direction, и решатель читает направление нормали оттуда.
typedef struct RigidSolverContact
{
    uint32_t bodyIndex;
    uint32_t otherIndex;
    // Плечи от центров масс до точки контакта. Считаются один раз на
    // подготовке: решатель обращается к ним восемь раз за шаг, и разность
    // point - position каждый раз тянула бы позицию тела из другой кэш-линии.
    double lever[2][3];
    double normalImpulse;
    double friction;
    RigidConstraintRow rows[3];
    double targetNormalSpeed;
    double tangentImpulse[2];
    double tangentCrossMass;
} RigidSolverContact;

_Static_assert(sizeof(RigidContact) == 80u, "cold contact geometry stays compact");
_Static_assert(sizeof(RigidSolverContact) == 344u,
               "solver contact is the measured per-iteration working set");
_Static_assert(sizeof(RigidSolverContact) % 8u == 0u,
               "solver contact stays a plain array element");

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

// Граница между вставками и пирамидальной сортировкой для кандидатов
// одного тела. Подобрана замером, а не рассуждением.
#define RIGID_CANDIDATE_INSERTION_MAX 16u

#define RIGID_SOLVER_COLOR_COUNT 64u
// Ниже этого числа прогонов цвет решается вызывающим потоком: диспетчер
// пула на короткой группе дороже самой группы. Порядок решения не меняется.
#define RIGID_SOLVER_PARALLEL_GRAIN 16u
#define RIGID_SOLVER_BATCH_COUNT (RIGID_SOLVER_COLOR_COUNT + 1u)

// Cache only geometry during the count pass. This is not a contact limit:
// bodies exceeding the small local cache regenerate their complete manifold
// list in the fill pass. No persistent/replay state is introduced.
#define RIGID_NARROWPHASE_POINTS_PER_BODY 8u
typedef struct RigidNarrowphasePoint
{
    uint32_t otherIndex;
    double point[3];
    double normal[3];
    double depth;
} RigidNarrowphasePoint;

_Static_assert(sizeof(RigidNarrowphasePoint) == 64u,
               "cached narrowphase geometry stays one line per point");

// Hash-chain rejection needs only coordinates and ordering, not the large
// solver cache or public body. Keep those reads together in a compact stream.
typedef struct RigidGridEntry
{
    int64_t cell[3];
    uint32_t next;
    uint32_t stableOrder;
} RigidGridEntry;

_Static_assert(sizeof(RigidGridEntry) == 32u, "Grid rejection metadata stays compact");

typedef struct RigidStepScratch
{
    RigidBodyCache *caches;
    RigidGridEntry *grid;
    RigidNarrowphasePoint *narrowphasePoints;
    RigidContact *contacts;
    // Горячее зеркало контактов: решатель ходит только сюда, холодные
    // геометрия и параметры остаются в contacts.
    RigidSolverContact *solverContacts;
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
    // Посещения узлов дерева, накопленные за тело. Запрос считает их в своём
    // описателе, а описатель у параллельного обхода свой на каждый диапазон:
    // иначе несколько потоков писали бы в один счётчик. Сумма собирается
    // последовательно, в каноническом порядке, поэтому число не плавает.
    uint32_t *narrowVisits;
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
    uint64_t grid = (uint64_t)bodyCount * sizeof(RigidGridEntry);
    uint64_t geometry =
        (uint64_t)bodyCount * RIGID_NARROWPHASE_POINTS_PER_BODY * sizeof(RigidNarrowphasePoint);
    uint64_t contacts = (uint64_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidContact);
    uint64_t solverContacts =
        (uint64_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidSolverContact);
    uint64_t links = (uint64_t)bodyCount * sizeof(uint32_t) * 5u;
    uint64_t schedule =
        (uint64_t)bodyCount *
        (sizeof(uint64_t) + RIGID_CONTACTS_PER_BODY * (sizeof(uint8_t) + sizeof(uint32_t)));
    uint64_t buckets = (uint64_t)BucketCountFor(bodyCount) * sizeof(uint32_t);
    uint64_t total = caches + grid + geometry + contacts + solverContacts + links + schedule +
                     buckets + sizeof(VoxelRigidStepStats) + 64u;
    return total > 0xFFFFFFFFull ? 0u : (uint32_t)total;
}

static VoxelRigidStepStats *StepStatsPointer(void *scratch, uint32_t bodyCount)
{
    uintptr_t padding = (0u - (uintptr_t)scratch) & 63u;
    uint8_t *cursor = (uint8_t *)scratch + padding;
    cursor += (size_t)bodyCount * sizeof(RigidBodyCache);
    cursor += (size_t)bodyCount * sizeof(RigidGridEntry);
    cursor += (size_t)bodyCount * RIGID_NARROWPHASE_POINTS_PER_BODY * sizeof(RigidNarrowphasePoint);
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidContact);
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidSolverContact);
    cursor += (size_t)bodyCount * sizeof(uint32_t) * 5u;
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
    double largestRadius = 0.0;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double radius = AbsoluteDouble(cache->columns[0][axis]) * body->halfExtent[0] +
                        AbsoluteDouble(cache->columns[1][axis]) * body->halfExtent[1] +
                        AbsoluteDouble(cache->columns[2][axis]) * body->halfExtent[2];
        cache->aabbMin[axis] = cache->position[axis] - radius;
        cache->aabbMax[axis] = cache->position[axis] + radius;
        if (radius > largestRadius)
        {
            largestRadius = radius;
        }
    }
    cache->radius = largestRadius;

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

    // Матрица взаимной ориентации: rotation[i][j] — косинус угла между осью i
    // первого тела и осью j второго. Все пятнадцать разделяющих осей
    // выражаются через неё и через проекции расстояния на оси тел, поэтому ни
    // одна из них не требует больше ни векторного произведения, ни трёх
    // скалярных: вместо примерно девятисот умножений на пару выходит около
    // двухсот. Классический приём из Real-Time Collision Detection (Ericson,
    // 4.4.1); ту же матрицу считают Bullet, Box2D и Jolt.
    double rotation[3][3];
    double absolute[3][3];
    double firstDistance[3];
    double secondDistance[3];
    // Кольцевые соседи оси. Остаток по модулю компилятор не сводит к
    // безусловному выбору в этих циклах, и на каждой рёберной оси остаётся
    // умножать на магическую константу; таблица убирает это, не трогая
    // порядок перебора осей.
    static const int32_t axisNext[3] = {1, 2, 0};
    static const int32_t axisLast[3] = {2, 0, 1};
    for (int32_t i = 0; i < 3; ++i)
    {
        for (int32_t j = 0; j < 3; ++j)
        {
            rotation[i][j] = Dot3(first->columns[i], second->columns[j]);
            absolute[i][j] = AbsoluteDouble(rotation[i][j]);
        }
        firstDistance[i] = Dot3(separation, first->columns[i]);
        secondDistance[i] = Dot3(separation, second->columns[i]);
    }

    double bestOverlap = DBL_MAX;
    double bestAxis[3] = {0.0, 0.0, 1.0};
    bool referenceIsSecond = true;
    int32_t bestFirstEdge = -1;
    int32_t bestSecondEdge = -1;
    for (int32_t which = 0; which < 6; ++which)
    {
        int32_t own = which < 3 ? which : which - 3;
        // Полупротяжённость вдоль собственной оси — само полуребро; вдоль
        // чужой она набирается из трёх косинусов.
        double reach;
        double distance;
        if (which < 3)
        {
            reach = firstHalf[own] + secondHalf[0] * absolute[own][0] +
                    secondHalf[1] * absolute[own][1] + secondHalf[2] * absolute[own][2];
            distance = firstDistance[own];
        }
        else
        {
            reach = secondHalf[own] + firstHalf[0] * absolute[0][own] +
                    firstHalf[1] * absolute[1][own] + firstHalf[2] * absolute[2][own];
            distance = secondDistance[own];
        }
        double overlap = reach - AbsoluteDouble(distance);
        if (overlap <= 0.0)
        {
            return false;
        }
        if (overlap < bestOverlap)
        {
            bestOverlap = overlap;
            // Нормаль всегда смотрит из второго тела в первое.
            const double *axis = which < 3 ? first->columns[own] : second->columns[own];
            double sign = distance < 0.0 ? -1.0 : 1.0;
            for (int32_t component = 0; component < 3; ++component)
            {
                bestAxis[component] = axis[component] * sign;
            }
            referenceIsSecond = which >= 3;
        }
    }

    // Полный SAT также обязан проверять пары рёбер. У почти параллельных
    // рёбер cross-ось вырождается и не является разделяющим направлением;
    // её длина здесь равна синусу угла между осями.
    // Квадрат лучшего перекрытия ведётся рядом с самим перекрытием: сравнение
    // нормируется через него, и без этого произведение лучшего значения на
    // себя считалось бы на каждой оси заново.
    double bestOverlapSquared = bestOverlap * bestOverlap;
    for (int32_t firstAxis = 0; firstAxis < 3; ++firstAxis)
    {
        int32_t firstNext = axisNext[firstAxis];
        int32_t firstLast = axisLast[firstAxis];
        for (int32_t secondAxis = 0; secondAxis < 3; ++secondAxis)
        {
            double lengthSquared =
                1.0 - rotation[firstAxis][secondAxis] * rotation[firstAxis][secondAxis];
            if (!(lengthSquared > 1.0e-12))
            {
                continue;
            }
            int32_t secondNext = axisNext[secondAxis];
            int32_t secondLast = axisLast[secondAxis];
            // Test separation without normalization. Normalize only when an
            // edge axis wins the minimum-penetration comparison; rejecting on
            // edge axes but always resolving on a face gives incorrect torque.
            double reach = firstHalf[firstNext] * absolute[firstLast][secondAxis] +
                           firstHalf[firstLast] * absolute[firstNext][secondAxis] +
                           secondHalf[secondNext] * absolute[firstAxis][secondLast] +
                           secondHalf[secondLast] * absolute[firstAxis][secondNext];
            double distance = firstDistance[firstLast] * rotation[firstNext][secondAxis] -
                              firstDistance[firstNext] * rotation[firstLast][secondAxis];
            double overlap = reach - AbsoluteDouble(distance);
            if (overlap <= 0.0)
            {
                return false;
            }
            if (overlap * overlap < bestOverlapSquared * lengthSquared)
            {
                double crossAxis[3];
                Cross3(first->columns[firstAxis], second->columns[secondAxis], crossAxis);
                double length = SquareRoot(lengthSquared);
                bestOverlap = overlap / length;
                bestOverlapSquared = bestOverlap * bestOverlap;
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

// === Контакты с миром ===
//
// Проверка «угол тела внутри блока» пропускает целые классы касаний.
// Повёрнутый на 45 градусов куб опирается на столб рёбрами: ни один его
// угол внутрь блока не попадает, и тело проваливается сквозь опору. И даже
// когда угол внутри, выталкивать его по ближайшей грани того самого блока
// нельзя — у пола в один слой открыта и нижняя грань, ближайшей она
// оказывается уже на половине погружения, и тело уезжает вниз сквозь пол.
//
// Поэтому окрестность тела собирается заново: сплошные клетки под его AABB
// склеиваются жадным разбиением в крупные коробки, грани, упирающиеся в
// соседнюю сплошную клетку, выбрасываются, и с каждой коробкой строится
// обычный граневой manifold — тот же, что и между двумя телами. Склейка
// заодно убирает внутренние рёбра: у слитой плиты пола боковых граней
// просто нет, и тело не цепляется за стыки блоков.

// Ядро выборки на один проход и её полный размер вместе с гало в одну
// клетку: гало нужно только чтобы отличить настоящую поверхность от
// границы выборки. Восемь клеток по оси дают ровно 512 бит.
#define RIGID_BLOCK_TILE 6
#define RIGID_BLOCK_SAMPLE (RIGID_BLOCK_TILE + 2)
#define RIGID_BLOCK_SAMPLE_CELLS (RIGID_BLOCK_SAMPLE * RIGID_BLOCK_SAMPLE * RIGID_BLOCK_SAMPLE)
#define RIGID_BLOCK_SAMPLE_WORDS (RIGID_BLOCK_SAMPLE_CELLS / 64)
// Тело, накрывающее больше этого числа клеток, с миром не сталкивается:
// обход его окрестности стоил бы дороже всего шага. Это предел размера
// тела, а не скорости; см. контракт в заголовке.
#define RIGID_BLOCK_MAX_CELLS ((int32_t)VOXEL_RIGID_MAX_WORLD_CELLS)

static bool AddInt64Checked(int64_t left, int64_t right, int64_t *outValue)
{
    if (right > 0 && left > INT64_MAX - right)
    {
        return false;
    }
    if (right < 0 && left < INT64_MIN - right)
    {
        return false;
    }
    *outValue = left + right;
    return true;
}

// Клетки выборки: solid хранит ответ мира, known — был ли вопрос задан,
// claimed — вошла ли клетка в уже выданную коробку.
typedef struct BlockSample
{
    const VoxelCollisionSource *collision;
    // true, если origin + любая локальная координата [0,7] не переполняет
    // int64: тогда проверки AddInt64Checked в горячем пути не нужны.
    bool originValid;
    int64_t origin[3];
    uint64_t solid[RIGID_BLOCK_SAMPLE_WORDS];
    uint64_t known[RIGID_BLOCK_SAMPLE_WORDS];
    uint64_t claimed[RIGID_BLOCK_SAMPLE_WORDS];
} BlockSample;

static uint32_t SampleIndex(int32_t x, int32_t y, int32_t z)
{
    return (uint32_t)((z * RIGID_BLOCK_SAMPLE + y) * RIGID_BLOCK_SAMPLE + x);
}

static bool SampleBit(const uint64_t *bits, uint32_t index)
{
    return (bits[index >> 6] & (UINT64_C(1) << (index & 63u))) != 0u;
}

static void SampleBitRaise(uint64_t *bits, uint32_t index)
{
    bits[index >> 6] |= UINT64_C(1) << (index & 63u);
}

// Координаты — внутри выборки, вместе с гало. Мир опрашивается лениво: у
// летящего тела ядро почти всегда пустое, и платить за гало незачем.
static bool SampleSolid(BlockSample *sample, int32_t x, int32_t y, int32_t z)
{
    if (x < 0 || y < 0 || z < 0 || x >= RIGID_BLOCK_SAMPLE || y >= RIGID_BLOCK_SAMPLE ||
        z >= RIGID_BLOCK_SAMPLE)
    {
        // За пределами выборки мир неизвестен. Считать его сплошным
        // безопаснее: неизвестная грань не станет направлением выталкивания.
        return true;
    }
    uint32_t index = SampleIndex(x, y, z);
    if (SampleBit(sample->known, index))
    {
        return SampleBit(sample->solid, index);
    }
    int32_t local[3] = {x, y, z};
    int64_t world[3];
    bool solid = true;
    bool valid = true;
    if (sample->originValid)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            world[axis] = sample->origin[axis] + local[axis];
        }
    }
    else
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            valid = valid && AddInt64Checked(sample->origin[axis], local[axis], &world[axis]);
        }
    }
    if (valid)
    {
        solid = BlockIsSolid(sample->collision, world[0], world[1], world[2]);
    }
    SampleBitRaise(sample->known, index);
    if (solid)
    {
        SampleBitRaise(sample->solid, index);
    }
    return solid;
}

// Ядро выборки: координаты заведомо внутри [0,6], а клетка ещё не
// спрашивалась — known обнулён в начале плитки. Границы и бит known здесь
// лишние, и из горячего пути первого прохода они убраны.
static bool SampleCoreCell(BlockSample *sample, int32_t x, int32_t y, int32_t z)
{
    uint32_t index = SampleIndex(x + 1, y + 1, z + 1);
    int64_t world[3];
    bool solid = true;
    if (sample->originValid)
    {
        world[0] = sample->origin[0] + x + 1;
        world[1] = sample->origin[1] + y + 1;
        world[2] = sample->origin[2] + z + 1;
        solid = BlockIsSolid(sample->collision, world[0], world[1], world[2]);
    }
    else
    {
        bool valid = true;
        int32_t local[3] = {x + 1, y + 1, z + 1};
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            valid = valid && AddInt64Checked(sample->origin[axis], local[axis], &world[axis]);
        }
        if (valid)
        {
            solid = BlockIsSolid(sample->collision, world[0], world[1], world[2]);
        }
    }
    SampleBitRaise(sample->known, index);
    if (solid)
    {
        SampleBitRaise(sample->solid, index);
    }
    return solid;
}

// Свободна для склейки: клетка сплошная и ещё не вошла в другую коробку.
// Спрашивается только про ядро, где ответ мира уже получен.
static bool SampleMergeable(const BlockSample *sample, int32_t x, int32_t y, int32_t z)
{
    uint32_t index = SampleIndex(x + 1, y + 1, z + 1);
    return SampleBit(sample->solid, index) && !SampleBit(sample->claimed, index);
}

// Полупротяжённость коробки, выровненной по осям, вдоль произвольной оси.
static double AxisAlignedRadius(const double half[3], const double axis[3])
{
    return half[0] * AbsoluteDouble(axis[0]) + half[1] * AbsoluteDouble(axis[1]) +
           half[2] * AbsoluteDouble(axis[2]);
}

// Отсечение опорной гранью даёт до восьми точек, а решателю нужно четыре.
// Брать первые попавшиеся нельзя: у повёрнутого куба на столбе они лежат по
// одну сторону от центра масс, и тело заваливается на ровном месте. Поэтому
// остаются самая глубокая точка, самая от неё далёкая и две крайние по
// разные стороны от их линии — четырёхугольник наибольшей площади.
// Вход без const намеренно: в ISO C до C23 указатель на массив нельзя
// молча подставить под указатель на массив с добавленным квалификатором,
// и gcc с -Wpedantic это отвергает.
static void ReduceManifoldPoints(double points[8][3], const double depths[8], uint32_t count,
                                 const double normal[3], BoxManifold *outManifold)
{
    uint32_t chosen[RIGID_MANIFOLD_POINTS];
    uint32_t chosenCount = 0u;
    if (count <= RIGID_MANIFOLD_POINTS)
    {
        for (uint32_t index = 0; index < count; ++index)
        {
            chosen[chosenCount++] = index;
        }
    }
    else
    {
        uint32_t deepest = 0u;
        for (uint32_t index = 1; index < count; ++index)
        {
            if (depths[index] > depths[deepest])
            {
                deepest = index;
            }
        }
        uint32_t farthest = deepest;
        double bestDistance = -1.0;
        for (uint32_t index = 0; index < count; ++index)
        {
            double delta[3];
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                delta[axis] = points[index][axis] - points[deepest][axis];
            }
            double distance = Dot3(delta, delta);
            if (distance > bestDistance)
            {
                bestDistance = distance;
                farthest = index;
            }
        }
        double reference[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            reference[axis] = points[farthest][axis] - points[deepest][axis];
        }
        uint32_t positive = deepest;
        uint32_t negative = deepest;
        double bestPositive = 0.0;
        double bestNegative = 0.0;
        for (uint32_t index = 0; index < count; ++index)
        {
            double delta[3];
            for (int32_t axis = 0; axis < 3; ++axis)
            {
                delta[axis] = points[index][axis] - points[deepest][axis];
            }
            double area[3];
            Cross3(reference, delta, area);
            double signedArea = Dot3(area, normal);
            if (signedArea > bestPositive)
            {
                bestPositive = signedArea;
                positive = index;
            }
            if (signedArea < bestNegative)
            {
                bestNegative = signedArea;
                negative = index;
            }
        }
        uint32_t candidates[RIGID_MANIFOLD_POINTS] = {deepest, farthest, positive, negative};
        for (uint32_t slot = 0; slot < RIGID_MANIFOLD_POINTS; ++slot)
        {
            bool duplicate = false;
            for (uint32_t other = 0; other < chosenCount; ++other)
            {
                duplicate = duplicate || chosen[other] == candidates[slot];
            }
            if (!duplicate)
            {
                chosen[chosenCount++] = candidates[slot];
            }
        }
    }

    outManifold->count = 0u;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outManifold->normal[axis] = normal[axis];
    }
    for (uint32_t slot = 0; slot < chosenCount; ++slot)
    {
        uint32_t source = chosen[slot];
        uint32_t target = outManifold->count++;
        outManifold->depth[target] = depths[source];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            outManifold->point[target][axis] = points[source][axis];
        }
    }
}

// Manifold тела и коробки блоков. От BuildBoxManifold отличается тем, что
// направление выталкивания выбирается только среди открытых граней блока:
// толкать тело внутрь соседнего сплошного блока нельзя, каким бы малым ни
// было там проникновение. Разделение по-прежнему проверяется полным SAT,
// иначе повёрнутый куб «касался» бы блока, до которого ему ещё далеко.
static bool BuildBlockManifold(const RigidBodyCache *cache, const double *halfExtent,
                               const double blockCentre[3], const double blockHalf[3],
                               uint32_t exposedMask, BoxManifold *outManifold)
{
    if (exposedMask == 0u)
    {
        return false;
    }
    double separation[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        separation[axis] = cache->position[axis] - blockCentre[axis];
    }
    double worldReach[3];

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        const double *direction = cache->columns[axis];
        double reach =
            BoxRadius(cache, halfExtent, direction) + AxisAlignedRadius(blockHalf, direction);
        if (reach - AbsoluteDouble(Dot3(separation, direction)) <= 0.0)
        {
            return false;
        }
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double direction[3] = {0.0, 0.0, 0.0};
        direction[axis] = 1.0;
        worldReach[axis] = BoxRadius(cache, halfExtent, direction) + blockHalf[axis];
        if (worldReach[axis] - AbsoluteDouble(separation[axis]) <= 0.0)
        {
            return false;
        }
    }
    for (int32_t bodyAxis = 0; bodyAxis < 3; ++bodyAxis)
    {
        for (int32_t worldAxis = 0; worldAxis < 3; ++worldAxis)
        {
            double unit[3] = {0.0, 0.0, 0.0};
            unit[worldAxis] = 1.0;
            double crossAxis[3];
            Cross3(cache->columns[bodyAxis], unit, crossAxis);
            if (!(Dot3(crossAxis, crossAxis) > 1.0e-12))
            {
                continue;
            }
            double reach =
                BoxRadius(cache, halfExtent, crossAxis) + AxisAlignedRadius(blockHalf, crossAxis);
            if (reach - AbsoluteDouble(Dot3(separation, crossAxis)) <= 0.0)
            {
                return false;
            }
        }
    }

    // Расстояние, на которое пришлось бы вынести тело в каждую сторону.
    // Минимум среди открытых граней и есть выход наружу.
    int32_t bestAxis = -1;
    double bestSign = 1.0;
    double bestDepth = DBL_MAX;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double reach = worldReach[axis];
        for (int32_t direction = 0; direction < 2; ++direction)
        {
            if ((exposedMask & (1u << (axis * 2 + direction))) == 0u)
            {
                continue;
            }
            double sign = direction == 0 ? -1.0 : 1.0;
            double depth = reach - sign * separation[axis];
            if (depth < bestDepth)
            {
                bestDepth = depth;
                bestAxis = axis;
                bestSign = sign;
            }
        }
    }
    if (bestAxis < 0 || !(bestDepth > 0.0))
    {
        return false;
    }

    double referenceNormal[3] = {0.0, 0.0, 0.0};
    referenceNormal[bestAxis] = bestSign;

    double incidentVertices[4][3];
    IncidentFace(cache, halfExtent, referenceNormal, incidentVertices);

    double polygon[8][3];
    double clipped[8][3];
    uint32_t count = 4u;
    for (uint32_t index = 0; index < 4u; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            polygon[index][axis] = incidentVertices[index][axis];
        }
    }
    for (int32_t side = 1; side < 3; ++side)
    {
        int32_t axisIndex = (bestAxis + side) % 3;
        double unit[3] = {0.0, 0.0, 0.0};
        unit[axisIndex] = 1.0;
        count = ClipAgainstPlane(polygon, count, unit,
                                 blockCentre[axisIndex] + blockHalf[axisIndex], clipped);
        if (count == 0u)
        {
            return false;
        }
        double negated[3] = {0.0, 0.0, 0.0};
        negated[axisIndex] = -1.0;
        count = ClipAgainstPlane(clipped, count, negated,
                                 -(blockCentre[axisIndex] - blockHalf[axisIndex]), polygon);
        if (count == 0u)
        {
            return false;
        }
    }

    double planeDistance = Dot3(blockCentre, referenceNormal) + blockHalf[bestAxis];
    double points[8][3];
    double depths[8];
    uint32_t kept = 0u;
    for (uint32_t index = 0; index < count; ++index)
    {
        double depth = planeDistance - Dot3(polygon[index], referenceNormal);
        if (depth <= 0.0)
        {
            continue;
        }
        depths[kept] = depth;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            points[kept][axis] = polygon[index][axis];
        }
        ++kept;
    }
    if (kept == 0u)
    {
        return false;
    }
    ReduceManifoldPoints(points, depths, kept, referenceNormal, outManifold);
    return outManifold->count != 0u;
}

static bool AppendWorldManifold(const VoxelRigidBody *body, uint32_t index,
                                const BoxManifold *manifold, RigidStepScratch *scratch)
{
    for (uint32_t point = 0; point < manifold->count; ++point)
    {
        RigidContact contact = {0};
        contact.bodyIndex = index;
        contact.otherIndex = UINT32_MAX;
        contact.depth = manifold->depth[point];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            contact.normal[axis] = manifold->normal[axis];
            contact.point[axis] = manifold->point[point][axis];
        }
        contact.restitution = body->restitution;
        contact.friction = body->friction;
        scratch->caches[index].hasWorldContact = true;
        if (!AppendContact(scratch, &contact))
        {
            return false;
        }
    }
    return true;
}

// Грань открыта, если хотя бы одна соседняя клетка за ней пуста. Клетки за
// границей выборки считаются сплошными: там ничего не известно, а лишняя
// открытая грань — это выталкивание в неизвестность.
static uint32_t BlockBoxExposure(BlockSample *sample, const int32_t low[3], const int32_t high[3])
{
    uint32_t exposed = 0u;
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        int32_t first = (axis + 1) % 3;
        int32_t second = (axis + 2) % 3;
        for (int32_t direction = 0; direction < 2; ++direction)
        {
            int32_t plane = direction == 0 ? low[axis] - 1 : high[axis] + 1;
            bool open = false;
            for (int32_t alongFirst = low[first]; alongFirst <= high[first] && !open; ++alongFirst)
            {
                for (int32_t alongSecond = low[second]; alongSecond <= high[second] && !open;
                     ++alongSecond)
                {
                    int32_t cell[3];
                    cell[axis] = plane;
                    cell[first] = alongFirst;
                    cell[second] = alongSecond;
                    open = !SampleSolid(sample, cell[0] + 1, cell[1] + 1, cell[2] + 1);
                }
            }
            if (open)
            {
                exposed |= 1u << (axis * 2 + direction);
            }
        }
    }
    return exposed;
}

// Разбирает одну выборку на коробки и выдаёт по ним контакты. false —
// переполнение общего бюджета контактов шага, дальше идти незачем.
static bool CollectSampleContacts(const VoxelRigidBody *body, uint32_t index,
                                  const RigidBodyCache *cache, BlockSample *sample,
                                  const int32_t coreSize[3], RigidStepScratch *scratch)
{
    bool anySolid = false;
    for (int32_t z = 0; z < coreSize[2]; ++z)
    {
        for (int32_t y = 0; y < coreSize[1]; ++y)
        {
            for (int32_t x = 0; x < coreSize[0]; ++x)
            {
                anySolid = SampleCoreCell(sample, x, y, z) || anySolid;
            }
        }
    }
    if (!anySolid)
    {
        return true;
    }

    for (int32_t z = 0; z < coreSize[2]; ++z)
    {
        for (int32_t y = 0; y < coreSize[1]; ++y)
        {
            for (int32_t x = 0; x < coreSize[0]; ++x)
            {
                if (!SampleMergeable(sample, x, y, z))
                {
                    continue;
                }
                // Жадная склейка: сначала вдоль X, потом целыми рядами по Y,
                // потом целыми слоями по Z. Ровный пол так превращается в
                // одну плиту, и внутренних граней у него не остаётся.
                int32_t low[3] = {x, y, z};
                int32_t high[3] = {x, y, z};
                while (high[0] + 1 < coreSize[0] && SampleMergeable(sample, high[0] + 1, y, z))
                {
                    ++high[0];
                }
                bool grow = true;
                while (grow && high[1] + 1 < coreSize[1])
                {
                    for (int32_t along = low[0]; along <= high[0] && grow; ++along)
                    {
                        grow = SampleMergeable(sample, along, high[1] + 1, z);
                    }
                    if (grow)
                    {
                        ++high[1];
                    }
                }
                grow = true;
                while (grow && high[2] + 1 < coreSize[2])
                {
                    for (int32_t alongX = low[0]; alongX <= high[0] && grow; ++alongX)
                    {
                        for (int32_t alongY = low[1]; alongY <= high[1] && grow; ++alongY)
                        {
                            grow = SampleMergeable(sample, alongX, alongY, high[2] + 1);
                        }
                    }
                    if (grow)
                    {
                        ++high[2];
                    }
                }
                for (int32_t claimZ = low[2]; claimZ <= high[2]; ++claimZ)
                {
                    for (int32_t claimY = low[1]; claimY <= high[1]; ++claimY)
                    {
                        for (int32_t claimX = low[0]; claimX <= high[0]; ++claimX)
                        {
                            SampleBitRaise(sample->claimed,
                                           SampleIndex(claimX + 1, claimY + 1, claimZ + 1));
                        }
                    }
                }

                double blockCentre[3];
                double blockHalf[3];
                bool valid = true;
                for (int32_t axis = 0; axis < 3; ++axis)
                {
                    int64_t start = 0;
                    valid = valid && AddInt64Checked(sample->origin[axis], low[axis] + 1, &start);
                    blockHalf[axis] = (double)(high[axis] - low[axis] + 1) * 0.5;
                    blockCentre[axis] = valid ? (double)start + blockHalf[axis] : 0.0;
                }
                if (!valid)
                {
                    continue;
                }
                uint32_t exposed = BlockBoxExposure(sample, low, high);
                BoxManifold manifold;
                if (!BuildBlockManifold(cache, body->halfExtent, blockCentre, blockHalf, exposed,
                                        &manifold))
                {
                    continue;
                }
                if (!AppendWorldManifold(body, index, &manifold, scratch))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

static void CollectWorldContacts(const VoxelRigidBody *bodies, uint32_t bodyCount,
                                 const VoxelCollisionSource *collision, RigidStepScratch *scratch)
{
    (void)bodyCount;
    BlockSample sample;
    sample.collision = collision;
    for (uint32_t ordered = 0; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        const RigidBodyCache *cache = &scratch->caches[index];
        if (!bodies[index].active || bodies[index].sleeping || !cache->collidable)
        {
            continue;
        }

        int64_t blockMin[3];
        int64_t blockMax[3];
        uint64_t span[3];
        bool valid = true;
        for (int32_t axis = 0; axis < 3 && valid; ++axis)
        {
            valid = TryFloorToInt64(cache->aabbMin[axis], &blockMin[axis]) &&
                    TryFloorToInt64(cache->aabbMax[axis], &blockMax[axis]);
        }
        uint64_t cells = 1u;
        for (int32_t axis = 0; axis < 3 && valid; ++axis)
        {
            span[axis] = (uint64_t)blockMax[axis] - (uint64_t)blockMin[axis] + 1u;
            valid = span[axis] <= (uint64_t)RIGID_BLOCK_MAX_CELLS;
            cells = valid ? cells * span[axis] : cells;
            valid = valid && cells <= (uint64_t)RIGID_BLOCK_MAX_CELLS;
        }
        if (!valid)
        {
            continue;
        }

        uint32_t tiles[3];
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            tiles[axis] = (uint32_t)((span[axis] + RIGID_BLOCK_TILE - 1u) / RIGID_BLOCK_TILE);
        }
        for (uint32_t tileZ = 0; tileZ < tiles[2]; ++tileZ)
        {
            for (uint32_t tileY = 0; tileY < tiles[1]; ++tileY)
            {
                for (uint32_t tileX = 0; tileX < tiles[0]; ++tileX)
                {
                    uint32_t tile[3] = {tileX, tileY, tileZ};
                    int32_t coreSize[3];
                    bool ready = true;
                    for (int32_t axis = 0; axis < 3; ++axis)
                    {
                        // Смещение не больше общего числа клеток, а оно уже
                        // проверено: сумма остаётся внутри blockMax.
                        int64_t start =
                            blockMin[axis] + (int64_t)tile[axis] * (int64_t)RIGID_BLOCK_TILE;
                        int64_t remaining = blockMax[axis] - start + 1;
                        coreSize[axis] =
                            remaining < RIGID_BLOCK_TILE ? (int32_t)remaining : RIGID_BLOCK_TILE;
                        ready = ready && AddInt64Checked(start, -1, &sample.origin[axis]);
                    }
                    if (!ready)
                    {
                        continue;
                    }
                    sample.originValid = true;
                    for (int32_t axis = 0; axis < 3; ++axis)
                    {
                        sample.originValid =
                            sample.originValid &&
                            (sample.origin[axis] <= INT64_MAX - (RIGID_BLOCK_SAMPLE - 1));
                    }
                    // Exact fixed-array bounds; Annex K is not available in the no-CRT runtime.
                    // NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
                    memset(sample.solid, 0, sizeof(sample.solid));
                    memset(sample.known, 0, sizeof(sample.known));
                    memset(sample.claimed, 0, sizeof(sample.claimed));
                    // NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
                    if (!CollectSampleContacts(&bodies[index], index, cache, &sample, coreSize,
                                               scratch))
                    {
                        return;
                    }
                }
            }
        }
    }
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
    (void)bodies;
    // Радиус AABB уже посчитан на стадии bounds и лежит в кэше: повторное
    // чтение столбцов поворота и halfExtent здесь было чистым дублированием.
    double largest = 0.0;
    for (uint32_t ordered = 0; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        const RigidBodyCache *cache = &scratch->caches[index];
        if (!cache->collidable)
        {
            continue;
        }
        if (cache->radius > largest)
        {
            largest = cache->radius;
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
        RigidGridEntry *entry = &scratch->grid[index];
        entry->next = RIGID_HASH_EMPTY;
        scratch->next[index] = RIGID_HASH_EMPTY;
        if (!bodies[index].active || !scratch->caches[index].collidable)
        {
            continue;
        }
        uint32_t bucket = CellHash(scratch->caches[index].cell, mask);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            entry->cell[axis] = scratch->caches[index].cell[axis];
        entry->stableOrder = ordered - 1u;
        entry->next = scratch->buckets[bucket];
        // Wake traversal rejects mostly awake bodies before needing cell data.
        // Keep its existing dense links; indexed queries reuse this array later.
        scratch->next[index] = entry->next;
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
    // Кандидатов у одного тела почти всегда единицы: на осевшей куче из 4096
    // тел в среднем семь. На таком числе вставки делают вдвое-втрое меньше
    // сравнений, чем пирамидальная сортировка, а сравнение здесь дорогое — оно
    // тянет ячейку из рабочего кэша тела и stableId из публичного тела, то
    // есть до четырёх строк кэша на одно сравнение.
    //
    // Порядок получается тот же самый: сравнение задаёт строгий полный
    // порядок (stableId уникальны), а у такого порядка отсортированная
    // последовательность единственная. Пирамидальная сортировка остаётся для
    // больших выборок, чтобы худший случай не стал квадратичным.
    if (kept <= RIGID_CANDIDATE_INSERTION_MAX)
    {
        for (uint32_t index = 1u; index < kept; ++index)
        {
            uint32_t value = scratch->next[index];
            uint32_t hole = index;
            while (hole > 0u && CandidateAfter(bodies, scratch, scratch->next[hole - 1u], value))
            {
                scratch->next[hole] = scratch->next[hole - 1u];
                --hole;
            }
            scratch->next[hole] = value;
        }
    }
    else
    {
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
    }

    if (scratch->broadphase != NULL)
    {
        for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
        {
            uint32_t index = scratch->order[ordered];
            if (!bodies[index].sleeping)
            {
                scratch->wakeQueue[queued++] = index;
            }
        }
        for (uint32_t current = 0u; current < queued && queued < scratch->activeCount; ++current)
        {
            uint32_t first = scratch->wakeQueue[current];
            if (!scratch->caches[first].collidable)
            {
                continue;
            }
            uint32_t count = 0u;
            if (!IndexedCandidates(bodies, scratch, first, true, &count))
                return false;
            for (uint32_t candidate = 0u; candidate < count; ++candidate)
            {
                if (!WakeContactPair(bodies, scratch, settings, first, scratch->next[candidate],
                                     &queued))
                    return false;
            }
        }
        return true;
    }

    // Сетка: обход идёт со стороны спящих. Когда спящих меньше, чем
    // бодрствующих, это просматривает заметно меньше окрестностей, а набор
    // пробуждённых тот же: спящий просыпается ровно тогда, когда пересекается
    // с бодрствующим, а пробуждение распространяется через очередь.
    uint32_t mask = scratch->bucketCount - 1u;
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t index = scratch->order[ordered];
        if (!bodies[index].sleeping)
        {
            continue;
        }
        const RigidBodyCache *sleeperCache = &scratch->caches[index];
        if (!sleeperCache->collidable)
        {
            continue;
        }
        for (int32_t dx = -1; dx <= 1 && bodies[index].sleeping; ++dx)
        {
            for (int32_t dy = -1; dy <= 1 && bodies[index].sleeping; ++dy)
            {
                for (int32_t dz = -1; dz <= 1 && bodies[index].sleeping; ++dz)
                {
                    int64_t neighbour[3] = {sleeperCache->cell[0] + dx, sleeperCache->cell[1] + dy,
                                            sleeperCache->cell[2] + dz};
                    uint32_t bucket = CellHash(neighbour, mask);
                    for (uint32_t first = scratch->buckets[bucket]; first != RIGID_HASH_EMPTY;
                         first = scratch->next[first])
                    {
                        if (bodies[first].sleeping)
                        {
                            continue;
                        }
                        const RigidGridEntry *entry = &scratch->grid[first];
                        if (entry->cell[0] != neighbour[0] || entry->cell[1] != neighbour[1] ||
                            entry->cell[2] != neighbour[2])
                        {
                            continue;
                        }
                        if (!scratch->caches[first].collidable)
                        {
                            continue;
                        }
                        if (!WakeContactPair(bodies, scratch, settings, first, index, &queued))
                        {
                            return false;
                        }
                        if (!bodies[index].sleeping)
                        {
                            break;
                        }
                    }
                }
            }
        }
    }
    for (uint32_t current = 0u; current < queued; ++current)
    {
        uint32_t first = scratch->wakeQueue[current];
        const RigidBodyCache *firstCache = &scratch->caches[first];
        if (!firstCache->collidable)
        {
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
                        if (!bodies[second].sleeping)
                        {
                            continue;
                        }
                        const RigidGridEntry *entry = &scratch->grid[second];
                        if (entry->cell[0] != neighbour[0] || entry->cell[1] != neighbour[1] ||
                            entry->cell[2] != neighbour[2])
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
        RigidNarrowphasePoint *saved =
            &scratch->narrowphasePoints[(size_t)first * RIGID_NARROWPHASE_POINTS_PER_BODY];
        if (job->writing && output <= outputEnd &&
            outputEnd - output <= RIGID_NARROWPHASE_POINTS_PER_BODY)
        {
            for (uint32_t point = 0u; output < outputEnd; ++point)
            {
                uint32_t second = saved[point].otherIndex;
                RigidContact contact = {0};
                contact.bodyIndex = first;
                contact.otherIndex = second;
                contact.depth = saved[point].depth;
                contact.restitution = bodies[first].restitution < bodies[second].restitution
                                          ? bodies[first].restitution
                                          : bodies[second].restitution;
                contact.friction = bodies[first].friction < bodies[second].friction
                                       ? bodies[first].friction
                                       : bodies[second].friction;
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                {
                    contact.point[axis] = saved[point].point[axis];
                    contact.normal[axis] = saved[point].normal[axis];
                }
                scratch->contacts[output++] = contact;
            }
            scratch->caches[first].candidatePairs = 0u;
            continue;
        }
        if (!bodies[first].sleeping && firstCache->collidable)
        {
            // Головы всех двадцати семи цепочек читаются заранее: чтения
            // независимы и уходят в память одновременно, а не по очереди
            // после каждой пройденной цепочки. Порядок обхода прежний.
            uint32_t heads[27];
            uint32_t head = 0u;
            for (int32_t dx = -1; dx <= 1; ++dx)
            {
                for (int32_t dy = -1; dy <= 1; ++dy)
                {
                    for (int32_t dz = -1; dz <= 1; ++dz)
                    {
                        const int64_t neighbour[3] = {firstCache->cell[0] + dx,
                                                      firstCache->cell[1] + dy,
                                                      firstCache->cell[2] + dz};
                        heads[head++] = scratch->buckets[CellHash(neighbour, mask)];
                    }
                }
            }
            head = 0u;
            for (int32_t dx = -1; dx <= 1; ++dx)
            {
                for (int32_t dy = -1; dy <= 1; ++dy)
                {
                    for (int32_t dz = -1; dz <= 1; ++dz)
                    {
                        const int64_t neighbour[3] = {firstCache->cell[0] + dx,
                                                      firstCache->cell[1] + dy,
                                                      firstCache->cell[2] + dz};
                        for (uint32_t second = heads[head++]; second != RIGID_HASH_EMPTY;
                             second = scratch->grid[second].next)
                        {
                            const RigidGridEntry *entry = &scratch->grid[second];
                            if (entry->cell[0] != neighbour[0] || entry->cell[1] != neighbour[1] ||
                                entry->cell[2] != neighbour[2] || entry->stableOrder <= ordered ||
                                bodies[second].sleeping)
                                continue;
                            const RigidBodyCache *secondCache = &scratch->caches[second];
                            // The grid only bounds centres, so a neighbouring cell
                            // still holds boxes whose tight AABBs are disjoint. This
                            // is exactly the first test BuildBoxManifold performs, so
                            // rejecting here removes a full SAT without changing any
                            // contact or its order.
                            bool separated = false;
                            for (uint32_t axis = 0u; axis < 3u; ++axis)
                            {
                                if (firstCache->aabbMax[axis] <= secondCache->aabbMin[axis] ||
                                    secondCache->aabbMax[axis] <= firstCache->aabbMin[axis])
                                {
                                    separated = true;
                                    break;
                                }
                            }
                            if (separated)
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
                            else
                            {
                                for (uint32_t point = 0u;
                                     point < manifold.count &&
                                     count + point < RIGID_NARROWPHASE_POINTS_PER_BODY;
                                     ++point)
                                {
                                    RigidNarrowphasePoint *destination = &saved[count + point];
                                    destination->otherIndex = second;
                                    destination->depth = manifold.depth[point];
                                    for (uint32_t axis = 0u; axis < 3u; ++axis)
                                    {
                                        destination->point[axis] = manifold.point[point][axis];
                                        destination->normal[axis] = manifold.normal[axis];
                                    }
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
    // A canonical prefix sum assigns disjoint output ranges. The second pass
    // copies cached geometry when it fits, otherwise regenerates every contact.
    // One large box may still touch hundreds of small boxes without truncation.
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

// Список кандидатов одного тела. На осевшей куче из 4096 тел их в среднем
// семь; сотня с запасом покрывает и заметно более плотные сцены. Если запрос
// не уложился, стадия целиком возвращается к последовательному пути, где
// буфер размером с массив тел, — состав контактов от этого не зависит.
#define RIGID_TREE_NARROW_CANDIDATES 128u

// Узкая фаза по дереву тем же двухпроходным способом, что и сеточная: сначала
// каждое тело считает свои контакты, затем канонический префиксный проход
// раздаёт непересекающиеся куски выхода, и второй проход пишет. Порядок
// контактов задаётся префиксным проходом по scratch->order, то есть он тот же,
// что и у последовательного обхода, при любом числе рабочих.
static void TreeNarrowphaseRange(void *context, uint32_t begin, uint32_t end)
{
    RigidNarrowphaseJob *job = (RigidNarrowphaseJob *)context;
    const VoxelRigidBody *bodies = job->bodies;
    RigidStepScratch *scratch = job->scratch;
    VoxelPhysicsConfigureThread();

    // Запрос пишет в две общие вещи: список кандидатов scratch->next и счётчик
    // посещённых узлов внутри описателя дерева. У каждого диапазона они свои —
    // буфер на стеке и копия описателя, — поэтому потоки не мешают друг другу.
    // Копия описателя смотрит на то же хранилище: сам обход дерево не меняет.
    uint32_t candidates[RIGID_TREE_NARROW_CANDIDATES];
    VoxelRigidBroadphase localTree = *scratch->broadphase;
    RigidStepScratch localScratch = *scratch;
    localScratch.broadphase = &localTree;
    localScratch.next = candidates;
    localScratch.candidateCapacity = RIGID_TREE_NARROW_CANDIDATES;

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
        RigidNarrowphasePoint *saved =
            &scratch->narrowphasePoints[(size_t)first * RIGID_NARROWPHASE_POINTS_PER_BODY];
        if (job->writing && output <= outputEnd &&
            outputEnd - output <= RIGID_NARROWPHASE_POINTS_PER_BODY)
        {
            for (uint32_t point = 0u; output < outputEnd; ++point)
            {
                uint32_t second = saved[point].otherIndex;
                RigidContact contact = {0};
                contact.bodyIndex = first;
                contact.otherIndex = second;
                contact.depth = saved[point].depth;
                contact.restitution = bodies[first].restitution < bodies[second].restitution
                                          ? bodies[first].restitution
                                          : bodies[second].restitution;
                contact.friction = bodies[first].friction < bodies[second].friction
                                       ? bodies[first].friction
                                       : bodies[second].friction;
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                {
                    contact.point[axis] = saved[point].point[axis];
                    contact.normal[axis] = saved[point].normal[axis];
                }
                scratch->contacts[output++] = contact;
            }
            scratch->caches[first].candidatePairs = 0u;
            continue;
        }
        if (bodies[first].active && !bodies[first].sleeping && firstCache->collidable)
        {
            uint32_t visitsBefore = localTree.visitedNodeCount;
            uint32_t candidateCount = 0u;
            if (!IndexedCandidates(bodies, &localScratch, first, false, &candidateCount))
            {
                // Кандидаты не поместились в буфер диапазона. Ничего ещё не
                // записано, поэтому вся стадия честно повторяется
                // последовательно; метка переживает возврат из диапазона.
                scratch->wakeQueue[first] = UINT32_MAX;
                return;
            }
            if (!job->writing)
            {
                scratch->narrowVisits[first] = localTree.visitedNodeCount - visitsBefore;
            }
            for (uint32_t candidate = 0u; candidate < candidateCount; ++candidate)
            {
                uint32_t second = candidates[candidate];
                if (bodies[second].sleeping || bodies[second].stableId <= bodies[first].stableId)
                {
                    continue;
                }
                ++pairs;
                const RigidBodyCache *secondCache = &scratch->caches[second];
                BoxManifold manifold;
                if (!BuildBoxManifold(firstCache, bodies[first].halfExtent, secondCache,
                                      bodies[second].halfExtent, &manifold))
                {
                    continue;
                }
                if (job->writing)
                {
                    if (output > outputEnd || manifold.count > outputEnd - output)
                    {
                        scratch->caches[first].candidatePairs = 1u;
                        return;
                    }
                    double restitution = bodies[first].restitution < bodies[second].restitution
                                             ? bodies[first].restitution
                                             : bodies[second].restitution;
                    double friction = bodies[first].friction < bodies[second].friction
                                          ? bodies[first].friction
                                          : bodies[second].friction;
                    for (uint32_t point = 0u; point < manifold.count; ++point)
                    {
                        scratch->contacts[output++] =
                            PairContact(first, second, &manifold, point, restitution, friction);
                    }
                }
                else
                {
                    for (uint32_t point = 0u;
                         point < manifold.count &&
                         count + point < RIGID_NARROWPHASE_POINTS_PER_BODY;
                         ++point)
                    {
                        RigidNarrowphasePoint *destination = &saved[count + point];
                        destination->otherIndex = second;
                        destination->depth = manifold.depth[point];
                        for (uint32_t axis = 0u; axis < 3u; ++axis)
                        {
                            destination->point[axis] = manifold.point[point][axis];
                            destination->normal[axis] = manifold.normal[axis];
                        }
                    }
                }
                count += manifold.count;
            }
        }
        else if (!job->writing)
        {
            scratch->narrowVisits[first] = 0u;
        }
        if (!job->writing)
        {
            scratch->wakeQueue[first] = count;
            scratch->caches[first].candidatePairs = pairs;
        }
        else
        {
            scratch->caches[first].candidatePairs = output == outputEnd ? 0u : 1u;
        }
    }
}

// Возвращает false при настоящей ошибке шага. Если кандидаты одного из тел не
// поместились в буфер диапазона, стадия не выполнена: outHandled остаётся
// false, и вызывающий проходит последовательным путём.
static bool CollectTreeContactsParallel(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                                        bool *outHandled)
{
    *outHandled = false;
    RigidNarrowphaseJob job = {bodies, scratch, false, 0u};
    ExecuteRange(scratch, scratch->activeCount, 32u, TreeNarrowphaseRange, &job);
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        if (scratch->wakeQueue[scratch->order[ordered]] == UINT32_MAX)
        {
            return true;
        }
    }

    uint32_t worldCount = scratch->contactCount;
    uint32_t total = worldCount;
    uint64_t visits = 0u;
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        uint32_t first = scratch->order[ordered];
        uint32_t count = scratch->wakeQueue[first];
        if (count > scratch->contactCapacity - total)
        {
            return false;
        }
        scratch->wakeQueue[first] = total;
        total += count;
        uint32_t pairs = scratch->caches[first].candidatePairs;
        uint32_t previous = scratch->stats->candidatePairCount;
        scratch->stats->candidatePairCount =
            pairs > UINT32_MAX - previous ? UINT32_MAX : previous + pairs;
        visits += scratch->narrowVisits[first];
    }
    scratch->broadphase->visitedNodeCount =
        visits > UINT32_MAX - scratch->broadphase->visitedNodeCount
            ? UINT32_MAX
            : scratch->broadphase->visitedNodeCount + (uint32_t)visits;

    job.writing = true;
    job.outputEnd = total;
    ExecuteRange(scratch, scratch->activeCount, 32u, TreeNarrowphaseRange, &job);
    for (uint32_t ordered = 0u; ordered < scratch->activeCount; ++ordered)
    {
        if (scratch->caches[scratch->order[ordered]].candidatePairs != 0u)
        {
            return false;
        }
    }
    scratch->contactCount = total;
    // Объединение островов и отметки контактов остаются последовательными и
    // упорядоченными, ровно как в сеточном пути.
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
    *outHandled = true;
    return true;
}

static bool CollectBodyContacts(VoxelRigidBody *bodies, uint32_t bodyCount,
                                RigidStepScratch *scratch)
{
    (void)bodyCount;
    bool parallel = scratch->options != NULL && scratch->options->executor != NULL &&
                    scratch->activeCount > 64u;
    if (parallel && scratch->broadphase == NULL)
    {
        return CollectGridContactsParallel(bodies, scratch);
    }
    if (parallel)
    {
        bool handled = false;
        if (!CollectTreeContactsParallel(bodies, scratch, &handled))
        {
            return false;
        }
        if (handled)
        {
            return true;
        }
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
        // The rank in unique stableId order selects each pair once, without
        // loading the public body or large solver cache for hash collisions.
        // Головы всех двадцати семи цепочек читаются заранее, одним проходом.
        // Раньше очередная корзина запрашивалась только после того, как
        // предыдущая цепочка была пройдена до конца, и двадцать семь промахов
        // мимо кэша выстраивались друг за другом. Независимые чтения процессор
        // держит в полёте одновременно, и ожидание памяти складывается не в
        // сумму, а почти в один промах. Порядок обхода не меняется: соседи
        // перебираются ровно в том же порядке dx, dy, dz.
        uint32_t heads[27];
        uint32_t head = 0u;
        for (int32_t dx = -1; dx <= 1; ++dx)
        {
            for (int32_t dy = -1; dy <= 1; ++dy)
            {
                for (int32_t dz = -1; dz <= 1; ++dz)
                {
                    const int64_t neighbour[3] = {firstCache->cell[0] + dx,
                                                  firstCache->cell[1] + dy,
                                                  firstCache->cell[2] + dz};
                    heads[head++] = scratch->buckets[CellHash(neighbour, mask)];
                }
            }
        }
        head = 0u;
        for (int32_t dx = -1; dx <= 1; ++dx)
        {
            for (int32_t dy = -1; dy <= 1; ++dy)
            {
                for (int32_t dz = -1; dz <= 1; ++dz)
                {
                    const int64_t neighbour[3] = {firstCache->cell[0] + dx,
                                                  firstCache->cell[1] + dy,
                                                  firstCache->cell[2] + dz};
                    for (uint32_t second = heads[head++]; second != RIGID_HASH_EMPTY;
                         second = scratch->grid[second].next)
                    {
                        // Хеш не является координатой: несколько из 27
                        // соседних ключей часто попадают в одну корзину. Без
                        // точной проверки ячейки одна пара добавлялась бы
                        // несколько раз и получала бы лишний импульс.
                        const RigidGridEntry *entry = &scratch->grid[second];
                        const int64_t *secondCell = entry->cell;
                        if (secondCell[0] != neighbour[0] || secondCell[1] != neighbour[1] ||
                            secondCell[2] != neighbour[2])
                        {
                            continue;
                        }
                        if (entry->stableOrder <= ordered || bodies[second].sleeping)
                        {
                            continue;
                        }
                        // Cell adjacency alone admits boxes whose tight AABBs are
                        // disjoint; BuildBoxManifold would reject them on its first
                        // test. Rejecting here is exact and removes the full SAT.
                        const RigidBodyCache *secondCache = &scratch->caches[second];
                        bool separated = false;
                        for (uint32_t axis = 0u; axis < 3u; ++axis)
                        {
                            if (firstCache->aabbMax[axis] <= secondCache->aabbMin[axis] ||
                                secondCache->aabbMax[axis] <= firstCache->aabbMin[axis])
                            {
                                separated = true;
                                break;
                            }
                        }
                        if (separated)
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

static void ContactVelocity(const RigidBodyCache *cache, const double lever[3], double out[3])
{
    double rotational[3];
    Cross3(cache->angular, lever, rotational);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        out[axis] = cache->linear[axis] + rotational[axis];
    }
}

static double PrepareImpulseResponse(double inverseMass, const double inverseInertia[3],
                                     const RigidBodyCache *cache, const double lever[3],
                                     const double direction[3], double angularResponse[3],
                                     double torqueOut[3])
{
    double torque[3];
    Cross3(lever, direction, torque);
    ApplyInverseInertia(cache, inverseInertia, torque, angularResponse);
    if (torqueOut != NULL)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            torqueOut[axis] = torque[axis];
        }
    }
    return inverseMass + Dot3(torque, angularResponse);
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

static void RelativeContactVelocity(const RigidSolverContact *contact,
                                    const RigidStepScratch *scratch, double velocity[3])
{
    ContactVelocity(&scratch->caches[contact->bodyIndex], contact->lever[0], velocity);
    if (contact->otherIndex != UINT32_MAX)
    {
        double otherVelocity[3];
        ContactVelocity(&scratch->caches[contact->otherIndex], contact->lever[1], otherVelocity);
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            velocity[axis] -= otherVelocity[axis];
        }
    }
}

// Крутящий момент первого касательного направления уже посчитан в
// PrepareImpulseResponse(endpoint, direction = 1): повторный Cross3 тех же
// операндов дал бы те же биты, поэтому переиспользуем его.
static double TangentCoupling(const double torque[3], const double angularResponse[3])
{
    return Dot3(torque, angularResponse);
}

static void PrepareContacts(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                            const VoxelRigidStepSettings *settings, uint32_t begin, uint32_t end)
{
    const double penetrationSlop = settings->penetrationSlop;
    const double penetrationCorrection = settings->penetrationCorrection;
    for (uint32_t index = begin; index < end; ++index)
    {
        const RigidContact *geometry = &scratch->contacts[index];
        RigidSolverContact *contact = &scratch->solverContacts[index];
        const uint32_t bodyIndex = geometry->bodyIndex;
        const uint32_t otherIndex = geometry->otherIndex;
        const bool paired = otherIndex != UINT32_MAX;
        const RigidBodyCache *cache = &scratch->caches[bodyIndex];
        const RigidBodyCache *otherCache = paired ? &scratch->caches[otherIndex] : NULL;
        const double bodyInverseMass = bodies[bodyIndex].inverseMass;
        const double *bodyInverseInertia = bodies[bodyIndex].inverseInertia;
        const double otherInverseMass = paired ? bodies[otherIndex].inverseMass : 0.0;
        const double *otherInverseInertia = paired ? bodies[otherIndex].inverseInertia : NULL;

        contact->bodyIndex = bodyIndex;
        contact->otherIndex = otherIndex;
        contact->friction = geometry->friction;
        // Холодная структура обнулялась при создании; у горячего зеркала
        // импульсы обязан обнулить первый же проход подготовки.
        contact->normalImpulse = 0.0;
        contact->tangentImpulse[0] = 0.0;
        contact->tangentImpulse[1] = 0.0;
        const double point[3] = {geometry->point[0], geometry->point[1], geometry->point[2]};
        const double normal[3] = {geometry->normal[0], geometry->normal[1], geometry->normal[2]};
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            contact->lever[0][axis] = point[axis] - cache->position[axis];
            contact->lever[1][axis] = paired ? point[axis] - otherCache->position[axis] : 0.0;
            contact->rows[0].direction[axis] = normal[axis];
        }
        BuildTangents(normal, contact->rows[1].direction, contact->rows[2].direction);
        double masses[3];
        // Cross(lever, direction) первого касательного направления совпадает с
        // крутящим моментом, который нужен TangentCoupling. Сохраняем его по
        // одному разу на тело вместо повторного Cross3.
        double couplingTorque[2][3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
        for (uint32_t direction = 0; direction < 3u; ++direction)
        {
            RigidConstraintRow *row = &contact->rows[direction];
            double *torqueOut = direction == 1u ? couplingTorque[0] : NULL;
            double mass = PrepareImpulseResponse(bodyInverseMass, bodyInverseInertia, cache,
                                                 contact->lever[0], row->direction,
                                                 row->angularResponse[0], torqueOut);
            if (paired)
            {
                torqueOut = direction == 1u ? couplingTorque[1] : NULL;
                mass += PrepareImpulseResponse(otherInverseMass, otherInverseInertia, otherCache,
                                               contact->lever[1], row->direction,
                                               row->angularResponse[1], torqueOut);
            }
            masses[direction] = mass;
            row->inverseEffectiveMass = mass > 0.0 && IsFiniteDouble(mass) ? 1.0 / mass : 0.0;
        }
        // Solve the two coupled tangential directions as a 2x2 block, then
        // project the TOTAL impulse onto the Coulomb disk (not a per-axis box).
        double coupling = TangentCoupling(couplingTorque[0], contact->rows[2].angularResponse[0]);
        if (paired)
        {
            coupling += TangentCoupling(couplingTorque[1], contact->rows[2].angularResponse[1]);
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
        double initialNormalSpeed = Dot3(velocity, normal);
        double penetration = geometry->depth - penetrationSlop;
        double bias = penetration > 0.0
                          ? penetrationCorrection * penetration / RIGID_STEP_SECONDS
                          : 0.0;
        bias = bias > RIGID_MAX_RECOVERY_SPEED ? RIGID_MAX_RECOVERY_SPEED : bias;
        double bounce = initialNormalSpeed < -RIGID_RESTITUTION_THRESHOLD
                            ? -geometry->restitution * initialNormalSpeed
                            : 0.0;
        // Restitution is based on the pre-solve impact speed. Recomputing it
        // each iteration cancels the bounce when the first iteration separates.
        contact->targetNormalSpeed = bias > bounce ? bias : bounce;
    }
}

static void ApplyContactImpulse(RigidStepScratch *scratch, const RigidSolverContact *contact,
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

static void ContactLocalAnchors(const RigidSolverContact *contact, const RigidStepScratch *scratch,
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
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            anchors[endpoint][axis] = Dot3(contact->lever[endpoint], bodyCache->columns[axis]);
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

static uint32_t ContactRunEnd(const RigidStepScratch *scratch, uint32_t begin)
{
    const RigidContact *first = &scratch->contacts[begin];
    uint32_t end = begin + 1u;
    while (end < scratch->contactCount && scratch->contacts[end].bodyIndex == first->bodyIndex &&
           scratch->contacts[end].otherIndex == first->otherIndex)
        ++end;
    return end;
}

static uint32_t FindCachedContact(const VoxelRigidBody *bodies, const RigidSolverContact *contact,
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
        // Hash collisions may belong to another worker. Only the owner of
        // this exact pair may read or write its mutable used flag.
        if (entry->stableIds[0] != firstId || entry->stableIds[1] != secondId)
        {
            continue;
        }
        if (entry->used)
        {
            continue;
        }
        double alignment = 0.0;
        bool finiteImpulse = true;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            alignment += contact->rows[0].direction[axis] * (double)entry->normal[axis];
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

static bool PrepareCachedImpulse(RigidSolverContact *contact, const RigidCachedContact *entry)
{
    double normalImpulse = Dot3(entry->worldImpulse, contact->rows[0].direction);
    double firstImpulse = Dot3(entry->worldImpulse, contact->rows[1].direction);
    double secondImpulse = Dot3(entry->worldImpulse, contact->rows[2].direction);
    if (!(normalImpulse > 0.0) || !IsFiniteDouble(normalImpulse) || !IsFiniteDouble(firstImpulse) ||
        !IsFiniteDouble(secondImpulse))
    {
        return false;
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
    return true;
}

typedef struct RigidWarmMatchJob
{
    const VoxelRigidBody *bodies;
    RigidStepScratch *scratch;
    RigidCachedContact *entries;
    const uint32_t *buckets;
    uint32_t bucketMask;
} RigidWarmMatchJob;

static void MatchCachedContactsRange(void *context, uint32_t begin, uint32_t end)
{
    RigidWarmMatchJob *job = context;
    RigidStepScratch *scratch = job->scratch;
    VoxelPhysicsConfigureThread();
    for (uint32_t cursor = begin; cursor < end;)
    {
        const RigidContact *first = &scratch->contacts[cursor];
        if (cursor != 0u && scratch->contacts[cursor - 1u].bodyIndex == first->bodyIndex &&
            scratch->contacts[cursor - 1u].otherIndex == first->otherIndex)
        {
            // Another range owns this pair's first point and matches the whole
            // run. Skip only our assigned portion, not a possibly huge tail.
            ++cursor;
            continue;
        }
        uint32_t last = ContactRunEnd(scratch, cursor);
        for (uint32_t index = cursor; index < last; ++index)
        {
            RigidSolverContact *contact = &scratch->solverContacts[index];
            uint32_t match = RIGID_HASH_EMPTY;
            if (contact->rows[0].inverseEffectiveMass > 0.0)
            {
                match = FindCachedContact(job->bodies, contact, scratch, job->entries, job->buckets,
                                          job->bucketMask);
            }
            if (match != RIGID_HASH_EMPTY)
            {
                // A chosen old point stays consumed even if its projected
                // impulse cannot be applied, exactly as in the serial path.
                job->entries[match].used = true;
                if (!PrepareCachedImpulse(contact, &job->entries[match]))
                    match = RIGID_HASH_EMPTY;
            }
            // This array is unused until the later solver scheduling phase.
            scratch->solveOrder[index] = match;
        }
        cursor = last;
    }
}

static void WarmStartContacts(const VoxelRigidBody *bodies, RigidStepScratch *scratch,
                              VoxelRigidContactCache *contactCache)
{
    contactCache->matchedContactCount = 0u;
    if (contactCache->contactCount == 0u)
        return;
    RigidCachedContact *entries = ContactCacheEntries(contactCache);
    const uint32_t *buckets = ContactCacheBuckets(contactCache);
    uint32_t bucketMask = BucketCountFor(contactCache->bodyCapacity) - 1u;
    for (uint32_t index = 0u; index < contactCache->contactCount; ++index)
    {
        entries[index].used = false;
    }
    bool parallelMatch = scratch->options != NULL && scratch->options->executor != NULL &&
                         scratch->contactCount > 64u;
    if (parallelMatch)
    {
        // Collectors emit each ordered pair in one contiguous run. Matching
        // needs only prepared geometry, never velocities changed by impulses.
        // Pair owners can therefore match independently; application stays in
        // canonical order after the synchronous dispatch barrier.
        RigidWarmMatchJob job = {bodies, scratch, entries, buckets, bucketMask};
        ExecuteRange(scratch, scratch->contactCount, 64u, MatchCachedContactsRange, &job);
    }
    for (uint32_t index = 0u; index < scratch->contactCount; ++index)
    {
        RigidSolverContact *contact = &scratch->solverContacts[index];
        if (!(contact->rows[0].inverseEffectiveMass > 0.0))
        {
            continue;
        }
        uint32_t match = parallelMatch ? scratch->solveOrder[index]
                                       : FindCachedContact(bodies, contact, scratch, entries,
                                                           buckets, bucketMask);
        if (match == RIGID_HASH_EMPTY)
        {
            continue;
        }
        if (!parallelMatch)
        {
            RigidCachedContact *entry = &entries[match];
            entry->used = true;
            if (!PrepareCachedImpulse(contact, entry))
                continue;
        }
        ApplyContactImpulse(scratch, contact, 0u, contact->normalImpulse);
        ApplyContactImpulse(scratch, contact, 1u, contact->tangentImpulse[0]);
        ApplyContactImpulse(scratch, contact, 2u, contact->tangentImpulse[1]);
        ++contactCache->matchedContactCount;
    }
}

typedef struct RigidStoreCacheJob
{
    const VoxelRigidBody *bodies;
    const RigidStepScratch *scratch;
    RigidCachedContact *entries;
} RigidStoreCacheJob;

// Поля кэша (ID пары, локальные якоря, нормаль, мировой импульс) не зависят
// друг от друга, поэтому считаются параллельно. Сшивка цепочек остаётся
// последовательной: её порядок задаёт канонический порядок в корзине.
static void StoreCacheFieldsRange(void *context, uint32_t begin, uint32_t end)
{
    RigidStoreCacheJob *job = (RigidStoreCacheJob *)context;
    VoxelPhysicsConfigureThread();
    for (uint32_t index = begin; index < end; ++index)
    {
        const RigidSolverContact *contact = &job->scratch->solverContacts[index];
        RigidCachedContact *entry = &job->entries[index];
        entry->stableIds[0] = job->bodies[contact->bodyIndex].stableId;
        entry->stableIds[1] =
            contact->otherIndex == UINT32_MAX ? 0u : job->bodies[contact->otherIndex].stableId;
        ContactLocalAnchors(contact, job->scratch, entry->localAnchors);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            entry->normal[axis] = (float)contact->rows[0].direction[axis];
            entry->worldImpulse[axis] =
                contact->rows[0].direction[axis] * contact->normalImpulse +
                contact->rows[1].direction[axis] * contact->tangentImpulse[0] +
                contact->rows[2].direction[axis] * contact->tangentImpulse[1];
        }
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
    if (scratch->contactCount != 0u)
    {
        RigidStoreCacheJob job = {bodies, scratch, entries};
        ExecuteRange(scratch, scratch->contactCount, 64u, StoreCacheFieldsRange, &job);
    }
    // Reverse insertion gives ascending canonical contact order in each chain.
    // The scratch poses still describe the pre-integration contact geometry.
    for (uint32_t remaining = scratch->contactCount; remaining > 0u; --remaining)
    {
        uint32_t index = remaining - 1u;
        RigidCachedContact *entry = &entries[index];
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
    RigidSolverContact *contact = &scratch->solverContacts[index];
    if (!(contact->rows[0].inverseEffectiveMass > 0.0))
    {
        return;
    }
    double velocity[3];
    RelativeContactVelocity(contact, scratch, velocity);
    double normalSpeed = Dot3(velocity, contact->rows[0].direction);
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

// === Парный решатель (SSE2 / NEON) ===
//
// Контакты одной полосы раскраски попарно не делят тел, поэтому два контакта
// можно решать одной 128-битной командой: полоса 0 — один контакт, полоса 1 —
// другой. Каждая полоса выполняет ту же последовательность операций над теми
// же операндами, что и скалярный SolveContact, а SSE2/NEON дают корректно
// округлённые поэлементные операции над binary64 (без FMA), поэтому результат
// побитово совпадает. Тела собираются в полосы на входе и раскладываются
// обратно на выходе.
#if defined(LAIUE_RIGID_PAIRED_SSE2) || defined(LAIUE_RIGID_PAIRED_NEON)

#if defined(LAIUE_RIGID_PAIRED_NEON)
typedef float64x2_t LaiuePairVector;
typedef uint64x2_t LaiuePairMask;

static inline LaiuePairVector LaiuePairSet(double low, double high)
{
    float64x2_t value = vdupq_n_f64(0.0);
    value = vsetq_lane_f64(low, value, 0);
    value = vsetq_lane_f64(high, value, 1);
    return value;
}

static inline double LaiuePairLow(LaiuePairVector value)
{
    return vgetq_lane_f64(value, 0);
}

static inline double LaiuePairHigh(LaiuePairVector value)
{
    return vgetq_lane_f64(value, 1);
}

static inline LaiuePairVector LaiuePairSplat(double value)
{
    return vdupq_n_f64(value);
}

static inline LaiuePairVector LaiuePairAdd(LaiuePairVector left, LaiuePairVector right)
{
    return vaddq_f64(left, right);
}

static inline LaiuePairVector LaiuePairSub(LaiuePairVector left, LaiuePairVector right)
{
    return vsubq_f64(left, right);
}

static inline LaiuePairVector LaiuePairMul(LaiuePairVector left, LaiuePairVector right)
{
    return vmulq_f64(left, right);
}

static inline LaiuePairVector LaiuePairDiv(LaiuePairVector left, LaiuePairVector right)
{
    return vdivq_f64(left, right);
}

static inline LaiuePairVector LaiuePairNeg(LaiuePairVector value)
{
    return vnegq_f64(value);
}

static inline LaiuePairVector LaiuePairAbs(LaiuePairVector value)
{
    return vabsq_f64(value);
}

static inline LaiuePairVector LaiuePairSqrt(LaiuePairVector value)
{
    return vsqrtq_f64(value);
}

static inline LaiuePairVector LaiuePairInfinity(void)
{
    return vreinterpretq_f64_u64(vdupq_n_u64(UINT64_C(0x7ff0000000000000)));
}

static inline LaiuePairMask LaiuePairLess(LaiuePairVector left, LaiuePairVector right)
{
    return vcltq_f64(left, right);
}

static inline LaiuePairMask LaiuePairGreater(LaiuePairVector left, LaiuePairVector right)
{
    return vcgtq_f64(left, right);
}

static inline LaiuePairMask LaiuePairNotEqual(LaiuePairVector left, LaiuePairVector right)
{
    return vbicq_u64(vdupq_n_u64(~UINT64_C(0)), vceqq_f64(left, right));
}

static inline LaiuePairMask LaiuePairMaskAnd(LaiuePairMask left, LaiuePairMask right)
{
    return vandq_u64(left, right);
}

static inline LaiuePairVector LaiuePairSelect(LaiuePairMask mask, LaiuePairVector yes,
                                              LaiuePairVector no)
{
    return vbslq_f64(mask, yes, no);
}

static inline LaiuePairMask LaiuePairMaskFromBools(bool low, bool high)
{
    uint64x2_t value = vdupq_n_u64(0u);
    value = vsetq_lane_u64(low ? ~UINT64_C(0) : UINT64_C(0), value, 0);
    value = vsetq_lane_u64(high ? ~UINT64_C(0) : UINT64_C(0), value, 1);
    return value;
}
#else
typedef __m128d LaiuePairVector;
typedef __m128d LaiuePairMask;

static inline LaiuePairVector LaiuePairSet(double low, double high)
{
    return _mm_setr_pd(low, high);
}

static inline double LaiuePairLow(LaiuePairVector value)
{
    return _mm_cvtsd_f64(value);
}

static inline double LaiuePairHigh(LaiuePairVector value)
{
    return _mm_cvtsd_f64(_mm_unpackhi_pd(value, value));
}

static inline LaiuePairVector LaiuePairSplat(double value)
{
    return _mm_set1_pd(value);
}

static inline LaiuePairVector LaiuePairAdd(LaiuePairVector left, LaiuePairVector right)
{
    return _mm_add_pd(left, right);
}

static inline LaiuePairVector LaiuePairSub(LaiuePairVector left, LaiuePairVector right)
{
    return _mm_sub_pd(left, right);
}

static inline LaiuePairVector LaiuePairMul(LaiuePairVector left, LaiuePairVector right)
{
    return _mm_mul_pd(left, right);
}

static inline LaiuePairVector LaiuePairDiv(LaiuePairVector left, LaiuePairVector right)
{
    return _mm_div_pd(left, right);
}

static inline LaiuePairVector LaiuePairNeg(LaiuePairVector value)
{
    return _mm_xor_pd(value, _mm_castsi128_pd(_mm_set1_epi64x((long long)0x8000000000000000LL)));
}

static inline LaiuePairVector LaiuePairAbs(LaiuePairVector value)
{
    return _mm_and_pd(value, _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL)));
}

static inline LaiuePairVector LaiuePairSqrt(LaiuePairVector value)
{
    return _mm_sqrt_pd(value);
}

static inline LaiuePairVector LaiuePairInfinity(void)
{
    return _mm_castsi128_pd(_mm_set1_epi64x((long long)0x7ff0000000000000LL));
}

static inline LaiuePairMask LaiuePairLess(LaiuePairVector left, LaiuePairVector right)
{
    return _mm_cmplt_pd(left, right);
}

static inline LaiuePairMask LaiuePairGreater(LaiuePairVector left, LaiuePairVector right)
{
    return _mm_cmpgt_pd(left, right);
}

static inline LaiuePairMask LaiuePairNotEqual(LaiuePairVector left, LaiuePairVector right)
{
    return _mm_cmpneq_pd(left, right);
}

static inline LaiuePairMask LaiuePairMaskAnd(LaiuePairMask left, LaiuePairMask right)
{
    return _mm_and_pd(left, right);
}

static inline LaiuePairVector LaiuePairSelect(LaiuePairMask mask, LaiuePairVector yes,
                                              LaiuePairVector no)
{
    return _mm_or_pd(_mm_and_pd(mask, yes), _mm_andnot_pd(mask, no));
}

static inline LaiuePairMask LaiuePairMaskFromBools(bool low, bool high)
{
    return _mm_castsi128_pd(_mm_set_epi64x(high ? -1LL : 0LL, low ? -1LL : 0LL));
}
#endif

static inline LaiuePairVector LaiuePairDot3(const LaiuePairVector left[3],
                                            const LaiuePairVector right[3])
{
    return LaiuePairAdd(
        LaiuePairAdd(LaiuePairMul(left[0], right[0]), LaiuePairMul(left[1], right[1])),
        LaiuePairMul(left[2], right[2]));
}

static inline void LaiuePairCross3(const LaiuePairVector left[3], const LaiuePairVector right[3],
                                   LaiuePairVector out[3])
{
    out[0] = LaiuePairSub(LaiuePairMul(left[1], right[2]), LaiuePairMul(left[2], right[1]));
    out[1] = LaiuePairSub(LaiuePairMul(left[2], right[0]), LaiuePairMul(left[0], right[2]));
    out[2] = LaiuePairSub(LaiuePairMul(left[0], right[1]), LaiuePairMul(left[1], right[0]));
}

// IsFiniteDouble без ветвления: |value| < +inf.
static inline LaiuePairMask LaiuePairFinite(LaiuePairVector value)
{
    return LaiuePairLess(LaiuePairAbs(value), LaiuePairInfinity());
}

// Применение одного ряда импульса к одному концу. Порядок
// (direction * magnitude) * inverseMass и angularResponse * magnitude
// повторяет скалярный ApplyPreparedImpulse; выключенные полосы получают
// прежнее значение выбором, а не добавлением нуля (ноль может сменить знак -0).
static inline void LaiuePairApplyRow(LaiuePairVector linear[3], LaiuePairVector angular[3],
                                     LaiuePairVector inverseMass,
                                     const LaiuePairVector direction[3],
                                     const LaiuePairVector response[3], LaiuePairVector magnitude,
                                     LaiuePairMask apply)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        LaiuePairVector linearIncrement =
            LaiuePairMul(LaiuePairMul(direction[axis], magnitude), inverseMass);
        linear[axis] =
            LaiuePairSelect(apply, LaiuePairAdd(linear[axis], linearIncrement), linear[axis]);
        LaiuePairVector angularIncrement = LaiuePairMul(response[axis], magnitude);
        angular[axis] =
            LaiuePairSelect(apply, LaiuePairAdd(angular[axis], angularIncrement), angular[axis]);
    }
}

static inline void LaiuePairLoadRow(const RigidSolverContact *first,
                                    const RigidSolverContact *second, uint32_t direction,
                                    LaiuePairVector outDirection[3],
                                    LaiuePairVector outResponse0[3],
                                    LaiuePairVector outResponse1[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        outDirection[axis] = LaiuePairSet(first->rows[direction].direction[axis],
                                          second->rows[direction].direction[axis]);
        outResponse0[axis] = LaiuePairSet(first->rows[direction].angularResponse[0][axis],
                                          second->rows[direction].angularResponse[0][axis]);
        outResponse1[axis] = LaiuePairSet(first->rows[direction].angularResponse[1][axis],
                                          second->rows[direction].angularResponse[1][axis]);
    }
}

// Скорости двух концов, собранные в полосы, раскладываются обратно в кэши тел.
// Контакт с миром второй конец не пишет.
static inline void LaiuePairScatter(RigidBodyCache *firstCache0, RigidBodyCache *secondCache0,
                                    RigidBodyCache *firstCache1, RigidBodyCache *secondCache1,
                                    bool firstPaired, bool secondPaired,
                                    const LaiuePairVector linear0[3],
                                    const LaiuePairVector angular0[3],
                                    const LaiuePairVector linear1[3],
                                    const LaiuePairVector angular1[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        firstCache0->linear[axis] = LaiuePairLow(linear0[axis]);
        secondCache0->linear[axis] = LaiuePairHigh(linear0[axis]);
        firstCache0->angular[axis] = LaiuePairLow(angular0[axis]);
        secondCache0->angular[axis] = LaiuePairHigh(angular0[axis]);
        if (firstPaired)
        {
            firstCache1->linear[axis] = LaiuePairLow(linear1[axis]);
            firstCache1->angular[axis] = LaiuePairLow(angular1[axis]);
        }
        if (secondPaired)
        {
            secondCache1->linear[axis] = LaiuePairHigh(linear1[axis]);
            secondCache1->angular[axis] = LaiuePairHigh(angular1[axis]);
        }
    }
}

#endif // LAIUE_RIGID_PAIRED_SSE2 || LAIUE_RIGID_PAIRED_NEON

#if defined(LAIUE_RIGID_PAIRED_SSE2) || defined(LAIUE_RIGID_PAIRED_NEON)
// Состояние пары между нормальным импульсом и трением. Трение вынесено в
// отдельную функцию не ради структуры, а ради стека: в сборке без CRT кадр
// функции ограничен 4 КиБ, а в Debug каждый промежуточный вектор получает
// свой слот, и целиком SolveContactPair в предел не влезала.
typedef struct LaiuePairFrictionState
{
    // Векторы первыми: у них выравнивание 16, и после девяти указателей
    // компилятор вставил бы зазор (это ошибка C4324 при /WX). Хвост
    // добивается явно, чтобы размер остался кратен 16 без скрытого зазора.
    LaiuePairVector inverseMass0;
    LaiuePairVector inverseMass1;
    LaiuePairVector friction;
    LaiuePairVector accumulated;
    LaiuePairVector previousNormal;
    LaiuePairMask hasFriction;
    LaiuePairMask paired;
    RigidSolverContact *first;
    RigidSolverContact *second;
    LaiuePairVector *linear0;
    LaiuePairVector *angular0;
    LaiuePairVector *linear1;
    LaiuePairVector *angular1;
    const LaiuePairVector *lever0;
    const LaiuePairVector *lever1;
    LaiuePairVector *velocity;
    uint64_t alignmentPadding;
} LaiuePairFrictionState;

static void SolveContactPairFriction(const LaiuePairFrictionState *state)
{
    RigidSolverContact *first = state->first;
    RigidSolverContact *second = state->second;
    LaiuePairVector *linear0 = state->linear0;
    LaiuePairVector *angular0 = state->angular0;
    LaiuePairVector *linear1 = state->linear1;
    LaiuePairVector *angular1 = state->angular1;
    const LaiuePairVector *lever0 = state->lever0;
    const LaiuePairVector *lever1 = state->lever1;
    LaiuePairVector *velocity = state->velocity;
    const LaiuePairVector inverseMass0 = state->inverseMass0;
    const LaiuePairVector inverseMass1 = state->inverseMass1;
    const LaiuePairVector friction = state->friction;
    const LaiuePairVector accumulated = state->accumulated;
    const LaiuePairVector previousNormal = state->previousNormal;
    const LaiuePairMask hasFriction = state->hasFriction;
    const LaiuePairMask paired = state->paired;
    const LaiuePairVector zero = LaiuePairSplat(0.0);

    // Трение: скорость пересчитывается там, где нормальный импульс её изменил.
    {
        LaiuePairVector rotational0[3];
        LaiuePairVector rotational1[3];
        LaiuePairCross3(angular0, lever0, rotational0);
        LaiuePairCross3(angular1, lever1, rotational1);
        const LaiuePairMask recompute =
            LaiuePairMaskAnd(hasFriction, LaiuePairNotEqual(accumulated, previousNormal));
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            LaiuePairVector relative = LaiuePairAdd(linear0[axis], rotational0[axis]);
            LaiuePairVector other = LaiuePairAdd(linear1[axis], rotational1[axis]);
            LaiuePairVector updated =
                LaiuePairSelect(paired, LaiuePairSub(relative, other), relative);
            velocity[axis] = LaiuePairSelect(recompute, updated, velocity[axis]);
        }
    }

    LaiuePairVector firstDirection[3];
    LaiuePairVector firstResponse0[3];
    LaiuePairVector firstResponse1[3];
    LaiuePairLoadRow(first, second, 1u, firstDirection, firstResponse0, firstResponse1);
    LaiuePairVector secondDirection[3];
    LaiuePairVector secondResponse0[3];
    LaiuePairVector secondResponse1[3];
    LaiuePairLoadRow(first, second, 2u, secondDirection, secondResponse0, secondResponse1);

    const LaiuePairVector firstSpeed = LaiuePairDot3(velocity, firstDirection);
    const LaiuePairVector secondSpeed = LaiuePairDot3(velocity, secondDirection);
    const LaiuePairVector firstMass =
        LaiuePairSet(first->rows[1].inverseEffectiveMass, second->rows[1].inverseEffectiveMass);
    const LaiuePairVector secondMass =
        LaiuePairSet(first->rows[2].inverseEffectiveMass, second->rows[2].inverseEffectiveMass);
    const LaiuePairVector crossMass =
        LaiuePairSet(first->tangentCrossMass, second->tangentCrossMass);
    const LaiuePairVector previousFirst =
        LaiuePairSet(first->tangentImpulse[0], second->tangentImpulse[0]);
    const LaiuePairVector previousSecond =
        LaiuePairSet(first->tangentImpulse[1], second->tangentImpulse[1]);
    LaiuePairVector firstImpulse =
        LaiuePairSub(LaiuePairSub(previousFirst, LaiuePairMul(firstMass, firstSpeed)),
                     LaiuePairMul(crossMass, secondSpeed));
    LaiuePairVector secondImpulse =
        LaiuePairSub(LaiuePairSub(previousSecond, LaiuePairMul(crossMass, firstSpeed)),
                     LaiuePairMul(secondMass, secondSpeed));

    // Проекция суммарного импульса на диск Кулона. Масштабирование делается до
    // возведения в квадрат: только ветвь с наибольшим по модулю импульсом.
    const LaiuePairVector limit = LaiuePairMul(friction, accumulated);
    const LaiuePairVector absoluteFirst = LaiuePairAbs(firstImpulse);
    const LaiuePairVector absoluteSecond = LaiuePairAbs(secondImpulse);
    const LaiuePairVector largest = LaiuePairSelect(LaiuePairGreater(absoluteSecond, absoluteFirst),
                                                    absoluteSecond, absoluteFirst);
    const LaiuePairMask scaleGuard =
        LaiuePairMaskAnd(LaiuePairGreater(largest, zero),
                         LaiuePairLess(limit, LaiuePairMul(largest, LaiuePairSplat(2.0))));
    const LaiuePairVector scaledFirst = LaiuePairDiv(firstImpulse, largest);
    const LaiuePairVector scaledSecond = LaiuePairDiv(secondImpulse, largest);
    const LaiuePairVector scaledLimit = LaiuePairDiv(limit, largest);
    const LaiuePairVector lengthSquared = LaiuePairAdd(LaiuePairMul(scaledFirst, scaledFirst),
                                                       LaiuePairMul(scaledSecond, scaledSecond));
    const LaiuePairMask scaleMask = LaiuePairMaskAnd(
        scaleGuard, LaiuePairGreater(lengthSquared, LaiuePairMul(scaledLimit, scaledLimit)));
    const LaiuePairVector scale = LaiuePairDiv(scaledLimit, LaiuePairSqrt(lengthSquared));
    firstImpulse = LaiuePairSelect(scaleMask, LaiuePairMul(firstImpulse, scale), firstImpulse);
    secondImpulse = LaiuePairSelect(scaleMask, LaiuePairMul(secondImpulse, scale), secondImpulse);

    const LaiuePairVector firstDelta = LaiuePairSub(firstImpulse, previousFirst);
    const LaiuePairVector secondDelta = LaiuePairSub(secondImpulse, previousSecond);
    const LaiuePairMask firstApply =
        LaiuePairMaskAnd(hasFriction, LaiuePairMaskAnd(LaiuePairNotEqual(firstDelta, zero),
                                                       LaiuePairFinite(firstDelta)));
    const LaiuePairMask secondApply =
        LaiuePairMaskAnd(hasFriction, LaiuePairMaskAnd(LaiuePairNotEqual(secondDelta, zero),
                                                       LaiuePairFinite(secondDelta)));
    LaiuePairApplyRow(linear0, angular0, inverseMass0, firstDirection, firstResponse0, firstDelta,
                      firstApply);
    LaiuePairApplyRow(linear1, angular1, inverseMass1, firstDirection, firstResponse1,
                      LaiuePairNeg(firstDelta), LaiuePairMaskAnd(firstApply, paired));
    LaiuePairApplyRow(linear0, angular0, inverseMass0, secondDirection, secondResponse0,
                      secondDelta, secondApply);
    LaiuePairApplyRow(linear1, angular1, inverseMass1, secondDirection, secondResponse1,
                      LaiuePairNeg(secondDelta), LaiuePairMaskAnd(secondApply, paired));

    first->tangentImpulse[0] =
        LaiuePairLow(LaiuePairSelect(hasFriction, firstImpulse, previousFirst));
    second->tangentImpulse[0] =
        LaiuePairHigh(LaiuePairSelect(hasFriction, firstImpulse, previousFirst));
    first->tangentImpulse[1] =
        LaiuePairLow(LaiuePairSelect(hasFriction, secondImpulse, previousSecond));
    second->tangentImpulse[1] =
        LaiuePairHigh(LaiuePairSelect(hasFriction, secondImpulse, previousSecond));

}
#endif

static void SolveContactPair(RigidStepScratch *scratch, uint32_t firstIndex, uint32_t secondIndex)
{
#if defined(LAIUE_RIGID_PAIRED_SSE2) || defined(LAIUE_RIGID_PAIRED_NEON)
    RigidSolverContact *first = &scratch->solverContacts[firstIndex];
    RigidSolverContact *second = &scratch->solverContacts[secondIndex];
    if (!(first->rows[0].inverseEffectiveMass > 0.0) ||
        !(second->rows[0].inverseEffectiveMass > 0.0))
    {
        SolveContact(scratch, firstIndex);
        SolveContact(scratch, secondIndex);
        return;
    }

    const bool firstPaired = first->otherIndex != UINT32_MAX;
    const bool secondPaired = second->otherIndex != UINT32_MAX;
    RigidBodyCache *firstCache0 = &scratch->caches[first->bodyIndex];
    RigidBodyCache *secondCache0 = &scratch->caches[second->bodyIndex];
    RigidBodyCache *firstCache1 = firstPaired ? &scratch->caches[first->otherIndex] : firstCache0;
    RigidBodyCache *secondCache1 =
        secondPaired ? &scratch->caches[second->otherIndex] : secondCache0;

    const LaiuePairVector zero = LaiuePairSplat(0.0);
    const LaiuePairMask paired = LaiuePairMaskFromBools(firstPaired, secondPaired);

    LaiuePairVector linear0[3];
    LaiuePairVector angular0[3];
    LaiuePairVector linear1[3];
    LaiuePairVector angular1[3];
    LaiuePairVector lever0[3];
    LaiuePairVector lever1[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        linear0[axis] = LaiuePairSet(firstCache0->linear[axis], secondCache0->linear[axis]);
        angular0[axis] = LaiuePairSet(firstCache0->angular[axis], secondCache0->angular[axis]);
        linear1[axis] = LaiuePairSet(firstCache1->linear[axis], secondCache1->linear[axis]);
        angular1[axis] = LaiuePairSet(firstCache1->angular[axis], secondCache1->angular[axis]);
        lever0[axis] = LaiuePairSet(first->lever[0][axis], second->lever[0][axis]);
        lever1[axis] = LaiuePairSet(first->lever[1][axis], second->lever[1][axis]);
    }
    const LaiuePairVector inverseMass0 =
        LaiuePairSet(firstCache0->inverseMass, secondCache0->inverseMass);
    const LaiuePairVector inverseMass1 =
        LaiuePairSet(firstCache1->inverseMass, secondCache1->inverseMass);
    const LaiuePairVector friction = LaiuePairSet(first->friction, second->friction);
    const LaiuePairMask hasFriction = LaiuePairGreater(friction, zero);

    // Относительная скорость: конец 0 минус конец 1 там, где второй существует.
    LaiuePairVector velocity[3];
    {
        LaiuePairVector rotational0[3];
        LaiuePairVector rotational1[3];
        LaiuePairCross3(angular0, lever0, rotational0);
        LaiuePairCross3(angular1, lever1, rotational1);
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            LaiuePairVector relative = LaiuePairAdd(linear0[axis], rotational0[axis]);
            LaiuePairVector other = LaiuePairAdd(linear1[axis], rotational1[axis]);
            velocity[axis] = LaiuePairSelect(paired, LaiuePairSub(relative, other), relative);
        }
    }

    // Нормальный импульс.
    LaiuePairVector direction[3];
    LaiuePairVector response0[3];
    LaiuePairVector response1[3];
    LaiuePairLoadRow(first, second, 0u, direction, response0, response1);
    const LaiuePairVector normalMass =
        LaiuePairSet(first->rows[0].inverseEffectiveMass, second->rows[0].inverseEffectiveMass);
    const LaiuePairVector target =
        LaiuePairSet(first->targetNormalSpeed, second->targetNormalSpeed);
    const LaiuePairVector normalSpeed = LaiuePairDot3(velocity, direction);
    const LaiuePairVector previousNormal =
        LaiuePairSet(first->normalImpulse, second->normalImpulse);
    LaiuePairVector accumulated =
        LaiuePairAdd(previousNormal, LaiuePairMul(LaiuePairSub(target, normalSpeed), normalMass));
    accumulated = LaiuePairSelect(LaiuePairLess(accumulated, zero), zero, accumulated);
    first->normalImpulse = LaiuePairLow(accumulated);
    second->normalImpulse = LaiuePairHigh(accumulated);
    const LaiuePairVector normalDelta = LaiuePairSub(accumulated, previousNormal);
    const LaiuePairMask normalApply =
        LaiuePairMaskAnd(LaiuePairNotEqual(normalDelta, zero), LaiuePairFinite(normalDelta));
    LaiuePairApplyRow(linear0, angular0, inverseMass0, direction, response0, normalDelta,
                      normalApply);
    LaiuePairApplyRow(linear1, angular1, inverseMass1, direction, response1,
                      LaiuePairNeg(normalDelta), LaiuePairMaskAnd(normalApply, paired));

    if (!(first->friction > 0.0) && !(second->friction > 0.0))
    {
        LaiuePairScatter(firstCache0, secondCache0, firstCache1, secondCache1, firstPaired,
                         secondPaired, linear0, angular0, linear1, angular1);
        return;
    }

    {
        LaiuePairFrictionState state;
        state.first = first;
        state.second = second;
        state.linear0 = linear0;
        state.angular0 = angular0;
        state.linear1 = linear1;
        state.angular1 = angular1;
        state.lever0 = lever0;
        state.lever1 = lever1;
        state.velocity = velocity;
        state.inverseMass0 = inverseMass0;
        state.inverseMass1 = inverseMass1;
        state.friction = friction;
        state.accumulated = accumulated;
        state.previousNormal = previousNormal;
        state.hasFriction = hasFriction;
        state.paired = paired;
        state.alignmentPadding = 0u;
        SolveContactPairFriction(&state);
    }

    LaiuePairScatter(firstCache0, secondCache0, firstCache1, secondCache1, firstPaired,
                     secondPaired, linear0, angular0, linear1, angular1);
#else
    // Архитектура без парного пути: обе ветви остаются скалярными.
    SolveContact(scratch, firstIndex);
    SolveContact(scratch, secondIndex);
#endif
}

typedef struct RigidJobContext
{
    const VoxelRigidBody *bodies;
    RigidStepScratch *scratch;
    const VoxelRigidStepSettings *settings;
    uint32_t offset;
    // Полосы цветного решателя не делят тел, поэтому их можно решать парами.
    // Полоса переполнения (больше цветов, чем помещается) остаётся скалярной:
    // её манифольды могут делить тела, и парный путь нарушил бы порядок.
    bool pairedRuns;
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

#if defined(LAIUE_RIGID_PAIRED_SSE2) || defined(LAIUE_RIGID_PAIRED_NEON)

// Точка раздела полосы раскраски: граница между манифольдами (runs), ближайшая
// к половине контактов. Левая и правая половины состоят из разных манифольдов,
// а любые два манифольда одной полосы не делят тел, поэтому контакты разных
// половин независимы и решаются парами. Порядок внутри манифольда сохраняется.
static uint32_t SolvePairSplit(const RigidStepScratch *scratch, uint32_t offset, uint32_t begin,
                               uint32_t end)
{
    uint32_t total = 0u;
    for (uint32_t run = begin; run < end; ++run)
    {
        uint32_t first = scratch->solveOrder[offset + run];
        total += ContactRunEnd(scratch, first) - first;
    }
    if (total < 2u)
        return end;
    uint32_t target = (total + 1u) / 2u;
    uint32_t prefix = 0u;
    for (uint32_t run = begin + 1u; run < end; ++run)
    {
        uint32_t previous = scratch->solveOrder[offset + run - 1u];
        prefix += ContactRunEnd(scratch, previous) - previous;
        if (prefix >= target)
            return run;
    }
    return end - 1u;
}

// Курсор по контактам одной половины: перебирает контакты манифольд за
// манифольдом в исходном порядке.
typedef struct RigidSolverCursor
{
    const RigidStepScratch *scratch;
    uint32_t offset;
    uint32_t run;
    uint32_t limit;
    uint32_t index;
    uint32_t last;
} RigidSolverCursor;

static void SolverCursorBegin(RigidSolverCursor *cursor, const RigidStepScratch *scratch,
                              uint32_t offset, uint32_t run, uint32_t limit)
{
    cursor->scratch = scratch;
    cursor->offset = offset;
    cursor->run = run;
    cursor->limit = limit;
    cursor->index = 0u;
    cursor->last = 0u;
    if (run < limit)
    {
        cursor->index = scratch->solveOrder[offset + run];
        cursor->last = ContactRunEnd(scratch, cursor->index);
    }
}

static bool SolverCursorValid(const RigidSolverCursor *cursor)
{
    return cursor->run < cursor->limit;
}

static void SolverCursorAdvance(RigidSolverCursor *cursor)
{
    if (++cursor->index < cursor->last)
        return;
    uint32_t run = cursor->run + 1u;
    while (run < cursor->limit)
    {
        uint32_t index = cursor->scratch->solveOrder[cursor->offset + run];
        uint32_t last = ContactRunEnd(cursor->scratch, index);
        if (index < last)
        {
            cursor->run = run;
            cursor->index = index;
            cursor->last = last;
            return;
        }
        ++run;
    }
    cursor->run = cursor->limit;
    cursor->index = 0u;
    cursor->last = 0u;
}

#endif // LAIUE_RIGID_PAIRED_SSE2 || LAIUE_RIGID_PAIRED_NEON

static void SolveBatchRange(void *context, uint32_t begin, uint32_t end)
{
    RigidJobContext *job = (RigidJobContext *)context;
    RigidStepScratch *scratch = job->scratch;
    VoxelPhysicsConfigureThread();
#if defined(LAIUE_RIGID_PAIRED_SSE2) || defined(LAIUE_RIGID_PAIRED_NEON)
    if (job->pairedRuns)
    {
        const uint32_t split = SolvePairSplit(scratch, job->offset, begin, end);
        RigidSolverCursor left;
        RigidSolverCursor right;
        SolverCursorBegin(&left, scratch, job->offset, begin, split);
        SolverCursorBegin(&right, scratch, job->offset, split, end);
        while (SolverCursorValid(&left) && SolverCursorValid(&right))
        {
            SolveContactPair(scratch, left.index, right.index);
            SolverCursorAdvance(&left);
            SolverCursorAdvance(&right);
        }
        while (SolverCursorValid(&left))
        {
            SolveContact(scratch, left.index);
            SolverCursorAdvance(&left);
        }
        while (SolverCursorValid(&right))
        {
            SolveContact(scratch, right.index);
            SolverCursorAdvance(&right);
        }
        return;
    }
#endif
    // Полоса переполнения и архитектуры без парного пути решаются скалярно
    // в исходном порядке контактов.
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
    RigidJobContext job = {bodies, scratch, settings, 0u, false};
    double begin = ProfileNow(scratch);
    ExecuteRange(scratch, scratch->contactCount, 32u, PrepareContactRange, &job);
    ProfileFinish(scratch, VOXEL_RIGID_PROFILE_PREPARE, begin);
    begin = ProfileNow(scratch);
    // Complete ALL restitution targets before warm-start changes velocities.
    // Cache matching is pair-owned; impulse application remains canonical.
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
            job.pairedRuns = color != RIGID_SOLVER_COLOR_COUNT;
            if (color == RIGID_SOLVER_COLOR_COUNT)
                SolveBatchRange(&job, 0u, count);
            else
                ExecuteRange(scratch, count, RIGID_SOLVER_PARALLEL_GRAIN, SolveBatchRange, &job);
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
    // решатель считал в double, а хранится величина без потолка. У тела без
    // единого контакта решатель к кэшу не прикасался, поэтому кэш — это ровно
    // прочитанная из тела скорость: добавка была бы нулевой, а чтение
    // шести компонент и вычитание — впустую.
    bool velocityChanged = cache->collidable && (cache->hasWorldContact || cache->hasBodyContact);
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (velocityChanged)
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
    // После IntegrateRange весь wakeQueue активных тел обнулён, и ноль здесь
    // читается как «остров готов и не шумит». Прежний отдельный проход,
    // проставлявший бит тишины всем телам, был не нужен: бит готовности
    // ставится только тому острову, где тело ещё не созрело.
    const uint32_t noisyFlag = 1u;
    const uint32_t supportedFlag = 2u;
    bool sleepEnabled = settings->sleepFrames != 0u && settings->sleepLinearSpeed > 0.0 &&
                        settings->sleepAngularSpeed > 0.0;
    bool hasGravity =
        settings->gravity[0] != 0.0 || settings->gravity[1] != 0.0 || settings->gravity[2] != 0.0;
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
            scratch->wakeQueue[root] |= noisyFlag;
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
            scratch->wakeQueue[IslandRoot(scratch, index)] == supportedFlag)
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
    state.grid = (RigidGridEntry *)cursor;
    cursor += (size_t)bodyCount * sizeof(RigidGridEntry);
    state.narrowphasePoints = (RigidNarrowphasePoint *)cursor;
    cursor += (size_t)bodyCount * RIGID_NARROWPHASE_POINTS_PER_BODY * sizeof(RigidNarrowphasePoint);
    state.contacts = (RigidContact *)cursor;
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidContact);
    state.solverContacts = (RigidSolverContact *)cursor;
    cursor += (size_t)bodyCount * RIGID_CONTACTS_PER_BODY * sizeof(RigidSolverContact);
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
    state.narrowVisits = (uint32_t *)cursor;
    cursor += (size_t)bodyCount * sizeof(uint32_t);
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
    RigidJobContext cacheJob = {bodies, &state, settings, 0u, false};
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
