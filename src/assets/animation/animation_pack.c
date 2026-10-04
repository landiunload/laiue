#include "animation/animation_internal.h"

#include "content/content_service.h"
#include "platform/system.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const LaiueContentServiceV1 *g_content;

void LaiueAnimationSetContentService(const struct LaiueContentServiceV1 *content)
{
    g_content = content;
}

static uint64_t AlignUp(uint64_t value)
{
    return (value + 15u) & ~UINT64_C(15);
}

static void SetStatus(uint32_t *destination, uint32_t status)
{
    if (destination != NULL)
        *destination = status;
}

static LaiueAnimationV1 *AllocateClip(const AnimationInfo *info)
{
    const uint64_t channels = AlignUp(sizeof(LaiueAnimationV1));
    const uint64_t keys =
        AlignUp(channels + (uint64_t)info->channelCount * sizeof(AnimationChannel));
    const uint64_t joints = AlignUp(keys + (uint64_t)info->keyCount * sizeof(AnimationKey));
    const uint64_t total = joints + (uint64_t)info->jointCount * sizeof(AnimationJoint);
    if (total > ANIMATION_MAX_FILE_BYTES)
        return NULL;
    uint8_t *block = (uint8_t *)PlatformAllocate((size_t)total, true);
    if (block == NULL)
        return NULL;
    LaiueAnimationV1 *clip = (LaiueAnimationV1 *)block;
    clip->data.channels = info->channelCount != 0u ? (AnimationChannel *)(block + channels) : NULL;
    clip->data.keys = info->keyCount != 0u ? (AnimationKey *)(block + keys) : NULL;
    clip->data.joints = info->jointCount != 0u ? (AnimationJoint *)(block + joints) : NULL;
    clip->data.channelCount = info->channelCount;
    clip->data.keyCount = info->keyCount;
    clip->data.jointCount = info->jointCount;
    clip->data.duration = info->duration;
    return clip;
}

static LaiueAnimationV1 *LoadMemory(const void *bytes, uint32_t sizeBytes, uint32_t *outStatus)
{
    SetStatus(outStatus, LAIUE_ANIMATION_LOAD_INVALID_CLIP);
    AnimationInfo info = {0};
    if (AnimationInspect(bytes, sizeBytes, &info) != ANIMATION_OK)
        return NULL;
    LaiueAnimationV1 *clip = AllocateClip(&info);
    if (clip == NULL)
    {
        SetStatus(outStatus, LAIUE_ANIMATION_LOAD_OUT_OF_MEMORY);
        return NULL;
    }
    if (AnimationDecode(bytes, sizeBytes, &info, &clip->data) != ANIMATION_OK)
    {
        PlatformFree(clip);
        return NULL;
    }
    SetStatus(outStatus, LAIUE_ANIMATION_LOAD_OK);
    return clip;
}

static LaiueAnimationV1 *LoadFile(const wchar_t *path, uint32_t *outStatus)
{
    SetStatus(outStatus, LAIUE_ANIMATION_LOAD_IO_ERROR);
    if (path == NULL)
        return NULL;
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    if (!PlatformReadEntireFile(path, ANIMATION_MAX_FILE_BYTES, &bytes, &size))
        return NULL;
    LaiueAnimationV1 *clip = LoadMemory(bytes, (uint32_t)size, outStatus);
    PlatformFree(bytes);
    return clip;
}

static LaiueAnimationV1 *Placeholder(uint32_t status, uint32_t *outStatus)
{
    const AnimationInfo info = {0};
    LaiueAnimationV1 *clip = AllocateClip(&info);
    if (clip != NULL)
        clip->flags = LAIUE_ANIMATION_FLAG_PLACEHOLDER;
    SetStatus(outStatus, clip != NULL ? status : LAIUE_ANIMATION_LOAD_OUT_OF_MEMORY);
    return clip;
}

static LaiueContentCatalog *CatalogOrDefault(LaiueContentCatalog *catalog)
{
    if (catalog == NULL && g_content != NULL && g_content->defaultCatalog != NULL)
        return g_content->defaultCatalog();
    return catalog;
}

static LaiueAnimationV1 *LoadFrom(LaiueContentCatalog *catalog, const wchar_t *name,
                                  uint32_t *outStatus)
{
    SetStatus(outStatus, LAIUE_ANIMATION_LOAD_INVALID_CLIP);
    if (name == NULL)
        return NULL;
    const LaiueContentServiceV1 *content = g_content;
    if (content == NULL)
        return Placeholder(LAIUE_ANIMATION_LOAD_NO_CATALOG, outStatus);
    if (content->pathIsSafe == NULL || content->pathIsSafe(name) == 0u)
        return NULL;
    catalog = CatalogOrDefault(catalog);
    wchar_t active[LAIUE_CONTENT_NAME_CAPACITY];
    if (catalog == NULL || content->getActivePack == NULL ||
        content->getActivePack(catalog, LAIUE_CONTENT_ANIMATION_PACK, active,
                               LAIUE_CONTENT_NAME_CAPACITY) == 0u)
        return Placeholder(LAIUE_ANIMATION_LOAD_NO_ACTIVE_PACK, outStatus);
    if (content->buildResourcePath == NULL)
        return Placeholder(LAIUE_ANIMATION_LOAD_NO_CATALOG, outStatus);
    wchar_t *path =
        (wchar_t *)PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL)
    {
        SetStatus(outStatus, LAIUE_ANIMATION_LOAD_OUT_OF_MEMORY);
        return NULL;
    }
    if (content->buildResourcePath(catalog, LAIUE_CONTENT_ANIMATION_PACK, active, name, L".lk",
                                   path, LAIUE_CONTENT_PATH_CAPACITY) == 0u)
    {
        PlatformFree(path);
        return Placeholder(LAIUE_ANIMATION_LOAD_NOT_FOUND, outStatus);
    }
    PlatformPathInformation information = {0};
    uint32_t status = LAIUE_ANIMATION_LOAD_NOT_FOUND;
    LaiueAnimationV1 *clip = NULL;
    if (PlatformGetPathInformation(path, &information) && information.exists)
    {
        if (information.isDirectory || information.isSymbolicLink || information.size == 0u ||
            information.size > ANIMATION_MAX_FILE_BYTES)
            status = LAIUE_ANIMATION_LOAD_INVALID_CLIP;
        else
            clip = LoadFile(path, &status);
    }
    PlatformFree(path);
    if (clip != NULL)
    {
        SetStatus(outStatus, status);
        return clip;
    }
    if (status == LAIUE_ANIMATION_LOAD_OUT_OF_MEMORY)
    {
        SetStatus(outStatus, status);
        return NULL;
    }
    return Placeholder(status, outStatus);
}

static uint32_t GetView(const LaiueAnimationV1 *clip, LaiueAnimationViewV1 *outView)
{
    if (clip == NULL || outView == NULL || outView->structSize < sizeof(*outView))
        return 0u;
    outView->flags = clip->flags;
    outView->duration = clip->data.duration;
    outView->channelCount = clip->data.channelCount;
    outView->keyCount = clip->data.keyCount;
    outView->jointCount = clip->data.jointCount;
    return 1u;
}

static uint32_t GetChannel(const LaiueAnimationV1 *clip, uint32_t channel,
                           LaiueAnimationChannelV1 *outChannel)
{
    if (clip == NULL || outChannel == NULL || channel >= clip->data.channelCount)
        return 0u;
    const AnimationChannel *source = &clip->data.channels[channel];
    outChannel->kind = source->kind;
    outChannel->target = source->target;
    outChannel->interpolation = source->interpolation;
    outChannel->keyCount = source->keyCount;
    return 1u;
}

static void Release(LaiueAnimationV1 *clip)
{
    PlatformFree(clip);
}

static uint32_t EnumeratePacks(LaiueContentCatalog *catalog, LaiueAnimationListV1 *outList)
{
    if (outList == NULL)
        return 0u;
    outList->entries = NULL;
    outList->count = 0u;
    catalog = CatalogOrDefault(catalog);
    if (g_content == NULL || catalog == NULL || g_content->enumerate == NULL ||
        g_content->releaseList == NULL)
        return 0u;
    LaiueContentList source = {0};
    if (g_content->enumerate(catalog, LAIUE_CONTENT_ANIMATION_PACK, &source) == 0u)
        return 0u;
    uint32_t result = 1u;
    if (source.count != 0u)
    {
        outList->entries = (LaiueAnimationNameV1 *)PlatformAllocate(
            (size_t)source.count * sizeof(LaiueAnimationNameV1), true);
        if (outList->entries == NULL)
            result = 0u;
        else
        {
            for (uint32_t index = 0u; index < source.count; ++index)
            {
                memcpy(outList->entries[index].name, source.entries[index].name,
                       sizeof(outList->entries[index].name));
                outList->entries[index].active = source.entries[index].active ? 1u : 0u;
            }
            outList->count = source.count;
        }
    }
    g_content->releaseList(&source);
    return result;
}

static uint32_t ActivatePack(LaiueContentCatalog *catalog, const wchar_t *name)
{
    catalog = CatalogOrDefault(catalog);
    return g_content != NULL && catalog != NULL && g_content->setActivePack != NULL
               ? g_content->setActivePack(catalog, LAIUE_CONTENT_ANIMATION_PACK, name)
               : 0u;
}

static void ReleaseList(LaiueAnimationListV1 *list)
{
    if (list == NULL)
        return;
    PlatformFree(list->entries);
    list->entries = NULL;
    list->count = 0u;
}

static bool Append(wchar_t *out, uint32_t capacity, const wchar_t *prefix, const wchar_t *name)
{
    uint32_t length = 0u;
    for (uint32_t index = 0u; prefix[index] != 0; ++index)
    {
        if (length + 1u >= capacity)
            return false;
        out[length++] = prefix[index];
    }
    if (length != 0u)
    {
        if (length + 1u >= capacity)
            return false;
        out[length++] = L'/';
    }
    for (uint32_t index = 0u; name[index] != 0; ++index)
    {
        if (length + 1u >= capacity)
            return false;
        out[length++] = name[index];
    }
    out[length] = 0;
    return true;
}

typedef struct ScanFrame
{
    PlatformDirectoryIterator iterator;
    PlatformDirectoryEntry entry;
    wchar_t directory[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t name[LAIUE_CONTENT_NAME_CAPACITY];
} ScanFrame;

typedef struct Scan
{
    LaiueAnimationNameV1 *entries;
    uint32_t capacity;
    uint32_t count;
    uint32_t failed;
} Scan;

static void ScanClips(const wchar_t *directory, const wchar_t *prefix, uint32_t depth, Scan *scan)
{
    if (depth >= LAIUE_CONTENT_PATH_SEGMENT_MAX || scan->failed != 0u)
        return;
    /* Heap frames keep no-CRT stacks below the __chkstk threshold. */
    ScanFrame *frame = (ScanFrame *)PlatformAllocate(sizeof(*frame), false);
    if (frame == NULL || !PlatformDirectoryOpen(&frame->iterator, directory))
    {
        scan->failed = 1u;
        PlatformFree(frame);
        return;
    }
    while (scan->failed == 0u && PlatformDirectoryNext(&frame->iterator, &frame->entry))
    {
        if (frame->entry.isSymbolicLink || g_content->nameIsSafe(frame->entry.name) == 0u)
            continue;
        if (!Append(frame->name, LAIUE_CONTENT_NAME_CAPACITY, prefix, frame->entry.name))
            continue;
        if (frame->entry.isDirectory)
        {
            if (Append(frame->directory, LAIUE_CONTENT_PATH_CAPACITY, directory, frame->entry.name))
                ScanClips(frame->directory, frame->name, depth + 1u, scan);
            continue;
        }
        uint32_t length = 0u;
        while (frame->name[length] != 0)
            ++length;
        if (length <= 3u || frame->name[length - 3u] != L'.' || frame->name[length - 2u] != L'l' ||
            frame->name[length - 1u] != L'k')
            continue;
        frame->name[length - 3u] = 0;
        if (scan->entries != NULL)
        {
            if (scan->count >= scan->capacity)
            {
                scan->failed = 1u; /* tree grew between count/fill; no partial success */
                break;
            }
            memcpy(scan->entries[scan->count].name, frame->name,
                   sizeof(scan->entries[scan->count].name));
            scan->entries[scan->count].active = 0u;
        }
        if (scan->count == UINT32_MAX)
            scan->failed = 1u;
        else
            ++scan->count;
    }
    PlatformDirectoryClose(&frame->iterator);
    PlatformFree(frame);
}

static uint32_t EnumerateClips(LaiueContentCatalog *catalog, LaiueAnimationListV1 *outList)
{
    if (outList == NULL)
        return 0u;
    outList->entries = NULL;
    outList->count = 0u;
    catalog = CatalogOrDefault(catalog);
    if (g_content == NULL || catalog == NULL || g_content->getActivePack == NULL ||
        g_content->buildPath == NULL || g_content->nameIsSafe == NULL)
        return 0u;
    wchar_t active[LAIUE_CONTENT_NAME_CAPACITY];
    if (g_content->getActivePack(catalog, LAIUE_CONTENT_ANIMATION_PACK, active,
                                 LAIUE_CONTENT_NAME_CAPACITY) == 0u)
        return 1u;
    wchar_t *root =
        (wchar_t *)PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (root == NULL)
        return 0u;
    uint32_t result = 0u;
    if (g_content->buildPath(catalog, LAIUE_CONTENT_ANIMATION_PACK, active, NULL, root,
                             LAIUE_CONTENT_PATH_CAPACITY) != 0u)
    {
        Scan count = {0};
        ScanClips(root, L"", 0u, &count);
        if (count.failed == 0u)
        {
            if (count.count == 0u)
                result = 1u;
            else
            {
                LaiueAnimationNameV1 *entries = (LaiueAnimationNameV1 *)PlatformAllocate(
                    (size_t)count.count * sizeof(LaiueAnimationNameV1), true);
                if (entries != NULL)
                {
                    Scan fill = {entries, count.count, 0u, 0u};
                    ScanClips(root, L"", 0u, &fill);
                    if (fill.failed == 0u)
                    {
                        outList->entries = entries;
                        outList->count = fill.count;
                        result = 1u;
                    }
                    else
                        PlatformFree(entries);
                }
            }
        }
    }
    PlatformFree(root);
    return result;
}

static const LaiueAnimationServiceV1 g_service = {
    .structSize = sizeof(LaiueAnimationServiceV1),
    .abiVersion = LAIUE_ANIMATION_SERVICE_ABI_VERSION_1,
    .enumeratePacks = EnumeratePacks,
    .activatePack = ActivatePack,
    .enumerateClips = EnumerateClips,
    .releaseList = ReleaseList,
    .loadFrom = LoadFrom,
    .loadFile = LoadFile,
    .loadMemory = LoadMemory,
    .getView = GetView,
    .getChannel = GetChannel,
    .release = Release,
    .sampleChannel = AnimationSampleChannel,
    .samplePose = AnimationSamplePose,
    .skinVertices = AnimationSkinVertices,
};

const LaiueAnimationServiceV1 *LaiueAnimationGetStaticServiceV1(void)
{
    return &g_service;
}
