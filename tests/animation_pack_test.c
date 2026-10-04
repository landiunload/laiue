#include "animation/animation_service.h"
#include "content/content_service.h"
#include "media/animation.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static wchar_t g_root[LAIUE_PLATFORM_PATH_CAPACITY];
static wchar_t g_path[LAIUE_PLATFORM_PATH_CAPACITY];
static uint8_t g_bytes[128];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Animation packs: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool SameWide(const wchar_t *a, const wchar_t *b)
{
    while (*a != 0 && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void Path(const wchar_t *suffix)
{
    uint32_t length = 0u;
    while (g_root[length] != 0)
    {
        g_path[length] = g_root[length];
        ++length;
    }
    g_path[length++] = L'/';
    for (uint32_t i = 0u; suffix[i] != 0; ++i)
    {
        Expect(length + 1u < LAIUE_PLATFORM_PATH_CAPACITY, "path capacity");
        g_path[length++] = suffix[i];
    }
    g_path[length] = 0;
}

LAIUE_TEST_ENTRY(AnimationPackTestEntryPoint)
{
    Expect(PlatformExecutableDirectory(g_root, LAIUE_PLATFORM_PATH_CAPACITY), "executable path");
    Path(L"animation_pack_test_v1");
    memcpy(g_root, g_path, sizeof(g_root));
    Expect(PlatformCreateDirectory(g_root), "test directory");
    Path(L"animations");
    Expect(PlatformCreateDirectory(g_path), "animations directory");
    Path(L"animations/Base.lkp");
    Expect(PlatformCreateDirectory(g_path), "pack directory");
    Path(L"animations/Base.lkp/sub");
    Expect(PlatformCreateDirectory(g_path), "subfolder");
    AnimationChannel channel = {ANIMATION_SCALAR, 77u, ANIMATION_LINEAR, 0u, 2u};
    AnimationKey keys[2] = {{0.0f, {1.0f, 0.0f, 0.0f, 0.0f}, 0u},
                            {1.0f, {3.0f, 0.0f, 0.0f, 0.0f}, 0u}};
    AnimationData data = {&channel, 1u, keys, 2u, NULL, 0u, 1.0f};
    uint32_t written = 0u;
    Expect(AnimationEncode(&data, g_bytes, sizeof(g_bytes), &written) == ANIMATION_OK,
           "fixture codec");
    Path(L"animations/Base.lkp/sub/wave.lk");
    Expect(PlatformWriteEntireFile(g_path, g_bytes, written), "fixture file");
    Path(L"animations/Base.lkp/broken.lk");
    Expect(PlatformWriteEntireFile(g_path, "bad", 3u), "broken fixture");

    LaiueModuleHostConfigV1 config = {0};
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic = {0};
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    LaiueModuleHost *secondHost = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL && secondHost != NULL, "module hosts");
    const LaiueModuleApiV1 *apis[] = {LaiueAnimationGetStaticModuleApiV1(),
                                      LaiueContentGetStaticModuleApiV1()};
    Expect(LaiueModuleHostLoadStatic(host, apis, 2u, &diagnostic) == LAIUE_MODULE_OK,
           "optional content provider resolves");
    const LaiueAnimationServiceV1 *service =
        (const LaiueAnimationServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_ANIMATION_SERVICE_NAME, 1u, sizeof(LaiueAnimationServiceV1), NULL, NULL);
    const LaiueContentServiceV1 *content =
        (const LaiueContentServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_CONTENT_SERVICE_NAME, 1u, sizeof(LaiueContentServiceV1), NULL, NULL);
    Expect(service != NULL && content != NULL, "published services");
    /* The singleton contract fails the second create before changing the
     * first host's borrowed provider. Its rollback/unload is harmless. */
    Expect(LaiueModuleHostLoadStatic(secondHost, apis, 1u, &diagnostic) != LAIUE_MODULE_OK,
           "second hosted animation instance fails closed");
    LaiueModuleHostUnloadAll(secondHost);
    LaiueContentCatalog *catalog = content->createCatalog(g_root);
    Expect(catalog != NULL && service->activatePack(catalog, L"Base.lkp") != 0u,
           "first host still activates pack after second host failure");
    LaiueAnimationListV1 list = {0};
    Expect(service->enumeratePacks(catalog, &list) != 0u && list.count == 1u &&
               list.entries[0].active != 0u && SameWide(list.entries[0].name, L"Base.lkp"),
           "pack enumeration uses the content lifecycle");
    service->releaseList(&list);
    Expect(service->enumerateClips(catalog, &list) != 0u && list.count == 2u,
           "clip enumeration includes subfolder");
    bool sawWave = false;
    for (uint32_t i = 0u; i < list.count; ++i)
        sawWave = sawWave || SameWide(list.entries[i].name, L"sub/wave");
    Expect(sawWave, "resource path retains subfolder and drops extension");
    service->releaseList(&list);
    uint32_t status = 0u;
    LaiueAnimationV1 *clip = service->loadFrom(catalog, L"sub/wave", &status);
    LaiueAnimationValueV1 sample = {0};
    Expect(clip != NULL && status == LAIUE_ANIMATION_LOAD_OK &&
               service->sampleChannel(clip, 0u, 0.5f, 0u, &sample) != 0u && sample.value[0] == 2.0f,
           "nested clip loads and samples");
    service->release(clip);
    clip = service->loadFrom(catalog, L"missing", &status);
    LaiueAnimationViewV1 view = {.structSize = sizeof(view)};
    Expect(clip != NULL && status == LAIUE_ANIMATION_LOAD_NOT_FOUND &&
               service->getView(clip, &view) != 0u && view.channelCount == 0u &&
               (view.flags & LAIUE_ANIMATION_FLAG_PLACEHOLDER) != 0u &&
               service->samplePose(clip, 10.0f, 1u, NULL, NULL, NULL, 0u) != 0u,
           "missing clip is an empty neutral pose");
    service->release(clip);
    clip = service->loadFrom(catalog, L"broken", &status);
    Expect(clip != NULL && status == LAIUE_ANIMATION_LOAD_INVALID_CLIP,
           "malformed clip reports reason and falls back");
    service->release(clip);
    Expect(service->loadFrom(catalog, L"../outside", &status) == NULL &&
               status == LAIUE_ANIMATION_LOAD_INVALID_CLIP,
           "path traversal rejected");
    Expect(service->activatePack(catalog, L"") != 0u, "deactivation");
    clip = service->loadFrom(catalog, L"sub/wave", &status);
    Expect(clip != NULL && status == LAIUE_ANIMATION_LOAD_NO_ACTIVE_PACK, "inactive fallback");
    service->release(clip);
    content->destroyCatalog(catalog);
    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostLoadStatic(secondHost, apis, 1u, &diagnostic) == LAIUE_MODULE_OK,
           "singleton can transfer after first host unload");
    service = (const LaiueAnimationServiceV1 *)LaiueModuleHostQueryService(
        secondHost, LAIUE_ANIMATION_SERVICE_NAME, 1u, sizeof(LaiueAnimationServiceV1), NULL, NULL);
    Expect(service != NULL, "standalone module service");
    clip = service->loadFrom(NULL, L"sub/wave", &status);
    Expect(clip != NULL && status == LAIUE_ANIMATION_LOAD_NO_CATALOG,
           "module starts without content provider");
    service->release(clip);
    clip = service->loadMemory(g_bytes, written, &status);
    Expect(clip != NULL && status == LAIUE_ANIMATION_LOAD_OK,
           "memory loading works without content module");
    service->release(clip);
    LaiueModuleHostDestroy(secondHost);
    LaiueModuleHostDestroy(host);
    LaiueTestRuntimeWrite("Animation pack checks passed\n");
    LAIUE_TEST_SUCCESS();
}
