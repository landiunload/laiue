/* Manual query benchmark, excluded from ALL and CTest. Each case warms up
 * 256 times, then reports the median of nine runs in nanoseconds per query:
 * 131072 calls/run for one box and 8192 calls/run for 64 coincident boxes.
 * Queries perform no allocations. The unchanged fixture checksum is
 * 10589983488. Run before/after executables alternately on the same host;
 * do not run builds, tests or another benchmark concurrently.
 *
 * Reproduce the Windows baseline with either windows-msvc or windows-clang:
 *   cmake --preset windows-msvc -DLAIUE_BUILD_GRAPHICS=OFF
 *     -DLAIUE_BUILD_BENCHMARKS=ON -DLAIUE_ENABLE_LTO=OFF
 *     -DLAIUE_AGGRESSIVE_INLINING=OFF "-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob1"
 *   cmake --build --preset windows-msvc-release
 *     --target laiue_mesh_world_query_benchmark
 * clang-cl uses the identical Ob1/no-LTO configuration. The module retains
 * its normal precise-FP and AVX2/x86-64-v3 options; no stack probes or CRT
 * checks are disabled. Default Ob3+LTCG failed to link the original 64-OBB
 * implementation, so both measured versions deliberately use Ob1/no-LTO.
 * The final implementation is also linked and tested with default Ob3+LTCG.
 *
 * Original baseline source:
 *   git show 5269aa44244ce1b28da33f701a53910c27220b5f:
 *     src/simulation/mesh_world/mesh_world.c
 * (The command above is one line, with no space following the colon.)
 * Blob: 1725a148f9ef69c38409cccd0b49c64fc5479986.
 * Save that file outside the checkout. Use `ninja -f build-Release.ninja
 * -t commands laiue_mesh_world` to obtain this configuration's exact compile
 * and DLL-link commands. Compile the saved source to a separate object,
 * replace only that object in the DLL-link command, and write the baseline
 * DLL to a separate directory. Copy the same benchmark executable there;
 * both copies then run identical client code against their adjacent DLL.
 * This avoids changing the current source tree or mixing compiler flags.
 * The comparison recorded for this change used the same MSVC-built client
 * executable for the MSVC and clang-cl module variants.
 */

#include "mesh_world/mesh_world_service.h"
#include "platform/system.h"
#include "test_runtime.h"
#include <stdint.h>

static volatile uint64_t checksum;
static const LaiueMeshWorldServiceV1 *service;
static LaiueMeshColliderV1 colliders[64];
static LaiueMeshBoxV1 boxes[64];
static void Number(uint64_t value)
{
    char text[32];
    uint32_t count = 0;
    do
    {
        text[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    for (uint32_t i = 0; i < count / 2; ++i)
    {
        char c = text[i];
        text[i] = text[count - 1 - i];
        text[count - 1 - i] = c;
    }
    text[count] = 0;
    PlatformWriteConsoleUtf8(text);
}
static void Query(LaiueMeshWorldV1 *world, uint32_t kind)
{
    LaiueMeshPositionV1 start = {{0, 0, 0}, {0, 4, 4}}, center = {{0, 0, 0}, {4, 4, 4}};
    float half[3] = {.25f, .25f, .25f}, delta[3] = {8, 0, 0}, direction[3] = {1, 0, 0};
    float minimum[3] = {-1, -1, -1}, maximum[3] = {1, 1, 1};
    LaiueMeshHitV1 hit = {0};
    uint32_t count = 0;
    if (kind == 0)
    {
        uint32_t yes = service->sweepBox(world, &start, half, delta, &hit);
        checksum += yes + (uint64_t)(hit.time * 10000);
    }
    if (kind == 1)
    {
        uint32_t yes = service->raycast(world, &start, direction, 8, &hit);
        checksum += yes + (uint64_t)(hit.time * 10000);
    }
    if (kind == 2)
    {
        uint32_t yes =
            service->overlapBoxes(world, &center, minimum, maximum, colliders, 64, &count);
        checksum += yes + count;
    }
    if (kind == 3)
        checksum += service->blockSolid(world, 4, 4, 4, 1);
}
LAIUE_TEST_ENTRY(MeshWorldQueryBenchmarkEntryPoint)
{
    service = LaiueMeshWorldGetStaticServiceV1();
    const char *names[4] = {"sweep", "ray", "overlap", "solid"};
    for (uint32_t sizeIndex = 0; sizeIndex < 2; ++sizeIndex)
    {
        uint32_t boxCount = sizeIndex ? 64 : 1;
        LaiueMeshWorldConfigV1 config = {sizeof(config), 16, 16};
        LaiueMeshWorldV1 *world = 0;
        if (!service->create(&config, &world))
            LaiueTestRuntimeExit(1);
        for (uint32_t i = 0; i < boxCount; ++i)
        {
            boxes[i].halfExtent[0] = .5f;
            boxes[i].halfExtent[1] = .5f;
            boxes[i].halfExtent[2] = .5f;
            boxes[i].rotation[3] = 1;
        }
        LaiueMeshShapeV1 shape = {0};
        shape.structSize = sizeof(shape);
        shape.boxes = boxes;
        shape.boxCount = boxCount;
        for (uint32_t a = 0; a < 3; ++a)
        {
            shape.boundsMin[a] = -.5f;
            shape.boundsMax[a] = .5f;
        }
        LaiueMeshInstanceDescV1 desc = {0};
        desc.structSize = sizeof(desc);
        desc.model = 1;
        desc.flags = LAIUE_MESH_INSTANCE_COLLIDABLE;
        desc.transform.position.local[0] = 4;
        desc.transform.position.local[1] = 4;
        desc.transform.position.local[2] = 4;
        desc.transform.rotation[3] = 1;
        desc.transform.scale[0] = 1;
        desc.transform.scale[1] = 1;
        desc.transform.scale[2] = 1;
        LaiueMeshInstanceV1 instance = 0;
        if (!service->registerShape(world, 1, &shape) || !service->add(world, &desc, &instance))
            LaiueTestRuntimeExit(2);
        for (uint32_t kind = 0; kind < 4; ++kind)
        {
            uint32_t iterations = sizeIndex ? 8192 : 131072;
            double samples[9];
            for (uint32_t warm = 0; warm < 256; ++warm)
                Query(world, kind);
            for (uint32_t run = 0; run < 9; ++run)
            {
                double start = PlatformMonotonicSeconds();
                for (uint32_t i = 0; i < iterations; ++i)
                    Query(world, kind);
                samples[run] = (PlatformMonotonicSeconds() - start) * 1e9 / (double)iterations;
            }
            for (uint32_t i = 1; i < 9; ++i)
            {
                double value = samples[i];
                uint32_t at = i;
                while (at && samples[at - 1] > value)
                {
                    samples[at] = samples[at - 1];
                    --at;
                }
                samples[at] = value;
            }
            PlatformWriteConsoleUtf8(names[kind]);
            PlatformWriteConsoleUtf8(" boxes=");
            Number(boxCount);
            PlatformWriteConsoleUtf8(" median_ns=");
            Number((uint64_t)samples[4]);
            PlatformWriteConsoleUtf8("\n");
        }
        service->destroy(world);
    }
    PlatformWriteConsoleUtf8("checksum=");
    Number(checksum);
    PlatformWriteConsoleUtf8("\n");
    LAIUE_TEST_SUCCESS();
}
