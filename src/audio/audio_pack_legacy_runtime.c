/* This translation unit calls the real mixer/content DLLs.  Keep their
 * declarations as imports even though the compatibility target also builds
 * the public AudioPack C functions with LAIUE_BUILD_AUDIO_PACK. */
#define LAIUE_AUDIO_PACK_LEGACY_ADAPTER 1
#include "audio/audio_pack_runtime.h"

static uint32_t LegacyDeviceCreate(const AudioDeviceConfiguration *configuration,
                                   AudioDevice **outDevice)
{
    return (uint32_t)AudioDeviceCreate(configuration, outDevice);
}

static void LegacyDeviceDestroy(AudioDevice *device)
{
    AudioDeviceDestroy(device);
}

static uint32_t LegacyClipCreate(AudioDevice *device, const AudioClipDescription *description,
                                 AudioClip **outClip)
{
    return (uint32_t)AudioClipCreate(device, description, outClip);
}

static const LaiueAudioServiceV1 audio = {
    .structSize = sizeof(LaiueAudioServiceV1),
    .abiVersion = LAIUE_AUDIO_SERVICE_ABI_VERSION_1,
    .deviceCreate = LegacyDeviceCreate,
    .deviceDestroy = LegacyDeviceDestroy,
    .clipCreate = LegacyClipCreate,
};

static uint32_t LegacyGetRoot(
    LaiueContentCatalog *catalog, wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogGetRoot(catalog, destination, capacity) ? 1u : 0u;
}

static uint32_t LegacySetActivePack(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name)
{
    return LaiueContentCatalogSetActivePack(catalog, (LaiueContentType)type, name) ? 1u : 0u;
}

static uint32_t LegacyGetActivePack(
    LaiueContentCatalog *catalog, uint32_t type, wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogGetActivePack(catalog, (LaiueContentType)type, destination, capacity)
               ? 1u
               : 0u;
}

static uint32_t LegacyBuildPath(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *childName, wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogBuildPath(catalog, (LaiueContentType)type, name, childName,
                                        destination, capacity)
               ? 1u
               : 0u;
}

static uint32_t LegacyBuildResourcePath(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *name,
    const wchar_t *resourcePath, const wchar_t *extension,
    wchar_t *destination, uint32_t capacity)
{
    return LaiueContentCatalogBuildResourcePath(catalog, (LaiueContentType)type, name,
                                                resourcePath, extension, destination, capacity)
               ? 1u
               : 0u;
}

static uint32_t LegacyEnumerate(
    LaiueContentCatalog *catalog, uint32_t type, LaiueContentList *outList)
{
    return LaiueContentCatalogEnumerate(catalog, (LaiueContentType)type, outList) ? 1u : 0u;
}

static uint32_t LegacyOrderFormats(
    LaiueContentCatalog *catalog, uint32_t type, const wchar_t *const *defaults,
    uint32_t defaultCount, const wchar_t **outOrder, uint32_t capacity)
{
    return LaiueContentCatalogOrderFormats(catalog, (LaiueContentType)type, defaults,
                                            defaultCount, outOrder, capacity);
}

static uint32_t LegacyNameIsSafe(const wchar_t *name)
{
    return LaiueContentNameIsSafe(name) ? 1u : 0u;
}

static uint32_t LegacyPathIsSafe(const wchar_t *path)
{
    return LaiueContentPathIsSafe(path) ? 1u : 0u;
}

static const LaiueContentServiceV1 content = {
    .structSize = sizeof(LaiueContentServiceV1),
    .abiVersion = LAIUE_CONTENT_SERVICE_ABI_VERSION_1,
    .createCatalog = (LaiueContentCreateCatalogFn)LaiueContentCatalogCreate,
    .destroyCatalog = LaiueContentCatalogDestroy,
    .getRoot = LegacyGetRoot,
    .setActivePack = LegacySetActivePack,
    .getActivePack = LegacyGetActivePack,
    .buildPath = LegacyBuildPath,
    .buildResourcePath = LegacyBuildResourcePath,
    .enumerate = LegacyEnumerate,
    .releaseList = LaiueContentListRelease,
    .orderFormats = LegacyOrderFormats,
    .nameIsSafe = LegacyNameIsSafe,
    .pathIsSafe = LegacyPathIsSafe,
    .defaultCatalog = LaiueContentCatalogDefault,
};

const AudioPackRuntime *AudioPackRuntimeGet(void)
{
    static const AudioPackRuntime runtime = {&audio, &content};
    return &runtime;
}

void AudioPackRuntimeSet(const AudioPackRuntime *runtime)
{
    (void)runtime;
}
