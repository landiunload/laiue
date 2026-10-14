/* Manual CPU skinning benchmark. Each row measures the complete public
 * operation, including matrix preparation and transactional validation.
 * Fixtures, output checksums and reporting stay outside the timed region.
 * Both paths use the same inputs/repetition count; rounds alternate order. */
#include "animation/animation_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ANIMATION_BENCHMARK_ROUNDS
#define ANIMATION_BENCHMARK_ROUNDS 9u
#endif

#define MAX_VERTICES 100000u
#define MAX_JOINTS 128u

static LaiueAnimationSkinVertexV1 g_vertices[MAX_VERTICES];
static LaiueAnimationMatrixV1 g_palette[MAX_JOINTS];
static LaiueAnimationSkinnedVertexV1 g_legacy[MAX_VERTICES];
static LaiueAnimationSkinnedVertexV1 g_cached[MAX_VERTICES];
static double g_scratch[(73u * MAX_JOINTS + 7u) / 8u];
static volatile uint64_t g_checksumSink;

static void Require(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Animation benchmark failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    do
    {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
        text[index] = digits[length - index - 1u];
    text[length] = '\0';
    LaiueTestRuntimeWrite(text);
}

static void Column(uint64_t value)
{
    LaiueTestRuntimeWrite(",");
    WriteUnsigned(value);
}

static uint64_t Checksum(const LaiueAnimationSkinnedVertexV1 *vertices, uint32_t count)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const uint8_t *bytes = (const uint8_t *)vertices;
    for (size_t byte = 0u; byte < (size_t)count * sizeof(*vertices); ++byte)
        hash = (hash ^ bytes[byte]) * UINT64_C(1099511628211);
    g_checksumSink = hash;
    return hash;
}

static void Fixture(uint32_t vertexCount, uint32_t jointCount, uint32_t influences)
{
    memset(g_palette, 0, sizeof(g_palette));
    for (uint32_t joint = 0u; joint < jointCount; ++joint)
    {
        float *matrix = g_palette[joint].m;
        matrix[0] = (joint & 1u ? -1.0f : 1.0f) * (0.75f + (float)(joint % 7u) * 0.125f);
        matrix[1] = (float)(joint % 3u) * 0.0625f;
        matrix[5] = 1.0f + (float)(joint % 5u) * 0.125f;
        matrix[6] = (float)(joint % 4u) * 0.03125f;
        matrix[10] = 0.5f + (float)(joint % 9u) * 0.125f;
        matrix[3] = (float)(joint % 11u) * 0.125f;
        matrix[7] = -(float)(joint % 13u) * 0.0625f;
        matrix[11] = (float)(joint % 17u) * 0.03125f;
    }
    for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex)
    {
        LaiueAnimationSkinVertexV1 *record = &g_vertices[vertex];
        record->position[0] = (float)(vertex % 31u) * 0.03125f;
        record->position[1] = -(float)(vertex % 37u) * 0.015625f;
        record->position[2] = (float)(vertex % 41u) * 0.0078125f;
        record->normal[0] = 0.5f;
        record->normal[1] = -0.25f;
        record->normal[2] = 1.0f;
        for (uint32_t influence = 0u; influence < 4u; ++influence)
        {
            record->joints[influence] = (vertex * 7u + influence * 13u) % jointCount;
            record->weights[influence] = influence < influences ? 1.0f / (float)influences : 0.0f;
        }
    }
}

static uint64_t Run(const LaiueAnimationServiceV1 *service, uint32_t vertexCount,
                    uint32_t jointCount, uint32_t repetitions, bool cached)
{
    const double start = PlatformMonotonicSeconds();
    if (!cached)
    {
        for (uint32_t repeat = 0u; repeat < repetitions; ++repeat)
            Require(service->skinVertices(g_vertices, vertexCount, g_palette, jointCount, g_legacy,
                                          vertexCount) != 0u,
                    "legacy skinning");
    }
#if !defined(ANIMATION_BENCHMARK_LEGACY_ONLY)
    else
    {
        const uint32_t scratchBytes = service->skinningScratchBytes(jointCount);
        for (uint32_t repeat = 0u; repeat < repetitions; ++repeat)
            Require(service->skinVerticesWithScratch(g_vertices, vertexCount, g_palette, jointCount,
                                                     g_legacy, vertexCount, g_scratch,
                                                     scratchBytes) != 0u,
                    "cached skinning");
    }
#endif
    const double elapsed = PlatformMonotonicSeconds() - start;
    return (uint64_t)(elapsed * 1000000000.0 + 0.5);
}

static void Row(const LaiueAnimationServiceV1 *service, uint32_t round, uint32_t vertexCount,
                uint32_t jointCount, uint32_t influences, uint32_t repetitions, bool cached)
{
    const uint64_t elapsed = Run(service, vertexCount, jointCount, repetitions, cached);
    const uint64_t checksum = Checksum(g_legacy, vertexCount);
    LaiueTestRuntimeWrite(cached ? "cached" : "legacy");
    Column(round);
    Column(vertexCount);
    Column(jointCount);
    Column(influences);
    Column(repetitions);
    Column(elapsed);
    Column((elapsed + repetitions / 2u) / repetitions);
    Column(cached ? 73u * jointCount : 0u);
    Column(checksum);
    LaiueTestRuntimeWrite("\n");
}

LAIUE_TEST_ENTRY(AnimationBenchmarkEntryPoint)
{
    const LaiueAnimationServiceV1 *service = LaiueAnimationGetStaticServiceV1();
    Require(service != NULL && service->skinVertices != NULL, "legacy provider");
    bool hasCache = false;
#if !defined(ANIMATION_BENCHMARK_LEGACY_ONLY)
    hasCache = service->structSize >= offsetof(LaiueAnimationServiceV1, skinVerticesWithScratch) +
                                          sizeof(service->skinVerticesWithScratch) &&
               service->skinningScratchBytes != NULL && service->skinVerticesWithScratch != NULL;
    Require(hasCache && service->skinningScratchBytes(MAX_JOINTS) <= sizeof(g_scratch),
            "optional cache callbacks and bounded workspace");
#else
    (void)g_scratch;
#endif
    const uint32_t vertexCounts[] = {1u, 64u, 1000u, 10000u, MAX_VERTICES};
    const uint32_t jointCounts[] = {32u, MAX_JOINTS};
    const uint32_t influenceCounts[] = {1u, 4u};
    LaiueTestRuntimeWrite("path,round,vertices,joints,influences,repetitions,total_ns,ns_per_mesh,"
                          "scratch_bytes,checksum\n");
    for (uint32_t joint = 0u; joint < 2u; ++joint)
        for (uint32_t influences = 0u; influences < 2u; ++influences)
            for (uint32_t count = 0u; count < 5u; ++count)
            {
                const uint32_t vertices = vertexCounts[count];
                const uint32_t joints = jointCounts[joint];
                const uint32_t weights = influenceCounts[influences];
                Fixture(vertices, joints, weights);
                uint32_t repetitions = 1u;
                while (Run(service, vertices, joints, repetitions, false) < UINT64_C(15000000) &&
                       repetitions < 16384u)
                    repetitions *= 2u;
                (void)Run(service, vertices, joints, 1u, false);
                if (hasCache)
                {
                    memcpy(g_cached, g_legacy, (size_t)vertices * sizeof(g_legacy[0]));
                    (void)Run(service, vertices, joints, 1u, true);
                    Require(memcmp(g_legacy, g_cached, (size_t)vertices * sizeof(g_legacy[0])) == 0,
                            "cached and legacy outputs must be bit-identical");
                }
                for (uint32_t round = 0u; round < ANIMATION_BENCHMARK_ROUNDS; ++round)
                {
                    if (hasCache && (round & 1u) != 0u)
                        Row(service, round, vertices, joints, weights, repetitions, true);
                    Row(service, round, vertices, joints, weights, repetitions, false);
                    if (hasCache && (round & 1u) == 0u)
                        Row(service, round, vertices, joints, weights, repetitions, true);
                }
            }
    LAIUE_TEST_SUCCESS();
}
