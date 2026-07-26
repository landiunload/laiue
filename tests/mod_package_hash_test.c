#include "test_runtime.h"

// Use the production manifest reader and compatibility-hash implementation
// without widening the public laiue_mod ABI for a packaging-only verifier.
#include "../src/mod/mods.c"

#define HASH_LINE_CAPACITY \
    (MODS_ID_CAPACITY + 1U + MODS_CONTENT_HASH_SIZE * 2U + 1U)

static int32_t CompareIds(const char* left, const char* right)
{
    uint32_t index = 0;
    while (left[index] != '\0' &&
           left[index] == right[index])
    {
        ++index;
    }
    return (uint8_t)left[index] < (uint8_t)right[index]
        ? -1
        : (uint8_t)left[index] > (uint8_t)right[index]
            ? 1 : 0;
}

static bool HashIsNonzero(
    const uint8_t hash[MODS_CONTENT_HASH_SIZE])
{
    uint8_t combined = 0;
    for (uint32_t index = 0;
         index < MODS_CONTENT_HASH_SIZE; ++index)
    {
        combined |= hash[index];
    }
    return combined != 0;
}

static bool AppendCharacter(
    char* output, uint32_t capacity,
    uint32_t* length, char value)
{
    if (*length + 1U >= capacity)
    {
        return false;
    }
    output[(*length)++] = value;
    output[*length] = '\0';
    return true;
}

static bool AppendAscii(
    char* output, uint32_t capacity,
    uint32_t* length, const char* value)
{
    for (uint32_t index = 0; value[index] != '\0'; ++index)
    {
        if (!AppendCharacter(
                output, capacity, length, value[index]))
        {
            return false;
        }
    }
    return true;
}

static bool AppendHash(
    char* output, uint32_t capacity, uint32_t* length,
    const uint8_t hash[MODS_CONTENT_HASH_SIZE])
{
    static const char hexadecimal[] = "0123456789abcdef";
    for (uint32_t index = 0;
         index < MODS_CONTENT_HASH_SIZE; ++index)
    {
        if (!AppendCharacter(
                output, capacity, length,
                hexadecimal[hash[index] >> 4U]) ||
            !AppendCharacter(
                output, capacity, length,
                hexadecimal[hash[index] & 0x0fU]))
        {
            return false;
        }
    }
    return true;
}

LAIUE_TEST_ENTRY(ModPackageHashTestEntryPoint)
{
    ModsState* mods = PlatformAllocate(sizeof(*mods), true);
    if (mods == NULL)
    {
        LaiueTestRuntimeWrite(
            "mod package hash: allocation failed\r\n");
        LaiueTestRuntimeExit(1);
    }
    ModsInit(mods, L"mod_package_hash_enabled.txt");
    ModsRefresh(mods);
    if (mods->count == 0)
    {
        PlatformFree(mods);
        LaiueTestRuntimeWrite(
            "mod package hash: no packages found\r\n");
        LaiueTestRuntimeExit(1);
    }

    const ModEntry* sorted[MODS_MAX_ENTRIES];
    for (uint32_t index = 0; index < mods->count; ++index)
    {
        sorted[index] = &mods->entries[index];
    }
    for (uint32_t index = 1; index < mods->count; ++index)
    {
        const ModEntry* value = sorted[index];
        uint32_t destination = index;
        while (destination != 0 &&
               CompareIds(
                   value->id,
                   sorted[destination - 1U]->id) < 0)
        {
            sorted[destination] =
                sorted[destination - 1U];
            --destination;
        }
        sorted[destination] = value;
    }

    const uint32_t outputCapacity =
        MODS_MAX_ENTRIES * HASH_LINE_CAPACITY + 1U;
    char *output = PlatformAllocate(outputCapacity, false);
    if (output == NULL)
    {
        PlatformFree(mods);
        LaiueTestRuntimeWrite(
            "mod package hash: output allocation failed\r\n");
        LaiueTestRuntimeExit(1);
    }
    uint32_t length = 0;
    output[0] = '\0';
    for (uint32_t index = 0; index < mods->count; ++index)
    {
        const ModEntry* entry = sorted[index];
        if (!entry->compatible ||
            !HashIsNonzero(entry->contentHash) ||
            !AppendAscii(
                output, outputCapacity, &length, entry->id) ||
            !AppendCharacter(
                output, outputCapacity, &length, '|') ||
            !AppendHash(
                output, outputCapacity, &length,
                entry->contentHash) ||
            !AppendCharacter(
                output, outputCapacity, &length, '\n'))
        {
            PlatformFree(output);
            PlatformFree(mods);
            LaiueTestRuntimeWrite(
                "mod package hash: incompatible package\r\n");
            LaiueTestRuntimeExit(1);
        }
    }
    PlatformFree(mods);

    const uint32_t utf8PathCapacity =
        LAIUE_PLATFORM_PATH_CAPACITY * 4U;
    char *outputPathUtf8 =
        PlatformAllocate(utf8PathCapacity, false);
    if (outputPathUtf8 == NULL)
    {
        PlatformFree(output);
        LaiueTestRuntimeWrite(
            "mod package hash: path allocation failed\r\n");
        LaiueTestRuntimeExit(1);
    }
    uint32_t outputPathLength = PlatformGetEnvironmentUtf8(
        "LAIUE_MOD_HASH_OUTPUT",
        outputPathUtf8, utf8PathCapacity);
    if (outputPathLength != 0)
    {
        wchar_t *outputPath = PlatformAllocate(
            (size_t)LAIUE_PLATFORM_PATH_CAPACITY *
                sizeof(*outputPath),
            false);
        uint32_t wideLength = 0;
        bool wroteOutput =
            outputPath != NULL &&
            PlatformUtf8ToWide(
                outputPathUtf8, outputPathLength,
                outputPath, LAIUE_PLATFORM_PATH_CAPACITY,
                &wideLength) &&
            wideLength != 0 &&
            PlatformWriteEntireFile(
                outputPath, output, length);
        PlatformFree(outputPath);
        if (!wroteOutput)
        {
            PlatformFree(outputPathUtf8);
            PlatformFree(output);
            LaiueTestRuntimeWrite(
                "mod package hash: output write failed\r\n");
            LaiueTestRuntimeExit(1);
        }
    }
    PlatformFree(outputPathUtf8);

    LaiueTestRuntimeWrite(output);
    PlatformFree(output);
    LAIUE_TEST_SUCCESS();
}
