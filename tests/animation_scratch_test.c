#include "animation/animation_service.h"
#include "test_runtime.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEST_VERTICES 128u
#define TEST_JOINTS LAIUE_ANIMATION_MAX_SKIN_JOINTS
#define SCRATCH_BYTES (73u * TEST_JOINTS)
#define LEGACY_TABLE_BYTES (8u + 21u * sizeof(uintptr_t))

static _Alignas(8) LaiueAnimationMatrixV1 g_palette[TEST_JOINTS];
static _Alignas(8) LaiueAnimationSkinVertexV1 g_vertices[TEST_VERTICES];
static LaiueAnimationSkinnedVertexV1 g_legacy[TEST_VERTICES];
static _Alignas(8) LaiueAnimationSkinnedVertexV1 g_cached[TEST_VERTICES];
static LaiueAnimationSkinnedVertexV1 g_saved[TEST_VERTICES];
static double g_scratch[(SCRATCH_BYTES + 7u) / 8u + 1u];
static double g_savedScratch[(SCRATCH_BYTES + 7u) / 8u + 1u];

_Static_assert(offsetof(LaiueAnimationServiceV1, skinningScratchBytes) == LEGACY_TABLE_BYTES,
               "the original animation service prefix is unchanged");
_Static_assert(sizeof(g_scratch) > SCRATCH_BYTES, "the workspace has a canary tail");

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Animation scratch: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool HasScratch(const LaiueAnimationServiceV1 *service)
{
    return service != NULL &&
           service->structSize >= offsetof(LaiueAnimationServiceV1, skinVerticesWithScratch) +
                                      sizeof(service->skinVerticesWithScratch) &&
           service->skinningScratchBytes != NULL && service->skinVerticesWithScratch != NULL;
}

static void Fixture(uint32_t jointCount, uint32_t influences)
{
    memset(g_palette, 0, sizeof(g_palette));
    memset(g_vertices, 0, sizeof(g_vertices));
    for (uint32_t joint = 0u; joint < jointCount; ++joint)
    {
        float *m = g_palette[joint].m;
        /* A signed nonuniform scale followed by a 90-degree rotation,
         * translation and shear: normal transforms cannot use position TRS. */
        m[1] = (joint & 1u ? -1.0f : 1.0f) * (0.75f + (float)(joint % 5u) * 0.25f);
        m[4] = -(0.5f + (float)(joint % 7u) * 0.125f);
        m[6] = (float)(joint % 3u) * 0.0625f;
        m[10] = 1.25f + (float)(joint % 4u) * 0.125f;
        m[3] = (float)(joint % 9u) * 0.25f;
        m[7] = -(float)(joint % 11u) * 0.125f;
        m[11] = (float)(joint % 13u) * 0.0625f;
    }
    for (uint32_t vertex = 0u; vertex < TEST_VERTICES; ++vertex)
    {
        g_vertices[vertex].position[0] = (float)(vertex % 17u) * 0.125f;
        g_vertices[vertex].position[1] = -(float)(vertex % 19u) * 0.0625f;
        g_vertices[vertex].position[2] = (float)(vertex % 23u) * 0.03125f;
        if (vertex % 11u != 0u)
        {
            g_vertices[vertex].normal[0] = 1.0f;
            g_vertices[vertex].normal[1] = -0.5f;
            g_vertices[vertex].normal[2] = 0.25f;
        }
        for (uint32_t influence = 0u; influence < 4u; ++influence)
        {
            g_vertices[vertex].joints[influence] = (vertex * 7u + influence * 13u) % jointCount;
            g_vertices[vertex].weights[influence] =
                influence < influences ? 1.0f / (float)influences : 0.0f;
        }
    }
}

static void Compare(const LaiueAnimationServiceV1 *service, uint32_t jointCount)
{
    const uint32_t bytes = service->skinningScratchBytes(jointCount);
    memset(g_scratch, 0xa5, sizeof(g_scratch));
    Expect(service->skinVertices(g_vertices, TEST_VERTICES, g_palette, jointCount, g_legacy,
                                 TEST_VERTICES) != 0u,
           "legacy fixture succeeds");
    Expect(service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, jointCount,
                                            g_cached, TEST_VERTICES, g_scratch, bytes) != 0u,
           "cached fixture succeeds with the exact queried size");
    Expect(memcmp(g_legacy, g_cached, sizeof(g_cached)) == 0,
           "signed nonuniform transforms, weights and zero normals are bit-identical");
    const uint8_t *tail = (const uint8_t *)g_scratch;
    for (size_t byte = bytes; byte < sizeof(g_scratch); ++byte)
        Expect(tail[byte] == 0xa5u, "the exact-size scratch query does not overwrite its canary");
    memcpy(g_saved, g_cached, sizeof(g_saved));
    memset(g_scratch, 0x3c, sizeof(g_scratch));
    Expect(service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, jointCount,
                                            g_cached, TEST_VERTICES, g_scratch, bytes) != 0u &&
               memcmp(g_saved, g_cached, sizeof(g_saved)) == 0,
           "workspace reuse does not retain a previous pose");
}

static void Reject(const LaiueAnimationServiceV1 *service, uint32_t jointCount, const char *message)
{
    memset(g_cached, 0x69, sizeof(g_cached));
    memcpy(g_saved, g_cached, sizeof(g_saved));
    Expect(service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, jointCount,
                                            g_cached, TEST_VERTICES, g_scratch,
                                            service->skinningScratchBytes(jointCount)) == 0u &&
               memcmp(g_saved, g_cached, sizeof(g_saved)) == 0,
           message);
    memset(g_legacy, 0x69, sizeof(g_legacy));
    Expect(service->skinVertices(g_vertices, TEST_VERTICES, g_palette, jointCount, g_legacy,
                                 TEST_VERTICES) == 0u &&
               memcmp(g_saved, g_legacy, sizeof(g_saved)) == 0,
           "the same failure has the legacy output transaction");
}

static void WorkspaceGuards(const LaiueAnimationServiceV1 *service)
{
    Fixture(4u, 4u);
    const uint32_t bytes = service->skinningScratchBytes(4u);
    memset(g_cached, 0x69, sizeof(g_cached));
    memcpy(g_saved, g_cached, sizeof(g_saved));
    memset(g_scratch, 0xa5, sizeof(g_scratch));
    memcpy(g_savedScratch, g_scratch, sizeof(g_scratch));
    Expect(service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u, g_cached,
                                            TEST_VERTICES, g_scratch, bytes - 1u) == 0u &&
               service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u, g_cached,
                                                TEST_VERTICES, (uint8_t *)g_scratch + 1u,
                                                bytes) == 0u &&
               service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u, g_cached,
                                                TEST_VERTICES, NULL, bytes) == 0u &&
               service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u, g_cached,
                                                TEST_VERTICES - 1u, g_scratch, bytes) == 0u,
           "short, unaligned, NULL workspace and short output are rejected");
    Expect(memcmp(g_savedScratch, g_scratch, sizeof(g_scratch)) == 0 &&
               memcmp(g_saved, g_cached, sizeof(g_saved)) == 0,
           "capacity and alignment failures write neither workspace nor output");
    Expect(service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u, g_cached,
                                            TEST_VERTICES, g_vertices, bytes) == 0u &&
               service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u, g_cached,
                                                TEST_VERTICES, g_palette, bytes) == 0u &&
               service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u, g_cached,
                                                TEST_VERTICES, g_cached, bytes) == 0u,
           "workspace cannot alias either input or the output");
    Compare(service, 4u);
    memcpy(g_savedScratch, g_scratch, sizeof(g_scratch));
    Expect(
        service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u,
                                         (LaiueAnimationSkinnedVertexV1 *)(void *)(g_vertices + 1u),
                                         TEST_VERTICES, g_scratch, bytes) == 0u &&
            service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u,
                                             (LaiueAnimationSkinnedVertexV1 *)(void *)g_palette,
                                             TEST_VERTICES, g_scratch, bytes) == 0u &&
            memcmp(g_savedScratch, g_scratch, sizeof(g_scratch)) == 0,
        "input/output overlap is rejected before preparing any matrix");
    const void *unreadable = (const void *)(uintptr_t)1u;
    Expect(service->skinVerticesWithScratch(unreadable, UINT32_MAX, unreadable, TEST_JOINTS + 1u,
                                            (LaiueAnimationSkinnedVertexV1 *)(uintptr_t)1u,
                                            UINT32_MAX, (void *)(uintptr_t)1u, UINT32_MAX) == 0u,
           "oversized joint count rejects unreadable inputs before size arithmetic");
    Expect(service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 4u, g_cached,
                                            TEST_VERTICES, (void *)(UINTPTR_MAX - 7u), bytes) == 0u,
           "wrapping workspace range is rejected before dereference");
}

static void LegacyPrefix(const LaiueAnimationServiceV1 *service)
{
    _Alignas(8) uint8_t prefix[LEGACY_TABLE_BYTES];
    LaiueAnimationServiceV1 *old = (LaiueAnimationServiceV1 *)(void *)prefix;
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const size_t pageBytes = info.dwPageSize;
    uint8_t *pages = VirtualAlloc(NULL, 2u * pageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Expect(pages != NULL && pageBytes >= sizeof(prefix), "old ABI guard pages allocate");
    DWORD previous = 0u;
    Expect(VirtualProtect(pages + pageBytes, pageBytes, PAGE_NOACCESS, &previous) != 0,
           "old ABI tail is inaccessible");
    old = (LaiueAnimationServiceV1 *)(void *)(pages + pageBytes - sizeof(prefix));
#endif
    memcpy(old, service, sizeof(prefix));
    old->structSize = (uint32_t)sizeof(prefix);
    Expect(!HasScratch(old), "a physically old table declines the optional tail");
    Fixture(4u, 4u);
    Expect(old->skinVertices(g_vertices, TEST_VERTICES, g_palette, 4u, g_legacy, TEST_VERTICES) !=
               0u,
           "the physically old table retains its old skinning callback");
#if defined(_WIN32)
    Expect(VirtualFree(pages, 0u, MEM_RELEASE) != 0, "old ABI guard pages release");
#endif
    LaiueAnimationServiceV1 missing;
    memcpy(&missing, service, sizeof(missing));
    missing.skinningScratchBytes = NULL;
    Expect(!HasScratch(&missing), "a missing optional callback declines the extension");
    missing.skinningScratchBytes = service->skinningScratchBytes;
    missing.skinVerticesWithScratch = NULL;
    Expect(!HasScratch(&missing), "both optional callbacks are required");
}

static void GuardedWorkspace(const LaiueAnimationServiceV1 *service)
{
#if defined(_WIN32)
    Fixture(8u, 4u);
    const uint32_t bytes = service->skinningScratchBytes(8u);
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const size_t pageBytes = info.dwPageSize;
    uint8_t *pages = VirtualAlloc(NULL, 2u * pageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Expect(pages != NULL && pageBytes >= bytes, "workspace guard pages allocate");
    DWORD previous = 0u;
    Expect(VirtualProtect(pages + pageBytes, pageBytes, PAGE_NOACCESS, &previous) != 0,
           "the byte after the exact queried workspace is inaccessible");
    void *scratch = pages + pageBytes - bytes;
    Expect(service->skinVertices(g_vertices, TEST_VERTICES, g_palette, 8u, g_legacy,
                                 TEST_VERTICES) != 0u &&
               service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 8u, g_cached,
                                                TEST_VERTICES, scratch, bytes) != 0u &&
               memcmp(g_legacy, g_cached, sizeof(g_cached)) == 0,
           "an exact queried workspace fits immediately before an inaccessible page");
    memcpy(g_saved, g_cached, sizeof(g_saved));
    Expect(service->skinVerticesWithScratch(g_vertices, TEST_VERTICES, g_palette, 8u, g_cached,
                                            TEST_VERTICES, scratch, bytes - 1u) == 0u &&
               memcmp(g_saved, g_cached, sizeof(g_saved)) == 0,
           "a short guarded workspace preserves the entire output");
    Expect(VirtualFree(pages, 0u, MEM_RELEASE) != 0, "workspace guard pages release");
#else
    (void)service;
#endif
}

LAIUE_TEST_ENTRY(AnimationScratchTestEntryPoint)
{
    const LaiueAnimationServiceV1 *service = LaiueAnimationGetStaticServiceV1();
    Expect(HasScratch(service), "provider publishes the complete non-NULL optional tail");
    Expect(service->skinningScratchBytes(0u) == 0u && service->skinningScratchBytes(1u) == 73u &&
               service->skinningScratchBytes(TEST_JOINTS) == SCRATCH_BYTES &&
               service->skinningScratchBytes(TEST_JOINTS + 1u) == 0u &&
               service->skinningScratchBytes(UINT32_MAX) == 0u,
           "workspace query handles zero, maximum and oversized counts without overflow");
    Expect(service->skinVerticesWithScratch(NULL, 0u, NULL, 0u, NULL, 0u, NULL, 0u) != 0u &&
               service->skinVerticesWithScratch(NULL, 0u, NULL, TEST_JOINTS + 1u, NULL, 0u, NULL,
                                                0u) == 0u,
           "zero vertices preserves the legacy no-op and maximum-count contract");
    for (uint32_t influences = 1u; influences <= 4u; ++influences)
    {
        Fixture(32u, influences);
        Compare(service, 32u);
    }
    Fixture(TEST_JOINTS, 4u);
    /* Exercise the maximum index even when TEST_VERTICES uses fewer joints. */
    g_vertices[TEST_VERTICES - 1u].joints[3] = TEST_JOINTS - 1u;
    Compare(service, TEST_JOINTS);
    Fixture(4u, 4u);
    g_vertices[TEST_VERTICES - 1u].weights[3] += 0.0005f;
    Compare(service, 4u);
    Fixture(4u, 1u);
    for (uint32_t vertex = 0u; vertex < TEST_VERTICES; ++vertex)
        g_vertices[vertex].joints[0] = 0u;
    memset(&g_palette[3], 0, sizeof(g_palette[3]));
    Compare(service, 4u);
    g_vertices[TEST_VERTICES - 1u].joints[0] = 3u;
    Reject(service, 4u, "a late used singular joint leaves the complete output unchanged");
    Fixture(4u, 1u);
    g_vertices[TEST_VERTICES - 1u].joints[3] = 4u;
    Reject(service, 4u, "a zero-weight out-of-range joint still rejects the whole mesh");
    Fixture(4u, 4u);
    g_vertices[TEST_VERTICES - 1u].weights[0] = -0.25f;
    Reject(service, 4u, "a late negative weight leaves the complete output unchanged");
    Fixture(4u, 4u);
    g_vertices[TEST_VERTICES - 1u].weights[0] = 0.0f;
    Reject(service, 4u, "a late invalid weight total leaves the complete output unchanged");
    Fixture(4u, 1u);
    g_vertices[TEST_VERTICES - 1u].position[0] = FLT_MAX;
    g_palette[g_vertices[TEST_VERTICES - 1u].joints[0]].m[4] = FLT_MAX;
    Reject(service, 4u, "finite inputs with late output overflow never partially publish");
    Fixture(4u, 1u);
    for (uint32_t vertex = 0u; vertex < TEST_VERTICES; ++vertex)
        g_vertices[vertex].joints[0] = 0u;
    const uint32_t nanBits = UINT32_C(0x7fc00000);
    memcpy(&g_palette[3].m[3], &nanBits, sizeof(nanBits));
    Reject(service, 4u, "a nonfinite unused matrix still rejects the palette");
    Fixture(4u, 4u);
    memcpy(&g_vertices[TEST_VERTICES - 1u].normal[1], &nanBits, sizeof(nanBits));
    Reject(service, 4u, "a late nonfinite vertex leaves the complete output unchanged");
    WorkspaceGuards(service);
    LegacyPrefix(service);
    GuardedWorkspace(service);
    LaiueTestRuntimeWrite("Animation scratch checks passed\n");
    LAIUE_TEST_SUCCESS();
}
