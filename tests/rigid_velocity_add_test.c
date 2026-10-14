// Добавка скорости к телу обязана давать ровно тот же bigint, что и общий
// путь: v + trunc(delta * 2^32), собранное из целой части, сдвига и
// дробного остатка. Быстрый путь меняет однолимбовую компоненту на месте,
// поэтому здесь проверяются именно его границы: перенос в новый лимб,
// точное погашение, канонический ноль, многолимбовые значения, дельты
// меньше 2^-32 и вне диапазона int64, а также атомарность набора из трёх
// компонент и пробуждение тела.

#include "physics/rigid_body.h"
#include "physics/numeric_provider.h"
#include "numeric/infinite_coord.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static void Expect(bool condition, const char *name)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Velocity add check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static InfiniteCoord FromInt64(int64_t value)
{
    InfiniteCoord zero;
    InfiniteCoord result;
    InfiniteCoordInit(&zero);
    InfiniteCoordInit(&result);
    Expect(InfiniteCoordTryCopyAddInt64(&result, &zero, value), "int64 value builds");
    return result;
}

// sign * (high * 2^64 + low) без опоры на проверяемый код physics.
static InfiniteCoord FromParts(int32_t sign, uint64_t high, uint64_t low)
{
    InfiniteCoord result = FromInt64(0);
    if (high != 0u)
    {
        InfiniteCoord highPart = FromInt64((int64_t)(high >> 1));
        InfiniteCoord shifted;
        InfiniteCoordInit(&shifted);
        Expect(InfiniteCoordTryCopyShiftLeft(&shifted, &highPart, 65u), "high part shifts");
        InfiniteCoordDestroy(&highPart);
        if ((high & 1u) != 0u)
        {
            InfiniteCoord one = FromInt64(1);
            InfiniteCoord oneShifted;
            InfiniteCoordInit(&oneShifted);
            Expect(InfiniteCoordTryCopyShiftLeft(&oneShifted, &one, 64u), "low bit of high shifts");
            InfiniteCoord sum;
            InfiniteCoordInit(&sum);
            Expect(InfiniteCoordTryAdd(&sum, &shifted, &oneShifted), "high parts add");
            InfiniteCoordDestroy(&one);
            InfiniteCoordDestroy(&oneShifted);
            InfiniteCoordDestroy(&shifted);
            shifted = sum;
        }
        InfiniteCoordDestroy(&result);
        result = shifted;
    }
    // low = 2 * (low >> 1) + (low & 1), чтобы каждая добавка помещалась в int64.
    Expect(InfiniteCoordTryAddInt64InPlace(&result, (int64_t)(low >> 1)) &&
               InfiniteCoordTryAddInt64InPlace(&result, (int64_t)(low >> 1)) &&
               InfiniteCoordTryAddInt64InPlace(&result, (int64_t)(low & 1u)),
           "low part adds");
    if (sign < 0)
    {
        InfiniteCoord negated;
        InfiniteCoordInit(&negated);
        Expect(InfiniteCoordTryCopyNegate(&negated, &result), "value negates");
        InfiniteCoordDestroy(&result);
        result = negated;
    }
    return result;
}

// Эталон: прежний общий путь, целиком на арифметике numeric.
static InfiniteCoord Reference(const InfiniteCoord *value, double delta)
{
    InfiniteCoord whole;
    InfiniteCoordInit(&whole);
    Expect(InfiniteCoordTrySetFromDouble(&whole, delta), "reference integer part");
    InfiniteCoord scaled;
    InfiniteCoordInit(&scaled);
    Expect(InfiniteCoordTryCopyShiftLeft(&scaled, &whole, 32u), "reference shift");
    InfiniteCoordDestroy(&whole);
    double fraction = 0.0;
    if (delta > -9007199254740992.0 && delta < 9007199254740992.0)
        fraction = delta - (double)(int64_t)delta;
    InfiniteCoord sum;
    InfiniteCoordInit(&sum);
    Expect(InfiniteCoordTryAdd(&sum, value, &scaled), "reference sum");
    InfiniteCoordDestroy(&scaled);
    InfiniteCoord total;
    InfiniteCoordInit(&total);
    Expect(InfiniteCoordTryCopyAddInt64(&total, &sum, (int64_t)(fraction * 4294967296.0)),
           "reference fraction");
    InfiniteCoordDestroy(&sum);
    return total;
}

static bool SameValue(const InfiniteCoord *left, const InfiniteCoord *right)
{
    if (left->sign != right->sign || left->limbCount != right->limbCount)
        return false;
    for (uint32_t limb = 0u; limb < left->limbCount; ++limb)
        if (left->limbs[limb] != right->limbs[limb])
            return false;
    return InfiniteCoordCompare(left, right) == 0;
}

static VoxelRigidBody MakeBody(void)
{
    VoxelRigidBody body;
    const VoxelRigidBodyDescription description = {
        .halfExtent = {0.5, 0.5, 0.5},
        .position = {0.0, 0.0, 0.0},
        .mass = 1.0,
        .restitution = 0.0,
        .friction = 0.5,
    };
    Expect(VoxelRigidBodyInitialize(&body, 1u, &description), "body initializes");
    return body;
}

static uint32_t checkedCases;

static void CheckCase(const InfiniteCoord *initial, double delta, bool angular)
{
    VoxelRigidBody body = MakeBody();
    InfiniteCoord *velocity = angular ? body.angularVelocity : body.linearVelocity;
    // Ось 1 получает проверяемую пару, оси 0 и 2 — однолимбовые значения со
    // своими добавками: набор из трёх компонент проверяется целиком, а
    // быстрый путь включается, только если его допускает и ось 1.
    const double deltas[3] = {0.25, delta, -0.125};
    InfiniteCoord expected[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        InfiniteCoordDestroy(&velocity[axis]);
        if (axis == 1u)
        {
            InfiniteCoordInit(&velocity[axis]);
            Expect(InfiniteCoordTryCopyAddInt64(&velocity[axis], initial, 0),
                   "initial value copies");
        }
        else
        {
            velocity[axis] = FromInt64(axis == 0u ? 7777 : -15554);
        }
        expected[axis] = Reference(&velocity[axis], deltas[axis]);
    }
    body.sleeping = true;
    body.sleepCounter = 9u;
    const bool added = angular ? VoxelRigidBodyAddAngularVelocity(&body, deltas)
                               : VoxelRigidBodyAddLinearVelocity(&body, deltas);
    Expect(added, "finite velocity delta is applied");
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        Expect(SameValue(&velocity[axis], &expected[axis]),
               "velocity equals v + trunc(delta * 2^32) in canonical form");
        InfiniteCoordDestroy(&expected[axis]);
    }
    Expect(!body.sleeping && body.sleepCounter == 0u, "non-zero delta wakes the body");
    VoxelRigidBodyRelease(&body);
    ++checkedCases;
}

static void CheckAtomicRefusal(void)
{
    VoxelRigidBody body = MakeBody();
    InfiniteCoordDestroy(&body.linearVelocity[0]);
    body.linearVelocity[0] = FromInt64(123456789);
    body.sleeping = true;
    volatile double zeroDivisor = 0.0;
    const double invalid[3] = {1.0, zeroDivisor / zeroDivisor, 2.0};
    Expect(!VoxelRigidBodyAddLinearVelocity(&body, invalid), "non-finite delta is refused");
    InfiniteCoord expected = FromInt64(123456789);
    Expect(SameValue(&body.linearVelocity[0], &expected) && body.linearVelocity[1].sign == 0 &&
               body.linearVelocity[2].sign == 0,
           "refused set leaves every component unchanged");
    Expect(body.sleeping, "refused set does not wake the body");
    InfiniteCoordDestroy(&expected);

    const double zero[3] = {0.0, 0.0, 0.0};
    Expect(VoxelRigidBodyAddAngularVelocity(&body, zero), "zero delta is accepted");
    Expect(body.sleeping, "zero delta does not wake the body");
    VoxelRigidBodyRelease(&body);
}

static uint64_t NextRandom(uint64_t *state)
{
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return *state;
}

LAIUE_TEST_ENTRY(RigidVelocityAddTestEntryPoint)
{
    PhysicsSetNumericService(LaiueNumericGetStaticServiceV1());

    const struct
    {
        int32_t sign;
        uint64_t high;
        uint64_t low;
    } initials[] = {
        {0, 0u, 0u},
        {1, 0u, 1u},
        {-1, 0u, 1u},
        {1, 0u, 12345u},
        {-1, 0u, 12345u},
        {1, 0u, (uint64_t)INT64_MAX},
        {-1, 0u, (uint64_t)1 << 63},
        {1, 0u, UINT64_MAX},
        {-1, 0u, UINT64_MAX},
        {1, 0u, UINT64_MAX - 3u},
        {1, 1u, 0u},
        {-1, 1u, 0u},
        {1, 1u, 5u},
        {-1, 1u, 5u},
        {1, 3u, UINT64_MAX},
    };
    const double deltas[] = {
        0.0,
        1.0e-12,
        -1.0e-12,
        0x1p-32,
        -0x1p-32,
        0.5,
        -0.5,
        1.0 / 3.0,
        -2.75,
        12345.0 / 4294967296.0,
        -12345.0 / 4294967296.0,
        1.0 / 4294967296.0,
        1.0e6 + 0.123,
        -1.0e9 - 0.77,
        2147483647.999,
        -2147483648.0,
        2147483648.0,
        -2147483648.5,
        1.0e15 + 0.5,
        -1.0e15,
        3.0e18,
        -1.0e300,
    };
    for (uint32_t initial = 0u; initial < sizeof(initials) / sizeof(initials[0]); ++initial)
    {
        InfiniteCoord value =
            FromParts(initials[initial].sign, initials[initial].high, initials[initial].low);
        for (uint32_t delta = 0u; delta < sizeof(deltas) / sizeof(deltas[0]); ++delta)
        {
            if (deltas[delta] == 0.0)
                continue;
            CheckCase(&value, deltas[delta], false);
            CheckCase(&value, deltas[delta], true);
        }
        InfiniteCoordDestroy(&value);
    }

    // Случайные однолимбовые значения и дельты разного порядка, включая
    // точное погашение: delta = -v * 2^-32 представима, если |v| < 2^53.
    uint64_t state = 0x5eed1234abcd0001ull;
    for (uint32_t round = 0u; round < 4000u; ++round)
    {
        int64_t raw = (int64_t)NextRandom(&state);
        int64_t initialFixed = raw >> (NextRandom(&state) % 63u);
        double magnitude = (double)(NextRandom(&state) >> 11) * 0x1p-53;
        int32_t exponent = (int32_t)(NextRandom(&state) % 80u) - 40;
        double delta = magnitude;
        for (int32_t step = 0; step < exponent; ++step)
            delta *= 2.0;
        for (int32_t step = 0; step > exponent; --step)
            delta *= 0.5;
        if ((NextRandom(&state) & 1u) != 0u)
            delta = -delta;
        if ((round & 15u) == 0u)
        {
            initialFixed >>= 11;
            delta = -(double)initialFixed * 0x1p-32;
        }
        if (delta == 0.0)
            continue;
        InfiniteCoord value = FromInt64(initialFixed);
        CheckCase(&value, delta, (round & 1u) != 0u);
        InfiniteCoordDestroy(&value);
    }

    CheckAtomicRefusal();
    Expect(checkedCases > 4000u, "every case was exercised");
    LaiueTestRuntimeWrite("Rigid velocity add checks passed\r\n");
    LAIUE_TEST_SUCCESS();
}
