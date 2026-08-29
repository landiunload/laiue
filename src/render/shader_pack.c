#include "render/shader_pack.h"
#include "platform/system.h"

#include <stddef.h>
#include <string.h>

#define SHADER_MANIFEST_MAX_BYTES 4096u

struct ShaderPackLoadedSet
{
    LaiueShaderSet shaderSet;
    void *allocations[LAIUE_SHADER_SLOT_COUNT];
};

typedef enum ShaderFileLoadResult
{
    SHADER_FILE_MISSING = 0,
    SHADER_FILE_LOADED,
    SHADER_FILE_INVALID,
    SHADER_FILE_IO_ERROR,
} ShaderFileLoadResult;

static bool BytesEqual(const uint8_t *left, const char *right, uint32_t count)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (left[index] != (uint8_t)right[index])
            return false;
    }
    return true;
}

static bool HasExactLine(const uint8_t* data, uint32_t length,
    const char* expected, uint32_t expectedLength, bool firstLineOnly)
{
    uint32_t start = 0;
    while (start < length)
    {
        uint32_t end = start;
        while (end < length && data[end] != '\n' && data[end] != '\r') ++end;
        if (end - start == expectedLength && BytesEqual(data + start, expected, expectedLength))
            return true;
        if (firstLineOnly) return false;
        while (end < length && (data[end] == '\n' || data[end] == '\r')) ++end;
        start = end;
    }
    return false;
}

static bool ShaderSetHeaderIsValid(const LaiueShaderSet *shaderSet)
{
    return shaderSet != NULL && shaderSet->structSize >= sizeof(*shaderSet) &&
           shaderSet->contractVersion == LAIUE_SHADER_CONTRACT_VERSION &&
           shaderSet->reserved == 0U &&
           (shaderSet->overrideMask & ~LAIUE_SHADER_ALL_SLOTS_MASK) == 0U;
}

void LaiueShaderSetInitialize(LaiueShaderSet *shaderSet)
{
    if (shaderSet == NULL)
        return;
    memset(shaderSet, 0, sizeof(*shaderSet));
    shaderSet->structSize = sizeof(*shaderSet);
    shaderSet->contractVersion = LAIUE_SHADER_CONTRACT_VERSION;
}

bool LaiueShaderSetSetOverride(LaiueShaderSet *shaderSet, LaiueShaderSlot slot,
                               const void *bytecode, uint32_t sizeBytes)
{
    if (!ShaderSetHeaderIsValid(shaderSet) || (uint32_t)slot >= (uint32_t)LAIUE_SHADER_SLOT_COUNT ||
        bytecode == NULL || sizeBytes == 0U || sizeBytes > LAIUE_SHADER_BYTECODE_MAX_BYTES)
        return false;
    shaderSet->bytecode[slot].bytes = bytecode;
    shaderSet->bytecode[slot].sizeBytes = sizeBytes;
    shaderSet->bytecode[slot].reserved = 0U;
    shaderSet->overrideMask |= LAIUE_SHADER_SLOT_MASK(slot);
    return true;
}

bool LaiueShaderSetClearOverride(LaiueShaderSet *shaderSet, LaiueShaderSlot slot)
{
    if (!ShaderSetHeaderIsValid(shaderSet) || (uint32_t)slot >= (uint32_t)LAIUE_SHADER_SLOT_COUNT)
        return false;
    memset(&shaderSet->bytecode[slot], 0, sizeof(shaderSet->bytecode[slot]));
    shaderSet->overrideMask &= ~LAIUE_SHADER_SLOT_MASK(slot);
    return true;
}

bool LaiueShaderSetIsValid(const LaiueShaderSet *shaderSet)
{
    if (!ShaderSetHeaderIsValid(shaderSet))
        return false;
    for (uint32_t index = 0; index < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++index)
    {
        const LaiueShaderBytecode *bytecode = &shaderSet->bytecode[index];
        bool overridden = (shaderSet->overrideMask & (1U << index)) != 0U;
        if (bytecode->reserved != 0U)
            return false;
        if (overridden)
        {
            if (bytecode->bytes == NULL || bytecode->sizeBytes == 0U ||
                bytecode->sizeBytes > LAIUE_SHADER_BYTECODE_MAX_BYTES)
                return false;
        }
        else if (bytecode->bytes != NULL || bytecode->sizeBytes != 0U)
            return false;
    }
    return true;
}

bool ShaderPackEnumerateFrom(LaiueContentCatalog *catalog, ShaderPackList *outList)
{
    if (catalog == NULL || outList == NULL)
        return false;
    outList->entries = NULL;
    outList->count = 0;

    LaiueContentList contentList;
    if (!LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_SHADER_PACK, &contentList))
        return false;

    outList->entries =
        PlatformAllocate((size_t)(contentList.count + 1U) * sizeof(ShaderPackEntry), true);
    if (outList->entries == NULL)
    {
        LaiueContentListRelease(&contentList);
        return false;
    }
    memcpy(outList->entries[0].name, L"Default", 8U * sizeof(wchar_t));
    bool hasActive = false;
    for (uint32_t sourceIndex = 0;
        sourceIndex < contentList.count; ++sourceIndex)
    {
        uint32_t destinationIndex = sourceIndex + 1U;
        uint32_t length = 0;
        while (contentList.entries[sourceIndex].name[length] != L'\0' &&
               length + 1U < SHADER_PACK_NAME_MAX)
        {
            outList->entries[destinationIndex].name[length] =
                contentList.entries[sourceIndex].name[length];
            ++length;
        }
        outList->entries[destinationIndex].name[length] = L'\0';
        outList->entries[destinationIndex].active =
            contentList.entries[sourceIndex].active;
        hasActive = hasActive || contentList.entries[sourceIndex].active;
    }
    outList->entries[0].active = !hasActive;
    outList->count = contentList.count + 1U;
    LaiueContentListRelease(&contentList);
    return true;
}

bool ShaderPackEnumerate(ShaderPackList *outList)
{
    return ShaderPackEnumerateFrom(LaiueContentCatalogDefault(), outList);
}

void ShaderPackListRelease(ShaderPackList* list)
{
    if (list == NULL)
        return;
    PlatformFree(list->entries);
    list->entries = NULL;
    list->count = 0;
}

bool ShaderPackActivateIn(LaiueContentCatalog *catalog, const wchar_t *name)
{
    return LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_SHADER_PACK,
                                            name == NULL || name[0] == L'\0' ? NULL : name);
}

bool ShaderPackActivate(const wchar_t *name)
{
    return ShaderPackActivateIn(LaiueContentCatalogDefault(), name);
}

static bool IsCompatibleManifest(const wchar_t *fullPath)
{
    PlatformPathInformation information;
    if (!PlatformGetPathInformation(fullPath, &information) || !information.exists ||
        information.isDirectory || information.isSymbolicLink || information.size == 0U ||
        information.size > SHADER_MANIFEST_MAX_BYTES)
        return false;
    uint8_t *data = NULL;
    uint64_t size = 0;
    if (!PlatformReadEntireFile(fullPath, SHADER_MANIFEST_MAX_BYTES, &data, &size) || size == 0U ||
        size > UINT32_MAX)
    {
        PlatformFree(data);
        return false;
    }

    static const char header[] = "LAIUE SHADER 1";
    static const char contract[] = "contract = 1";
    uint32_t byteOffset =
        size >= 3U && data[0] == 0xefU && data[1] == 0xbbU && data[2] == 0xbfU ? 3U : 0U;
    bool compatible = HasExactLine(data + byteOffset, (uint32_t)size - byteOffset, header,
                                   sizeof(header) - 1U, true) &&
                      HasExactLine(data + byteOffset, (uint32_t)size - byteOffset, contract,
                                   sizeof(contract) - 1U, false);
    PlatformFree(data);
    return compatible;
}

static ShaderFileLoadResult LoadShaderFile(const wchar_t *fullPath, void **outData,
                                           uint32_t *outSize)
{
    *outData = NULL;
    *outSize = 0U;
    PlatformPathInformation information;
    if (!PlatformGetPathInformation(fullPath, &information))
        return SHADER_FILE_IO_ERROR;
    if (!information.exists)
        return SHADER_FILE_MISSING;
    if (information.isDirectory || information.isSymbolicLink || information.size == 0U ||
        information.size > LAIUE_SHADER_BYTECODE_MAX_BYTES)
        return SHADER_FILE_INVALID;

    uint8_t *data = NULL;
    uint64_t size = 0;
    if (!PlatformReadEntireFile(fullPath, LAIUE_SHADER_BYTECODE_MAX_BYTES, &data, &size))
        return SHADER_FILE_IO_ERROR;
    if (size == 0U || size > LAIUE_SHADER_BYTECODE_MAX_BYTES || size > UINT32_MAX)
    {
        PlatformFree(data);
        return SHADER_FILE_INVALID;
    }
    *outData = data;
    *outSize = (uint32_t)size;
    return SHADER_FILE_LOADED;
}

const LaiueShaderSet *ShaderPackLoadedSetGet(const ShaderPackLoadedSet *loadedSet)
{
    return loadedSet != NULL ? &loadedSet->shaderSet : NULL;
}

void ShaderPackLoadedSetRelease(ShaderPackLoadedSet *loadedSet)
{
    if (loadedSet == NULL)
        return;
    for (uint32_t index = 0; index < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++index)
        PlatformFree(loadedSet->allocations[index]);
    PlatformFree(loadedSet);
}

ShaderPackLoadedSet *ShaderPackLoadActiveSet(LaiueContentCatalog *catalog,
                                             ShaderPackLoadStatus *outStatus)
{
    if (outStatus != NULL) *outStatus = SHADER_PACK_LOAD_IO_ERROR;
    if (catalog == NULL)
        return NULL;

    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    if (!LaiueContentCatalogGetActivePack(catalog, LAIUE_CONTENT_SHADER_PACK, activeName,
                                          LAIUE_CONTENT_NAME_CAPACITY))
    {
        wchar_t *activePath =
            PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
        bool invalidSelection =
            activePath != NULL &&
            LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_SHADER_PACK, NULL, L"active.txt",
                                         activePath, LAIUE_CONTENT_PATH_CAPACITY) &&
            PlatformPathExists(activePath);
        PlatformFree(activePath);
        if (outStatus != NULL)
            *outStatus = invalidSelection ? SHADER_PACK_LOAD_ACTIVATION_ERROR
                                          : SHADER_PACK_LOAD_NO_ACTIVE_PACK;
        return NULL;
    }

    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL)
        return NULL;
    if (!LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_SHADER_PACK, activeName, L"pack.lm",
                                      path, LAIUE_CONTENT_PATH_CAPACITY) ||
        !IsCompatibleManifest(path))
    {
        PlatformFree(path);
        if (outStatus != NULL)
            *outStatus = SHADER_PACK_LOAD_INVALID_MANIFEST;
        return NULL;
    }

    ShaderPackLoadedSet *loadedSet = PlatformAllocate(sizeof(*loadedSet), true);
    if (loadedSet == NULL)
    {
        PlatformFree(path);
        return NULL;
    }
    LaiueShaderSetInitialize(&loadedSet->shaderSet);

    static const wchar_t *const fileNames[LAIUE_SHADER_SLOT_COUNT] = {
        L"chunk_vs.ls",    L"chunk_ps.ls", L"panorama_vs.ls",
        L"panorama_ps.ls", L"ui_vs.ls",    L"ui_ps.ls",
    };

    bool anyLoaded = false;
    for (uint32_t index = 0; index < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++index)
    {
        if (!LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_SHADER_PACK, activeName,
                                          fileNames[index], path, LAIUE_CONTENT_PATH_CAPACITY))
        {
            PlatformFree(path);
            ShaderPackLoadedSetRelease(loadedSet);
            return NULL;
        }

        void *data = NULL;
        uint32_t size = 0;
        ShaderFileLoadResult result = LoadShaderFile(path, &data, &size);
        if (result == SHADER_FILE_MISSING)
            continue;
        if (result != SHADER_FILE_LOADED)
        {
            PlatformFree(path);
            ShaderPackLoadedSetRelease(loadedSet);
            if (outStatus != NULL)
                *outStatus = result == SHADER_FILE_INVALID ? SHADER_PACK_LOAD_INVALID_SHADER
                                                           : SHADER_PACK_LOAD_IO_ERROR;
            return NULL;
        }
        loadedSet->allocations[index] = data;
        if (!LaiueShaderSetSetOverride(&loadedSet->shaderSet, (LaiueShaderSlot)index, data, size))
        {
            PlatformFree(path);
            ShaderPackLoadedSetRelease(loadedSet);
            if (outStatus != NULL)
                *outStatus = SHADER_PACK_LOAD_INVALID_SHADER;
            return NULL;
        }
        anyLoaded = true;
    }
    PlatformFree(path);

    if (!anyLoaded)
    {
        ShaderPackLoadedSetRelease(loadedSet);
        if (outStatus != NULL) *outStatus = SHADER_PACK_LOAD_EMPTY;
        return NULL;
    }
    if (outStatus != NULL)
        *outStatus = SHADER_PACK_LOAD_OK;
    return loadedSet;
}

bool ShaderPackLoadActiveBytecode(void **outChunkVS, uint32_t *outChunkVSLength, void **outChunkPS,
                                  uint32_t *outChunkPSLength, void **outPanoramaVS,
                                  uint32_t *outPanoramaVSLength, void **outPanoramaPS,
                                  uint32_t *outPanoramaPSLength, void **outUIVS,
                                  uint32_t *outUIVSLength, void **outUIPS, uint32_t *outUIPSLength,
                                  ShaderPackLoadStatus *outStatus)
{
    void **outputs[LAIUE_SHADER_SLOT_COUNT] = {
        outChunkVS, outChunkPS, outPanoramaVS, outPanoramaPS, outUIVS, outUIPS,
    };
    uint32_t *lengths[LAIUE_SHADER_SLOT_COUNT] = {
        outChunkVSLength,    outChunkPSLength, outPanoramaVSLength,
        outPanoramaPSLength, outUIVSLength,    outUIPSLength,
    };
    for (uint32_t index = 0; index < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++index)
    {
        if (outputs[index] == NULL || lengths[index] == NULL)
        {
            if (outStatus != NULL)
                *outStatus = SHADER_PACK_LOAD_IO_ERROR;
            return false;
        }
        *outputs[index] = NULL;
        *lengths[index] = 0U;
    }

    ShaderPackLoadedSet *loadedSet =
        ShaderPackLoadActiveSet(LaiueContentCatalogDefault(), outStatus);
    if (loadedSet == NULL)
        return false;

    for (uint32_t index = 0; index < (uint32_t)LAIUE_SHADER_SLOT_COUNT; ++index)
    {
        if ((loadedSet->shaderSet.overrideMask & (1U << index)) == 0U)
            continue;
        *outputs[index] = loadedSet->allocations[index];
        *lengths[index] = loadedSet->shaderSet.bytecode[index].sizeBytes;
        loadedSet->allocations[index] = NULL;
    }
    ShaderPackLoadedSetRelease(loadedSet);
    return true;
}

void ShaderPackBytecodeRelease(void *bytecode)
{
    PlatformFree(bytecode);
}
