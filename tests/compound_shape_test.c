// Regression tests for the build-time compound-box coalescer:
//   VoxelRigidCompoundMergeBoxes
//
// The contract is the one documented in src/physics/compound_shape.h. The test
// deliberately exercises argument rejection, non-finite/degenerate geometry,
// overlap rejection, capacity and aliasing rules, transactional failure,
// deterministic repetition, exact in-place operation, positive/negative and
// huge coordinates, the concave G/L cavity, volume conservation and full
// mass/inertia agreement between the original children and the merged shape.
//
// No-CRT builds must not emit a large stack probe, so every buffer is static.

#include "physics/compound_shape.h"
#include "fp_environment_test_support.h"
#include "test_runtime.h"

#define TEST_MAX_BOXES 256u
#define REJECT_SENTINEL_COUNT 4u

static VoxelRigidCompoundBox originalBoxes[TEST_MAX_BOXES];
static VoxelRigidCompoundBox outputBoxes[TEST_MAX_BOXES];
static VoxelRigidCompoundBox secondOutput[TEST_MAX_BOXES];
static VoxelRigidCompoundBox inPlaceBoxes[TEST_MAX_BOXES];
static VoxelRigidCompoundBox backupBoxes[TEST_MAX_BOXES];
static VoxelRigidCompoundBox oversizedBoxes[TEST_MAX_BOXES + 1u];
static VoxelRigidCompoundBox rejectOut[REJECT_SENTINEL_COUNT];
static uint32_t rejectCount;
// A fixture size, not a production shape limit.
static VoxelRigidCompoundBox largeInput[4096];
static VoxelRigidCompoundBox largeOutput[4096];

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite("Compound shape failure: ");
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static union
{
    double scalar;
    uint64_t bits;
} scalarBits;

static uint64_t DoubleBits(double value)
{
    scalarBits.scalar = value;
    return scalarBits.bits;
}

static double NaNValue(void)
{
    scalarBits.bits = UINT64_C(0x7ff8000000000000);
    return scalarBits.scalar;
}

static double InfValue(void)
{
    scalarBits.bits = UINT64_C(0x7ff0000000000000);
    return scalarBits.scalar;
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
}

static bool Near(double value, double expected, double tolerance)
{
    return Absolute(value - expected) <= tolerance * (1.0 + Absolute(expected));
}

static VoxelRigidCompoundBox MakeBox(double cx, double cy, double cz, double hx, double hy,
                                     double hz)
{
    VoxelRigidCompoundBox box;
    box.center[0] = cx;
    box.center[1] = cy;
    box.center[2] = cz;
    box.halfExtent[0] = hx;
    box.halfExtent[1] = hy;
    box.halfExtent[2] = hz;
    return box;
}

static bool SameBox(const VoxelRigidCompoundBox *first, const VoxelRigidCompoundBox *second)
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (DoubleBits(first->center[axis]) != DoubleBits(second->center[axis]) ||
            DoubleBits(first->halfExtent[axis]) != DoubleBits(second->halfExtent[axis]))
        {
            return false;
        }
    }
    return true;
}

static bool SameBoxes(const VoxelRigidCompoundBox *first, const VoxelRigidCompoundBox *second,
                      uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (!SameBox(&first[index], &second[index]))
        {
            return false;
        }
    }
    return true;
}

static double BoxVolume(const VoxelRigidCompoundBox *box)
{
    return 8.0 * box->halfExtent[0] * box->halfExtent[1] * box->halfExtent[2];
}

static double TotalVolume(const VoxelRigidCompoundBox *boxes, uint32_t count)
{
    double volume = 0.0;
    for (uint32_t index = 0u; index < count; ++index)
    {
        volume += BoxVolume(&boxes[index]);
    }
    return volume;
}

static bool BoxContains(const VoxelRigidCompoundBox *box, double x, double y, double z)
{
    double coordinates[3] = {x, y, z};
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double low = box->center[axis] - box->halfExtent[axis];
        double high = box->center[axis] + box->halfExtent[axis];
        if (!(coordinates[axis] >= low) || !(coordinates[axis] <= high))
        {
            return false;
        }
    }
    return true;
}

static bool UnionContains(const VoxelRigidCompoundBox *boxes, uint32_t count, double x, double y,
                          double z)
{
    for (uint32_t index = 0u; index < count; ++index)
    {
        if (BoxContains(&boxes[index], x, y, z))
        {
            return true;
        }
    }
    return false;
}

// Exact occupancy comparison over a sample grid that never lands on the
// half-integer box faces: boundary points have zero measure, and the union
// must be identical for every interior/exterior sample.
static void ExpectSameOccupancy(const VoxelRigidCompoundBox *first, uint32_t firstCount,
                                const VoxelRigidCompoundBox *second, uint32_t secondCount,
                                const char *message)
{
    double low[3];
    double high[3];
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        low[axis] = first[0].center[axis] - first[0].halfExtent[axis];
        high[axis] = first[0].center[axis] + first[0].halfExtent[axis];
    }
    for (uint32_t index = 1u; index < firstCount; ++index)
    {
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            double candidateLow = first[index].center[axis] - first[index].halfExtent[axis];
            double candidateHigh = first[index].center[axis] + first[index].halfExtent[axis];
            if (candidateLow < low[axis])
            {
                low[axis] = candidateLow;
            }
            if (candidateHigh > high[axis])
            {
                high[axis] = candidateHigh;
            }
        }
    }
    for (uint32_t x = 0u; x < 64u; ++x)
    {
        double px = low[0] - 0.25 + 0.5 * (double)x;
        if (px > high[0] + 0.25)
        {
            break;
        }
        for (uint32_t y = 0u; y < 64u; ++y)
        {
            double py = low[1] - 0.25 + 0.5 * (double)y;
            if (py > high[1] + 0.25)
            {
                break;
            }
            for (uint32_t z = 0u; z < 64u; ++z)
            {
                double pz = low[2] - 0.25 + 0.5 * (double)z;
                if (pz > high[2] + 0.25)
                {
                    break;
                }
                bool firstInside = UnionContains(first, firstCount, px, py, pz);
                bool secondInside = UnionContains(second, secondCount, px, py, pz);
                Expect(firstInside == secondInside, message);
            }
        }
    }
}

static uint32_t BuildL9(VoxelRigidCompoundBox *boxes)
{
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        boxes[index] = MakeBox(0.0, 0.0, (double)index, 0.5, 0.5, 0.5);
    }
    for (uint32_t index = 1u; index < 5u; ++index)
    {
        boxes[4u + index] = MakeBox((double)index, 0.0, 0.0, 0.5, 0.5, 0.5);
    }
    return 9u;
}

static uint32_t BuildPlate(VoxelRigidCompoundBox *boxes)
{
    uint32_t count = 0u;
    for (uint32_t y = 0u; y < 4u; ++y)
    {
        for (uint32_t x = 0u; x < 4u; ++x)
        {
            boxes[count++] = MakeBox((double)x, (double)y, 0.0, 0.5, 0.5, 0.5);
        }
    }
    return count;
}

static uint32_t BuildStaircase(VoxelRigidCompoundBox *boxes)
{
    uint32_t count = 0u;
    for (uint32_t step = 0u; step < 3u; ++step)
    {
        boxes[count++] = MakeBox((double)step, 0.0, (double)step, 0.5, 0.5, 0.5);
        boxes[count++] = MakeBox((double)step + 1.0, 0.0, (double)step, 0.5, 0.5, 0.5);
    }
    return count;
}

static void PrepareSentinel(void)
{
    for (uint32_t index = 0u; index < REJECT_SENTINEL_COUNT; ++index)
    {
        rejectOut[index] = MakeBox(7.0, 7.0, 7.0, 1.0, 1.0, 1.0);
    }
    rejectCount = 0xABCDEF01u;
}

static bool SentinelsIntact(void)
{
    VoxelRigidCompoundBox sentinel = MakeBox(7.0, 7.0, 7.0, 1.0, 1.0, 1.0);
    for (uint32_t index = 0u; index < REJECT_SENTINEL_COUNT; ++index)
    {
        if (!SameBox(&rejectOut[index], &sentinel))
        {
            return false;
        }
    }
    return rejectCount == 0xABCDEF01u;
}

#define EXPECT_REJECTED(call_, message_)                                                          \
    do                                                                                            \
    {                                                                                             \
        PrepareSentinel();                                                                        \
        Expect(!(call_), (message_));                                                             \
        Expect(SentinelsIntact(), "rejected call leaves output unchanged");                       \
    } while (0)

static void TestRejectArguments(void)
{
    VoxelRigidCompoundBox valid[4];
    valid[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    valid[1] = MakeBox(3.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    valid[2] = MakeBox(6.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    valid[3] = MakeBox(9.0, 0.0, 0.0, 0.5, 0.5, 0.5);

    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(NULL, 4u, rejectOut, 4u, &rejectCount),
                    "null input rejected");
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(valid, 4u, NULL, 4u, &rejectCount),
                    "null output rejected");
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(valid, 4u, rejectOut, 4u, NULL),
                    "null outCount rejected");
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(valid, 0u, rejectOut, 4u, &rejectCount),
                    "zero count rejected");
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(oversizedBoxes, TEST_MAX_BOXES + 1u, rejectOut,
                                                 REJECT_SENTINEL_COUNT, &rejectCount),
                    "large input with insufficient output capacity rejected");
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(valid, 4u, rejectOut, 3u, &rejectCount),
                    "capacity below count rejected");
}

static void TestRejectInvalidBoxes(void)
{
    VoxelRigidCompoundBox bad[2];
    bad[1] = MakeBox(3.0, 0.0, 0.0, 0.5, 0.5, 0.5);

    bad[0] = MakeBox(NaNValue(), 0.0, 0.0, 0.5, 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(bad, 2u, rejectOut, 4u, &rejectCount),
                    "NaN centre rejected");
    bad[0] = MakeBox(InfValue(), 0.0, 0.0, 0.5, 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(bad, 2u, rejectOut, 4u, &rejectCount),
                    "infinite centre rejected");
    bad[0] = MakeBox(0.0, 0.0, 0.0, NaNValue(), 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(bad, 2u, rejectOut, 4u, &rejectCount),
                    "NaN half extent rejected");
    bad[0] = MakeBox(0.0, 0.0, 0.0, InfValue(), 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(bad, 2u, rejectOut, 4u, &rejectCount),
                    "infinite half extent rejected");
    bad[0] = MakeBox(0.0, 0.0, 0.0, 0.0, 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(bad, 2u, rejectOut, 4u, &rejectCount),
                    "zero half extent rejected");
    bad[0] = MakeBox(0.0, 0.0, 0.0, -0.5, 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(bad, 2u, rejectOut, 4u, &rejectCount),
                    "negative half extent rejected");
    // At 2^53 the spacing is 2.0, so +-0.5 collapses to the same double.
    bad[0] = MakeBox(9007199254740992.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(bad, 2u, rejectOut, 4u, &rejectCount),
                    "collapsed finite bounds rejected");
    bad[0] = MakeBox(1e308, 0.0, 0.0, 1e308, 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(bad, 2u, rejectOut, 4u, &rejectCount),
                    "overflowing finite bounds rejected");
}

static void TestOverlapAndTouching(void)
{
    VoxelRigidCompoundBox overlap[2];
    overlap[0] = MakeBox(0.0, 0.0, 0.0, 0.6, 0.5, 0.5);
    overlap[1] = MakeBox(0.5, 0.0, 0.0, 0.6, 0.5, 0.5);
    EXPECT_REJECTED(VoxelRigidCompoundMergeBoxes(overlap, 2u, rejectOut, 4u, &rejectCount),
                    "strictly overlapping input rejected");

    VoxelRigidCompoundBox touching[2];
    touching[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    touching[1] = MakeBox(1.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    uint32_t count = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(touching, 2u, outputBoxes, 2u, &count),
           "face-touching input accepted");
    Expect(count == 1u, "face-touching pair merges");
    VoxelRigidCompoundBox expected = MakeBox(0.5, 0.0, 0.0, 1.0, 0.5, 0.5);
    Expect(SameBox(&outputBoxes[0], &expected), "merged pair has exact united bounds");
}

static void TestCapacityAndAliasing(void)
{
    uint32_t count = BuildL9(originalBoxes);

    // Spare capacity above count must be accepted.
    uint32_t resultCount = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count + 1u, &resultCount),
           "spare capacity accepted");
    Expect(resultCount == 2u, "spare capacity keeps the merged count");

    // Partially overlapping input/output ranges are refused without writing.
    for (uint32_t index = 0u; index < count; ++index)
    {
        backupBoxes[index] = originalBoxes[index];
    }
    Expect(!VoxelRigidCompoundMergeBoxes(originalBoxes, count, originalBoxes + 1u, count,
                                         &resultCount),
           "partially overlapping output range rejected");
    Expect(SameBoxes(originalBoxes, backupBoxes, count),
           "partial overlap rejection leaves the input intact");

    // outCount must not alias the output or the input buffer.
    Expect(!VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count,
                                         (uint32_t *)outputBoxes),
           "outCount aliasing the output buffer rejected");
    Expect(!VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count,
                                         (uint32_t *)originalBoxes),
           "outCount aliasing the input buffer rejected");
    Expect(SameBoxes(originalBoxes, backupBoxes, count),
           "alias rejection leaves the input intact");
}

static void TestNineBlocksToTwo(void)
{
    uint32_t count = BuildL9(originalBoxes);
    uint32_t merged = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count, &merged),
           "G/L shape merged");
    Expect(merged == 2u, "nine G blocks collapse to exactly two boxes");
    ExpectSameOccupancy(originalBoxes, count, outputBoxes, merged,
                        "merged G preserves the exact occupancy");
    Expect(Near(TotalVolume(outputBoxes, merged), TotalVolume(originalBoxes, count), 1e-12),
           "merged G preserves the total volume");
    // The concave cavity between the two limbs must stay empty.
    Expect(!UnionContains(outputBoxes, merged, 2.0, 0.0, 2.0), "G cavity stays empty");
    Expect(!UnionContains(outputBoxes, merged, 3.5, 0.0, 3.5), "outer G corner stays empty");
    Expect(UnionContains(outputBoxes, merged, 0.0, 0.0, 2.0), "G vertical limb is solid");
    Expect(UnionContains(outputBoxes, merged, 3.0, 0.0, 0.0), "G horizontal limb is solid");
}

static void TestPlateToSingle(void)
{
    uint32_t count = BuildPlate(originalBoxes);
    Expect(count == 16u, "plate fixture has sixteen blocks");
    uint32_t merged = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count, &merged),
           "plate merged");
    Expect(merged == 1u, "sixteen plate blocks collapse to one box");
    VoxelRigidCompoundBox expected = MakeBox(1.5, 1.5, 0.0, 2.0, 2.0, 0.5);
    Expect(SameBox(&outputBoxes[0], &expected), "plate becomes the exact enclosing box");
    Expect(Near(TotalVolume(outputBoxes, merged), TotalVolume(originalBoxes, count), 1e-12),
           "plate keeps its volume");
    ExpectSameOccupancy(originalBoxes, count, outputBoxes, merged,
                        "merged plate preserves the exact occupancy");
}

static void TestZigzagStaysConcave(void)
{
    uint32_t count = BuildStaircase(originalBoxes);
    uint32_t merged = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count, &merged),
           "zigzag merged");
    Expect(merged == 3u, "connected zigzag keeps its three steps");
    ExpectSameOccupancy(originalBoxes, count, outputBoxes, merged,
                        "zigzag preserves the exact occupancy");
    Expect(!UnionContains(outputBoxes, merged, 0.0, 0.0, 1.5),
           "zigzag concave notch stays empty");
}

static void TestNoMergeKeepsLayout(void)
{
    VoxelRigidCompoundBox single[1];
    single[0] = MakeBox(-3.0, 2.0, 1.0, 0.5, 0.5, 0.5);
    uint32_t singleCount = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(single, 1u, outputBoxes, 1u, &singleCount),
           "single box accepted");
    Expect(singleCount == 1u, "single box keeps its count");
    Expect(SameBox(&single[0], &outputBoxes[0]), "single box is unchanged");

    VoxelRigidCompoundBox boxes[2];
    boxes[0] = MakeBox(0.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    boxes[1] = MakeBox(5.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    uint32_t merged = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(boxes, 2u, outputBoxes, 2u, &merged),
           "separated pair accepted");
    Expect(merged == 2u, "separated pair is not merged");
    Expect(SameBoxes(boxes, outputBoxes, 2u), "no-merge output equals the input layout bit for bit");
}

static void TestDeterministicRepeat(void)
{
    uint32_t count = BuildL9(originalBoxes);
    uint32_t firstCount = 0u;
    uint32_t secondCount = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count, &firstCount),
           "first deterministic merge");
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, secondOutput, count, &secondCount),
           "second deterministic merge");
    Expect(firstCount == secondCount, "deterministic merge keeps the same count");
    Expect(SameBoxes(outputBoxes, secondOutput, firstCount),
           "deterministic merge repeats every box bit for bit");
}

static void TestInPlaceMatchesOutOfPlace(void)
{
    uint32_t count = BuildL9(originalBoxes);
    uint32_t outOfPlaceCount = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count, &outOfPlaceCount),
           "out-of-place merge");
    for (uint32_t index = 0u; index < count; ++index)
    {
        inPlaceBoxes[index] = originalBoxes[index];
    }
    uint32_t inPlaceCount = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(inPlaceBoxes, count, inPlaceBoxes, count, &inPlaceCount),
           "exact in-place merge");
    Expect(inPlaceCount == outOfPlaceCount, "in-place merge keeps the same count");
    Expect(SameBoxes(inPlaceBoxes, outputBoxes, inPlaceCount),
           "in-place merge repeats the out-of-place result bit for bit");
}

static void TestPositiveNegativeCoordinates(void)
{
    uint32_t count = BuildL9(originalBoxes);
    // Shift across the origin so both positive and negative coordinates appear.
    for (uint32_t index = 0u; index < count; ++index)
    {
        originalBoxes[index].center[0] -= 2.0;
        originalBoxes[index].center[2] -= 2.0;
    }
    uint32_t merged = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count, &merged),
           "translated G/L merged");
    Expect(merged == 2u, "translated G/L still collapses to two boxes");
    ExpectSameOccupancy(originalBoxes, count, outputBoxes, merged,
                        "translated G/L preserves the exact occupancy");
    Expect(Near(TotalVolume(outputBoxes, merged), TotalVolume(originalBoxes, count), 1e-12),
           "translated G/L keeps its volume");
}

static void TestLargeCoordinateRepresentability(void)
{
    // 2^51 has spacing 0.5, so a unit-wide merge reconstructs exactly.
    VoxelRigidCompoundBox mergeable[2];
    mergeable[0] = MakeBox(2251799813685248.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    mergeable[1] = MakeBox(2251799813685249.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    uint32_t merged = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(mergeable, 2u, outputBoxes, 2u, &merged),
           "large exact pair accepted");
    Expect(merged == 1u, "large exactly representable pair merges");
    VoxelRigidCompoundBox expected = MakeBox(2251799813685248.5, 0.0, 0.0, 1.0, 0.5, 0.5);
    Expect(SameBox(&outputBoxes[0], &expected), "large merged bounds reconstruct exactly");

    // 2^53 has spacing 2.0: the naive midpoint rounds and cannot reconstruct
    // the united faces, so the merge must be skipped rather than lose an end.
    VoxelRigidCompoundBox unrepresentable[2];
    unrepresentable[0] = MakeBox(9007199254740992.0, 0.0, 0.0, 1.0, 0.5, 0.5);
    unrepresentable[1] = MakeBox(9007199254740994.0, 0.0, 0.0, 2.0, 0.5, 0.5);
    uint32_t skipped = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(unrepresentable, 2u, outputBoxes, 2u, &skipped),
           "large unrepresentable pair accepted without merge");
    Expect(skipped == 2u, "unrepresentable merge is skipped");
    Expect(SameBoxes(outputBoxes, unrepresentable, 2u),
           "skipped merge keeps the exact original layout");
}

static void TestMassPropertiesPreserved(void)
{
    uint32_t count = BuildL9(originalBoxes);
    uint32_t merged = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count, &merged),
           "mass properties fixture merged");
    Expect(merged == 2u, "mass properties fixture collapses to two boxes");

    double originalCenter[3];
    double originalHalf[3];
    double originalInverse[9];
    double mergedCenter[3];
    double mergedHalf[3];
    double mergedInverse[9];
    Expect(VoxelRigidCompoundMassProperties(originalBoxes, count, 9.0, originalCenter, originalHalf,
                                            originalInverse),
           "original mass properties resolved");
    Expect(VoxelRigidCompoundMassProperties(outputBoxes, merged, 9.0, mergedCenter, mergedHalf,
                                            mergedInverse),
           "merged mass properties resolved");

    Expect(Near(originalCenter[0], 10.0 / 9.0, 1e-12) && Near(originalCenter[1], 0.0, 1e-12) &&
               Near(originalCenter[2], 10.0 / 9.0, 1e-12),
           "original G/L centre of mass is analytic");
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        Expect(Near(mergedCenter[axis], originalCenter[axis], 1e-12),
               "merged centre of mass matches the children");
        Expect(Near(mergedHalf[axis], originalHalf[axis], 1e-12),
               "merged envelope matches the children");
    }
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        Expect(Near(mergedInverse[index], originalInverse[index], 1e-9),
               "merged inverse inertia matches the children");
    }
}

static void TestHostileFpDeterminism(void)
{
    uint32_t count = BuildL9(originalBoxes);
    uint32_t normalCount = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, outputBoxes, count, &normalCount),
           "normal FP merge");
    LaiueTestSetHostileFpEnvironment();
    uint32_t hostileCount = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(originalBoxes, count, secondOutput, count, &hostileCount),
           "hostile FP merge");
    Expect(VoxelPhysicsThreadIsConfigured(), "merge normalizes the FP environment");
    Expect(normalCount == hostileCount, "hostile FP keeps the merge count");
    Expect(SameBoxes(outputBoxes, secondOutput, normalCount),
           "hostile FP keeps every merged box bit for bit");
}

static void TestLargeShapeWithoutLegacyCap(void)
{
    for (uint32_t index = 0u; index < 4096u; ++index)
        largeInput[index] = MakeBox((double)index + 0.5, 0.5, 0.5, 0.5, 0.5, 0.5);
    double center[3], half[3], inverse[9];
    Expect(VoxelRigidCompoundMassProperties(largeInput, 4096u, 4096.0, center, half, inverse),
           "4096-child mass has no former 256-child limit");
    Expect(center[0] == 2048.0 && center[1] == 0.5 && center[2] == 0.5,
           "4096-child exact center");
    Expect(half[0] == 2048.0 && half[1] == 0.5 && half[2] == 0.5,
           "4096-child exact envelope");
    uint32_t count = 0u;
    Expect(VoxelRigidCompoundMergeBoxes(largeInput, 4096u, largeOutput, 4096u, &count),
           "4096-child generic merge");
    Expect(count == 1u && largeOutput[0].center[0] == 2048.0 &&
               largeOutput[0].halfExtent[0] == 2048.0,
           "4096-child merge keeps exact union");
}

LAIUE_TEST_ENTRY(CompoundShapeTestEntryPoint)
{
    for (uint32_t index = 0u; index < TEST_MAX_BOXES + 1u; ++index)
    {
        oversizedBoxes[index] = MakeBox((double)index * 2.0, 0.0, 0.0, 0.5, 0.5, 0.5);
    }
    TestRejectArguments();
    TestLargeShapeWithoutLegacyCap();
    TestRejectInvalidBoxes();
    TestOverlapAndTouching();
    TestCapacityAndAliasing();
    TestNineBlocksToTwo();
    TestPlateToSingle();
    TestZigzagStaysConcave();
    TestNoMergeKeepsLayout();
    TestDeterministicRepeat();
    TestInPlaceMatchesOutOfPlace();
    TestPositiveNegativeCoordinates();
    TestLargeCoordinateRepresentability();
    TestMassPropertiesPreserved();
    TestHostileFpDeterminism();
    LaiueTestRuntimeWrite("compound-shape-merge: ok\n");
    LAIUE_TEST_SUCCESS();
}
