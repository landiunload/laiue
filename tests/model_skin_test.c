#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "media/model.h"
#include "model/model_service.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

/* Independent bytes, not an encoder-produced fixture: six section entries
 * followed by one triangle, no materials/collision, and a two-joint skin. */
enum
{
    FIXTURE_BYTES = 400u,
    SKIN_ENTRY = 120u,
    SKIN_PAYLOAD = 296u,
    LAST_SKIN_VERTEX = 368u,
};

static uint8_t g_file[1024];
static uint8_t g_encoded[1024];
static ModelVertex g_vertices[3];
static uint32_t g_indices[3];
static ModelPart g_parts[1];
static ModelSkinVertex g_skin[3];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Model skin check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t U32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

static void PutU32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t byte = 0u; byte < 4u; ++byte)
        bytes[byte] = (uint8_t)(value >> (byte * 8u));
}

static void PutFloat(uint8_t *bytes, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    PutU32(bytes, bits);
}

static void Fixture(void)
{
    static const char tags[6][4] = {
        {'V', 'E', 'R', 'T'}, {'I', 'N', 'D', 'X'}, {'P', 'A', 'R', 'T'},
        {'S', 'T', 'R', 'S'}, {'B', 'N', 'D', 'S'}, {'S', 'K', 'I', 'N'},
    };
    static const uint32_t offsets[6] = {136u, 244u, 256u, 272u, 272u, SKIN_PAYLOAD};
    static const uint32_t lengths[6] = {108u, 12u, 16u, 0u, 24u, 104u};
    static const uint32_t counts[6] = {3u, 3u, 1u, 0u, 1u, 3u};
    memset(g_file, 0, sizeof(g_file));
    memcpy(g_file, "LMD1", 4u);
    g_file[4] = 1u;
    g_file[6] = 40u;
    PutU32(g_file + 8u, 6u);
    PutU32(g_file + 12u, FIXTURE_BYTES);
    PutU32(g_file + 28u, 7u);
    for (uint32_t section = 0u; section < 6u; ++section)
    {
        uint8_t *entry = g_file + 40u + section * 16u;
        memcpy(entry, tags[section], 4u);
        PutU32(entry + 4u, offsets[section]);
        PutU32(entry + 8u, lengths[section]);
        PutU32(entry + 12u, counts[section]);
    }
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
    {
        uint8_t *record = g_file + 136u + vertex * 36u;
        PutFloat(record + 20u, 1.0f);
        PutFloat(record + 24u, (float)vertex * 0.5f);
        PutU32(record + 32u, UINT32_C(0xff203040) + vertex);
        PutU32(g_file + 244u + vertex * 4u, vertex);
    }
    PutFloat(g_file + 172u, 1.0f);
    PutFloat(g_file + 212u, 1.0f);
    PutU32(g_file + 260u, 3u);
    PutFloat(g_file + 284u, 1.0f);
    PutFloat(g_file + 288u, 1.0f);
    PutU32(g_file + SKIN_PAYLOAD, 2u);
    PutU32(g_file + SKIN_PAYLOAD + 4u, 3u);
    PutFloat(g_file + 320u, 1.0f);
    PutU32(g_file + 336u, 1u);
    PutFloat(g_file + 352u, 1.0f);
    PutU32(g_file + LAST_SKIN_VERTEX + 4u, 1u);
    PutU32(g_file + LAST_SKIN_VERTEX + 8u, 1u);
    PutFloat(g_file + LAST_SKIN_VERTEX + 16u, 0.25f);
    PutFloat(g_file + LAST_SKIN_VERTEX + 20u, 0.75f);
}

static ModelData Buffers(void)
{
    ModelData model = {0};
    model.vertices = g_vertices;
    model.indices = g_indices;
    model.parts = g_parts;
    model.skinVertices = g_skin;
    return model;
}

static void CheckCodec(void)
{
    Fixture();
    ModelInfo info;
    Expect(ModelInspect(g_file, FIXTURE_BYTES, &info) == MODEL_OK && info.vertexCapacity == 3u &&
               info.skinVertexCapacity == 3u && info.boxCapacity == 0u && info.scratchBytes == 0u,
           "independent SKIN layout inspects without scratch");
    ModelData model = Buffers();
    Expect(ModelDecode(g_file, FIXTURE_BYTES, NULL, &info, &model, NULL, 0u) == MODEL_OK &&
               model.skinVertexCount == 3u && model.skinJointCount == 2u &&
               g_skin[1].joints[0] == 1u && g_skin[1].weights[0] == 1.0f &&
               g_skin[2].joints[2] == 1u && g_skin[2].weights[2] == 0.0f &&
               g_skin[2].weights[0] == 0.25f && g_skin[2].weights[1] == 0.75f,
           "caller buffers retain all four indices and weights");
    uint32_t encodedBytes = 0u;
    uint32_t written = 0u;
    Expect(ModelEncodedBytes(&model, &encodedBytes) == MODEL_OK && encodedBytes == FIXTURE_BYTES &&
               ModelEncode(&model, g_encoded, sizeof(g_encoded), &written) == MODEL_OK &&
               written == FIXTURE_BYTES && memcmp(g_encoded, g_file, FIXTURE_BYTES) == 0,
           "encoder matches independent complete file byte for byte");
    memset(g_encoded, 0xa5, sizeof(g_encoded));
    Expect(ModelEncode(&model, g_encoded, encodedBytes - 1u, &written) == MODEL_BUFFER_TOO_SMALL &&
               written == 0u && g_encoded[0] == 0xa5u && g_encoded[encodedBytes - 1u] == 0xa5u,
           "short encoder capacity never writes a partial file");
    ModelBox box = {{0.5f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.01f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    model.boxes = &box;
    model.boxCount = 1u;
    Expect(ModelEncode(&model, g_encoded, sizeof(g_encoded), &written) == MODEL_OK &&
               U32(g_encoded + 8u) == 7u && ModelInspect(g_encoded, written, &info) == MODEL_OK &&
               info.boxCapacity == 1u && info.skinVertexCapacity == 3u,
           "COLL and SKIN coexist in distinct sections");
    ModelData again = Buffers();
    again.boxes = &box;
    Expect(ModelDecode(g_encoded, written, NULL, &info, &again, NULL, 0u) == MODEL_OK &&
               again.skinJointCount == 2u && again.boxCount == 1u && g_skin[2].weights[1] == 0.75f,
           "collision and skin roundtrip together");

    Fixture();
    Expect(ModelInspect(g_file, FIXTURE_BYTES, &info) == MODEL_OK, "baseline reinspects");
    info.skinVertexCapacity = 2u;
    memset(g_vertices, 0xa5, sizeof(g_vertices));
    memset(g_skin, 0xa5, sizeof(g_skin));
    model = Buffers();
    Expect(ModelDecode(g_file, FIXTURE_BYTES, NULL, &info, &model, NULL, 0u) ==
                   MODEL_BUFFER_TOO_SMALL &&
               ((uint8_t *)g_vertices)[0] == 0xa5u && ((uint8_t *)g_skin)[0] == 0xa5u &&
               model.vertexCount == 0u,
           "short skin capacity fails before geometry or weights are written");
    info.skinVertexCapacity = 3u;
    model.skinVertices = NULL;
    Expect(ModelDecode(g_file, FIXTURE_BYTES, NULL, &info, &model, NULL, 0u) ==
                   MODEL_INVALID_ARGUMENT &&
               ((uint8_t *)g_vertices)[0] == 0xa5u,
           "missing caller skin buffer fails before writing geometry");
    const float acceptedSums[] = {0.9995f, 1.0005f};
    for (uint32_t sum = 0u; sum < 2u; ++sum)
    {
        Fixture();
        PutFloat(g_file + LAST_SKIN_VERTEX + 16u, acceptedSums[sum]);
        PutFloat(g_file + LAST_SKIN_VERTEX + 20u, 0.0f);
        PutU32(g_file + LAST_SKIN_VERTEX + 28u, UINT32_C(0x80000000));
        Expect(ModelInspect(g_file, FIXTURE_BYTES, &info) == MODEL_OK,
               "near-unit sums and finite negative zero remain valid");
        model = Buffers();
        Expect(ModelDecode(g_file, FIXTURE_BYTES, NULL, &info, &model, NULL, 0u) == MODEL_OK &&
                   g_skin[2].weights[0] == acceptedSums[sum],
               "codec retains authored weights instead of silently normalizing them");
    }
    g_skin[2].joints[3] = 2u;
    memset(g_encoded, 0xa5, sizeof(g_encoded));
    Expect(ModelEncode(&model, g_encoded, sizeof(g_encoded), &written) == MODEL_CORRUPT &&
               written == 0u && g_encoded[0] == 0xa5u,
           "encoder rejects late invalid skin before writing any bytes");
}

static void ExpectRejected(uint32_t size, ModelStatus expected, const char *message)
{
    ModelInfo info;
    Expect(ModelInspect(g_file, size, &info) == expected, message);
    const LaiueModelServiceV1 *service = LaiueModelGetStaticServiceV1();
    uint32_t status = LAIUE_MODEL_LOAD_OK;
    LaiueModelV1 *model = service->loadMemory(g_file, size, &status);
    Expect(model == NULL && status == LAIUE_MODEL_LOAD_INVALID_MODEL,
           "explicit malformed model load fails instead of hiding bad skin");
}

static void CheckRejected(void)
{
    Fixture();
    ExpectRejected(FIXTURE_BYTES - 1u, MODEL_TRUNCATED, "truncated last skin record rejects");
    Fixture();
    PutU32(g_file + SKIN_ENTRY + 8u, 7u);
    ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "partial skin header rejects");
    Fixture();
    PutU32(g_file + SKIN_ENTRY + 8u, 100u);
    ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "skin stride is exactly 32 bytes");
    Fixture();
    PutU32(g_file + SKIN_ENTRY + 4u, 300u);
    ExpectRejected(FIXTURE_BYTES, MODEL_TRUNCATED, "skin payload out of bounds rejects");
    Fixture();
    PutU32(g_file + SKIN_ENTRY + 12u, MODEL_MAX_VERTICES + 1u);
    ExpectRejected(FIXTURE_BYTES, MODEL_TOO_LARGE, "oversized skin record count rejects");
    Fixture();
    PutU32(g_file + SKIN_PAYLOAD, MODEL_MAX_SKIN_JOINTS + 1u);
    ExpectRejected(FIXTURE_BYTES, MODEL_TOO_LARGE, "oversized skin palette rejects");
    Fixture();
    PutU32(g_file + SKIN_PAYLOAD, 0u);
    ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "skin without joints rejects");
    Fixture();
    PutU32(g_file + SKIN_PAYLOAD + 4u, 2u);
    ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "payload and table vertex counts must agree");
    Fixture();
    PutU32(g_file + 52u, 2u);
    PutU32(g_file + 48u, 72u);
    ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "skin count must equal actual VERT count");
    Fixture();
    PutU32(g_file + LAST_SKIN_VERTEX + 12u, 2u);
    ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "invalid final zero-weight index rejects");
    static const uint32_t nonfinite[] = {
        UINT32_C(0x7f800000), UINT32_C(0xff800000), UINT32_C(0x7fc00001),
        UINT32_C(0xffc00001), UINT32_C(0x7f800001), UINT32_C(0xff800001),
    };
    for (uint32_t value = 0u; value < sizeof(nonfinite) / sizeof(nonfinite[0]); ++value)
    {
        Fixture();
        PutU32(g_file + LAST_SKIN_VERTEX + 28u, nonfinite[value]);
        ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "nonfinite final unused weight rejects");
    }
    Fixture();
    PutFloat(g_file + LAST_SKIN_VERTEX + 16u, -0.25f);
    PutFloat(g_file + LAST_SKIN_VERTEX + 20u, 1.25f);
    ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "negative weight rejects despite unit sum");
    Fixture();
    memset(g_file + LAST_SKIN_VERTEX + 16u, 0, 16u);
    ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "zero weight sum rejects");
    const float sums[] = {0.998f, 1.002f};
    for (uint32_t sum = 0u; sum < 2u; ++sum)
    {
        Fixture();
        PutFloat(g_file + LAST_SKIN_VERTEX + 16u, sums[sum]);
        PutFloat(g_file + LAST_SKIN_VERTEX + 20u, 0.0f);
        ExpectRejected(FIXTURE_BYTES, MODEL_CORRUPT, "weight sum outside tolerance rejects");
    }
    Fixture();
    ModelInfo info;
    Expect(ModelInspect(g_file, FIXTURE_BYTES, &info) == MODEL_OK, "valid metadata captures");
    PutU32(g_file + LAST_SKIN_VERTEX, UINT32_MAX);
    ModelData model = Buffers();
    memset(g_vertices, 0xa5, sizeof(g_vertices));
    memset(g_skin, 0xa5, sizeof(g_skin));
    Expect(ModelDecode(g_file, FIXTURE_BYTES, NULL, &info, &model, NULL, 0u) == MODEL_CORRUPT &&
               ((uint8_t *)g_vertices)[0] == 0xa5u && ((uint8_t *)g_skin)[0] == 0xa5u,
           "changed late record is revalidated before decoder publishes geometry");
}

static uint32_t AppendSection(bool duplicateSkin)
{
    Fixture();
    /* Make room for an extra table entry; all original payloads move by
     * sixteen bytes, independent of production layout planning. */
    for (uint32_t byte = FIXTURE_BYTES; byte-- > 136u;)
        g_file[byte + 16u] = g_file[byte];
    for (uint32_t section = 0u; section < 6u; ++section)
    {
        uint8_t *entry = g_file + 40u + section * 16u;
        PutU32(entry + 4u, U32(entry + 4u) + 16u);
    }
    const uint32_t length = duplicateSkin ? 104u : 12u;
    uint8_t *entry = g_file + 136u;
    memcpy(entry, duplicateSkin ? "SKIN" : "FUTR", 4u);
    PutU32(entry + 4u, FIXTURE_BYTES + 16u);
    PutU32(entry + 8u, length);
    PutU32(entry + 12u, 3u);
    if (duplicateSkin)
        memcpy(g_file + FIXTURE_BYTES + 16u, g_file + SKIN_PAYLOAD + 16u, length);
    else
        memset(g_file + FIXTURE_BYTES + 16u, 0x5a, length);
    PutU32(g_file + 8u, 7u);
    PutU32(g_file + 12u, FIXTURE_BYTES + 16u + length);
    return FIXTURE_BYTES + 16u + length;
}

static void CheckUnknownAndStatic(void)
{
    ModelInfo info;
    uint32_t bytes = AppendSection(false);
    Expect(ModelInspect(g_file, bytes, &info) == MODEL_OK && info.skinVertexCapacity == 3u,
           "future unknown section preserves recognized skin data");
    ModelData model = Buffers();
    Expect(ModelDecode(g_file, bytes, NULL, &info, &model, NULL, 0u) == MODEL_OK &&
               model.skinJointCount == 2u && g_skin[2].weights[1] == 0.75f,
           "future unknown data does not alter skin records");
    bytes = AppendSection(true);
    ExpectRejected(bytes, MODEL_CORRUPT, "duplicate SKIN sections reject");
    Fixture();
    memcpy(g_file + SKIN_ENTRY, "FUTR", 4u);
    Expect(ModelInspect(g_file, FIXTURE_BYTES, &info) == MODEL_OK && info.skinVertexCapacity == 0u,
           "unknown skin tag is a valid static model for an older reader");
    model = Buffers();
    model.skinVertexCount = 3u;
    model.skinJointCount = 2u;
    Expect(ModelDecode(g_file, FIXTURE_BYTES, NULL, &info, &model, NULL, 0u) == MODEL_OK &&
               model.skinVertexCount == 0u && model.skinJointCount == 0u &&
               model.vertexCount == 3u && model.indexCount == 3u,
           "static decoding clears prior skin metadata without changing geometry");
    uint32_t written = 0u;
    Expect(ModelEncode(&model, g_encoded, sizeof(g_encoded), &written) == MODEL_OK &&
               U32(g_encoded + 8u) == 5u,
           "static encoder keeps its original five-section layout");
    static const char obj[] = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    Expect(ModelInspect(obj, sizeof(obj) - 1u, &info) == MODEL_OK && info.skinVertexCapacity == 0u,
           "OBJ continues to expose only static geometry");
}

typedef struct Guarded
{
    uint8_t *pages;
    size_t pageBytes;
} Guarded;

static void *Guard(Guarded *guard, size_t bytes)
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    guard->pageBytes = info.dwPageSize;
    guard->pages =
        VirtualAlloc(NULL, guard->pageBytes * 2u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    Expect(guard->pages != NULL && bytes <= guard->pageBytes, "ABI guard pages allocate");
    DWORD previous = 0u;
    Expect(VirtualProtect(guard->pages + guard->pageBytes, guard->pageBytes, PAGE_NOACCESS,
                          &previous) != 0,
           "ABI tail becomes inaccessible");
#else
    const long pageBytes = sysconf(_SC_PAGESIZE);
    Expect(pageBytes > 0 && bytes <= (size_t)pageBytes, "native page size fits ABI object");
    guard->pageBytes = (size_t)pageBytes;
#if defined(MAP_ANONYMOUS)
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#else
    const int flags = MAP_PRIVATE | MAP_ANON;
#endif
    void *pages = mmap(NULL, guard->pageBytes * 2u, PROT_READ | PROT_WRITE, flags, -1, 0);
    Expect(pages != MAP_FAILED, "ABI anonymous guard pages map");
    guard->pages = pages;
    Expect(mprotect(guard->pages + guard->pageBytes, guard->pageBytes, PROT_NONE) == 0,
           "ABI tail becomes inaccessible");
#endif
    return guard->pages + guard->pageBytes - bytes;
}

static void ReleaseGuard(Guarded *guard)
{
#if defined(_WIN32)
    Expect(VirtualFree(guard->pages, 0u, MEM_RELEASE) != 0, "ABI guard pages release");
#else
    Expect(munmap(guard->pages, guard->pageBytes * 2u) == 0, "ABI guard pages release");
#endif
}

static void CheckShortSkinView(const LaiueModelServiceV1 *service, const LaiueModelV1 *model,
                               size_t prefixBytes, const char *message)
{
    Guarded guard;
    uint8_t *prefix = Guard(&guard, prefixBytes);
    uint8_t saved[sizeof(LaiueModelSkinViewV1)];
    if (prefixBytes == _Alignof(LaiueModelSkinViewV1))
        Expect((uintptr_t)prefix % _Alignof(LaiueModelSkinViewV1) == 0u,
               "the short capacity witness has native view alignment");
    for (uint32_t bytes = 0u; bytes < sizeof(LaiueModelSkinViewV1); ++bytes)
    {
        memset(prefix, 0xa5, prefixBytes);
        memcpy(prefix, &bytes, sizeof(bytes));
        memcpy(saved, prefix, prefixBytes);
        Expect(service->getSkinView(model, (LaiueModelSkinViewV1 *)(void *)prefix) == 0u &&
                   memcmp(prefix, saved, prefixBytes) == 0,
               message);
    }
    ReleaseGuard(&guard);
}

static void CheckUnalignedSkinView(const LaiueModelServiceV1 *service, const LaiueModelV1 *model)
{
    Guarded guard;
    enum
    {
        ALIGNMENT = _Alignof(LaiueModelSkinViewV1),
        BUFFER_BYTES = sizeof(LaiueModelSkinViewV1) + ALIGNMENT,
    };
    uint8_t *buffer = Guard(&guard, BUFFER_BYTES);
    uint8_t saved[BUFFER_BYTES];
    const uint32_t capacity = sizeof(LaiueModelSkinViewV1);
    Expect((uintptr_t)buffer % ALIGNMENT == 0u,
           "unaligned view fixture starts from aligned storage");
    for (size_t offset = 1u; offset < ALIGNMENT; ++offset)
    {
        memset(buffer, 0xa5, BUFFER_BYTES);
        memcpy(buffer + offset, &capacity, sizeof(capacity));
        memcpy(saved, buffer, sizeof(saved));
        Expect(service->getSkinView(model, (LaiueModelSkinViewV1 *)(void *)(buffer + offset)) ==
                       0u &&
                   memcmp(buffer, saved, sizeof(saved)) == 0,
               "full-capacity unaligned output rejects without changing any accessible byte");
    }
    ReleaseGuard(&guard);
}

static void CheckServiceAndAbi(void)
{
    const LaiueModelServiceV1 *service = LaiueModelGetStaticServiceV1();
    Expect(LAIUE_MODEL_SERVICE_HAS_SKIN_V1(service), "optional skin callback is present");
    Expect(!LAIUE_MODEL_SERVICE_HAS_SKIN_V1((const LaiueModelServiceV1 *)NULL),
           "missing provider has no skin callback");
    Fixture();
    uint32_t status = 0u;
    LaiueModelV1 *model = service->loadMemory(g_file, FIXTURE_BYTES, &status);
    Expect(model != NULL && status == LAIUE_MODEL_LOAD_OK, "public skin model loads");
    LaiueModelSkinViewV1 skin = {.structSize = sizeof(skin)};
    Expect(service->getSkinView(model, &skin) != 0u && skin.jointCount == 2u &&
               skin.vertexCount == 3u && skin.vertices != NULL &&
               skin.vertices[2].joints[2] == 1u && skin.vertices[2].weights[1] == 0.75f,
           "separate borrowed view exposes skin without an animation dependency");
    memset(g_file, 0xa5, sizeof(g_file));
    Expect(skin.vertices[0].weights[0] == 1.0f && skin.vertices[1].joints[0] == 1u &&
               skin.vertices[2].weights[1] == 0.75f,
           "the owned model retains weights after input bytes are overwritten");
    /* The four-byte header is deliberately not pointer-aligned on 64-bit
     * targets. The second prefix is aligned, so capacity validation remains
     * independently witnessed immediately before an inaccessible page. */
    CheckShortSkinView(service, model, sizeof(uint32_t),
                       "physically short output header rejects without touching inaccessible tail");
    CheckShortSkinView(
        service, model, _Alignof(LaiueModelSkinViewV1),
        "aligned physically short output rejects without touching inaccessible tail");
    CheckUnalignedSkinView(service, model);
    Guarded guard;
    LaiueModelViewV1 *oldView = Guard(&guard, sizeof(LaiueModelViewV1));
    memset(oldView, 0, sizeof(*oldView));
    Expect(service->getView(model, oldView) != 0u && oldView->vertexCount == 3u &&
               oldView->vertices[1].position[0] == 1.0f && oldView->indices[2] == 2u,
           "original getView writes only its unchanged physical ABI");
    ReleaseGuard(&guard);
    LaiueModelServiceV1 *oldService = Guard(&guard, LAIUE_MODEL_SERVICE_V1_LEGACY_SIZE);
    memcpy(oldService, service, LAIUE_MODEL_SERVICE_V1_LEGACY_SIZE);
    oldService->structSize = LAIUE_MODEL_SERVICE_V1_LEGACY_SIZE;
    Expect(!LAIUE_MODEL_SERVICE_HAS_SKIN_V1(oldService),
           "old provider prefix never reads a nonexistent callback");
    LaiueModelViewV1 view = {0};
    Expect(oldService->getView(model, &view) != 0u && view.vertexCount == 3u,
           "old model service prefix still provides geometry");
    ReleaseGuard(&guard);
    struct
    {
        LaiueModelSkinViewV1 view;
        uint8_t future[16];
    } future;
    memset(&future, 0xa5, sizeof(future));
    future.view.structSize = sizeof(future);
    Expect(service->getSkinView(model, &future.view) != 0u &&
               future.view.structSize == sizeof(LaiueModelSkinViewV1) &&
               future.future[0] == 0xa5u && future.future[15] == 0xa5u,
           "larger caller view preserves its unknown future tail");
    LaiueModelSkinViewV1 saved;
    memcpy(&saved, &skin, sizeof(saved));
    Expect(service->getSkinView(NULL, &skin) == 0u && memcmp(&skin, &saved, sizeof(skin)) == 0 &&
               service->getSkinView(model, NULL) == 0u,
           "invalid arguments leave existing output unchanged");
    service->release(model);

    Fixture();
    memcpy(g_file + SKIN_ENTRY, "FUTR", 4u);
    model = service->loadMemory(g_file, FIXTURE_BYTES, &status);
    Expect(model != NULL && service->getSkinView(model, &skin) != 0u && skin.jointCount == 0u &&
               skin.vertexCount == 0u && skin.vertices == NULL,
           "static model returns an explicit empty skin view");
    service->release(model);
    LaiueModelSetContentService(NULL);
    model = service->loadFrom(NULL, L"missing", &status);
    Expect(model != NULL && status == LAIUE_MODEL_LOAD_NO_CATALOG &&
               service->getSkinView(model, &skin) != 0u && skin.vertices == NULL &&
               skin.jointCount == 0u && skin.vertexCount == 0u,
           "neutral placeholder stays a static cube without content or animation");
    service->release(model);
}

LAIUE_TEST_ENTRY(ModelSkinTestEntryPoint)
{
    CheckCodec();
    CheckRejected();
    CheckUnknownAndStatic();
    CheckServiceAndAbi();
    LaiueTestRuntimeWrite("Model skin checks passed\n");
    LAIUE_TEST_SUCCESS();
}
