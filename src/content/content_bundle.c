#include "content/content_bundle.h"
#include "content/content_catalog.h"
#include "platform/system.h"

#include <string.h>

#define BUNDLE_MAGIC 0x3142434cU /* LCB1 */
#define BUNDLE_VERSION 2U
#define BUNDLE_LEGACY_VERSION 1U
#define BUNDLE_HEADER_SIZE 16U
#define BUNDLE_RECORD_HEADER_SIZE 12U
#define BUNDLE_MAX_FILES 4096U
#define BUNDLE_MAX_PATH_UTF8 512U
#define BUNDLE_MAX_DEPTH 12U
#define BUNDLE_INITIAL_CAPACITY 65536U
#define BUNDLE_DIRECTORY_MAX_ENTRIES 4096U

typedef struct BundleWriter
{
    uint8_t* bytes;
    uint64_t size;
    uint64_t capacity;
    uint32_t files;
} BundleWriter;

typedef struct BundleRoot
{
    LaiueContentType type;
    wchar_t name[128];
    bool directory;
} BundleRoot;

static wchar_t* AllocatePathBuffer(void)
{
    return PlatformAllocate(
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
}

static void WriteU16(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void WriteU32(uint8_t* output, uint32_t value)
{
    for (uint32_t i = 0; i < 4U; ++i)
        output[i] = (uint8_t)(value >> (i * 8U));
}

static void WriteU64(uint8_t* output, uint64_t value)
{
    for (uint32_t i = 0; i < 8U; ++i)
        output[i] = (uint8_t)(value >> (i * 8U));
}

static uint16_t ReadU16(const uint8_t* input)
{
    return (uint16_t)(input[0] | ((uint16_t)input[1] << 8U));
}

static uint32_t ReadU32(const uint8_t* input)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4U; ++i)
        value |= (uint32_t)input[i] << (i * 8U);
    return value;
}

static uint64_t ReadU64(const uint8_t* input)
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8U; ++i)
        value |= (uint64_t)input[i] << (i * 8U);
    return value;
}

static uint32_t WideLength(const wchar_t* text)
{
    uint32_t length = 0;
    while (text[length] != L'\0') ++length;
    return length;
}

static int32_t WideCompareInsensitive(
    const wchar_t* left, const wchar_t* right)
{
    uint32_t index = 0;
    for (;;)
    {
        wchar_t a = left[index];
        wchar_t b = right[index];
        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + (L'a' - L'A'));
        if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + (L'a' - L'A'));
        if (a != b) return a < b ? -1 : 1;
        if (a == L'\0') return 0;
        ++index;
    }
}

static bool AppendWide(wchar_t* output, uint32_t capacity,
    uint32_t* length, const wchar_t* text)
{
    while (*text != L'\0')
    {
        if (*length + 1U >= capacity) return false;
        output[(*length)++] = *text++;
    }
    output[*length] = L'\0';
    return true;
}

static bool JoinPath(const wchar_t* parent, const wchar_t* child,
    wchar_t* output, uint32_t capacity)
{
    uint32_t parentLength = WideLength(parent);
    uint32_t childLength = WideLength(child);
    if (parentLength + childLength + 2U > capacity) return false;
    memcpy(output, parent, (size_t)parentLength * sizeof(wchar_t));
    output[parentLength] = L'/';
    memcpy(output + parentLength + 1U, child,
        (size_t)(childLength + 1U) * sizeof(wchar_t));
    return true;
}

static bool WriterReserve(BundleWriter* writer, uint64_t additional)
{
    if (additional > LAIUE_CONTENT_BUNDLE_MAX_BYTES - writer->size)
        return false;
    uint64_t required = writer->size + additional;
    if (required <= writer->capacity) return true;
    uint64_t capacity = writer->capacity;
    while (capacity < required)
    {
        uint64_t next = capacity * 2U;
        capacity = next > LAIUE_CONTENT_BUNDLE_MAX_BYTES
            ? LAIUE_CONTENT_BUNDLE_MAX_BYTES : next;
        if (capacity < required
            && capacity == LAIUE_CONTENT_BUNDLE_MAX_BYTES)
            return false;
    }
    uint8_t* resized = PlatformReallocate(
        writer->bytes, (size_t)capacity, false);
    if (resized == NULL) return false;
    writer->bytes = resized;
    writer->capacity = capacity;
    return true;
}

static bool AddFile(BundleWriter* writer, LaiueContentType type,
    const wchar_t* relativePath, const wchar_t* absolutePath)
{
    if (writer->files >= BUNDLE_MAX_FILES) return false;
    char utf8[BUNDLE_MAX_PATH_UTF8];
    uint32_t pathLength = 0;
    if (!PlatformWideToUtf8(relativePath, utf8,
            BUNDLE_MAX_PATH_UTF8, &pathLength)
        || pathLength == 0 || pathLength > UINT16_MAX)
        return false;
    for (uint32_t i = 0; i < pathLength; ++i)
        if (utf8[i] == '\\') utf8[i] = '/';

    uint8_t* fileBytes = NULL;
    uint64_t fileSize = 0;
    if (!PlatformReadEntireFile(absolutePath,
            LAIUE_CONTENT_BUNDLE_MAX_BYTES, &fileBytes, &fileSize)
        || !WriterReserve(writer,
            BUNDLE_RECORD_HEADER_SIZE + pathLength + fileSize))
    {
        PlatformFree(fileBytes);
        return false;
    }

    uint8_t* header = writer->bytes + writer->size;
    header[0] = (uint8_t)type;
    header[1] = 0;
    WriteU16(header + 2U, (uint16_t)pathLength);
    WriteU64(header + 4U, fileSize);
    writer->size += BUNDLE_RECORD_HEADER_SIZE;
    memcpy(writer->bytes + writer->size, utf8, pathLength);
    writer->size += pathLength;
    if (fileSize != 0)
    {
        memcpy(writer->bytes + writer->size, fileBytes, (size_t)fileSize);
        writer->size += fileSize;
    }
    PlatformFree(fileBytes);
    ++writer->files;
    return true;
}

static void SortDirectoryEntries(
    PlatformDirectoryEntry* entries, uint32_t count)
{
    for (uint32_t i = 1; i < count; ++i)
    {
        PlatformDirectoryEntry value = entries[i];
        uint32_t position = i;
        while (position > 0 && WideCompareInsensitive(
            value.name, entries[position - 1U].name) < 0)
        {
            entries[position] = entries[position - 1U];
            --position;
        }
        entries[position] = value;
    }
}

static bool AddDirectory(BundleWriter* writer, LaiueContentType type,
    const wchar_t* absolute, const wchar_t* relative, uint32_t depth)
{
    if (depth >= BUNDLE_MAX_DEPTH) return false;
    PlatformDirectoryEntry* entries = PlatformAllocate(
        BUNDLE_DIRECTORY_MAX_ENTRIES * sizeof(*entries), false);
    PlatformDirectoryIterator* iterator =
        PlatformAllocate(sizeof(*iterator), false);
    PlatformDirectoryEntry* entry =
        PlatformAllocate(sizeof(*entry), false);
    if (entries == NULL || iterator == NULL || entry == NULL)
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        PlatformFree(entries);
        return false;
    }
    if (!PlatformDirectoryOpen(iterator, absolute))
    {
        PlatformFree(entry);
        PlatformFree(iterator);
        PlatformFree(entries);
        return false;
    }
    uint32_t count = 0;
    bool succeeded = true;
    while (PlatformDirectoryNext(iterator, entry))
    {
        if (entry->isSymbolicLink
            || !LaiueContentNameIsSafe(entry->name)
            || count >= BUNDLE_DIRECTORY_MAX_ENTRIES)
        {
            succeeded = false;
            break;
        }
        entries[count++] = *entry;
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(entry);
    PlatformFree(iterator);
    if (!succeeded)
    {
        PlatformFree(entries);
        return false;
    }
    SortDirectoryEntries(entries, count);
    for (uint32_t i = 1; i < count; ++i)
    {
        if (WideCompareInsensitive(
                entries[i - 1U].name, entries[i].name) == 0)
        {
            PlatformFree(entries);
            return false;
        }
    }

    wchar_t* childAbsolute = AllocatePathBuffer();
    wchar_t* childRelative = AllocatePathBuffer();
    if (childAbsolute == NULL || childRelative == NULL)
    {
        PlatformFree(childRelative);
        PlatformFree(childAbsolute);
        PlatformFree(entries);
        return false;
    }
    for (uint32_t i = 0; i < count && succeeded; ++i)
    {
        succeeded = JoinPath(absolute, entries[i].name,
                childAbsolute, LAIUE_CONTENT_PATH_CAPACITY)
            && JoinPath(relative, entries[i].name,
                childRelative, LAIUE_CONTENT_PATH_CAPACITY);
        if (!succeeded) break;
        succeeded = entries[i].isDirectory
            ? AddDirectory(writer, type, childAbsolute,
                childRelative, depth + 1U)
            : AddFile(writer, type, childRelative, childAbsolute);
    }
    PlatformFree(childRelative);
    PlatformFree(childAbsolute);
    PlatformFree(entries);
    return succeeded;
}

static bool SourceTypeValid(LaiueContentType type)
{
    return type == LAIUE_CONTENT_MOD_PACK
        || type == LAIUE_CONTENT_SHADER_PACK
        || type == LAIUE_CONTENT_TEXTURE_PACK;
}

bool LaiueContentBundleBuild(const LaiueContentBundleSource* sources,
    uint32_t sourceCount, LaiueContentBundle* output)
{
    if (output == NULL || sourceCount > LAIUE_CONTENT_BUNDLE_MAX_SOURCES
        || (sourceCount != 0 && sources == NULL))
        return false;
    memset(output, 0, sizeof(*output));
    BundleWriter writer = {
        .bytes = PlatformAllocate(BUNDLE_INITIAL_CAPACITY, true),
        .size = BUNDLE_HEADER_SIZE,
        .capacity = BUNDLE_INITIAL_CAPACITY,
    };
    if (writer.bytes == NULL) return false;

    bool succeeded = true;
    wchar_t* path = AllocatePathBuffer();
    if (path == NULL)
    {
        PlatformFree(writer.bytes);
        return false;
    }
    for (uint32_t i = 0; i < sourceCount && succeeded; ++i)
    {
        if (!SourceTypeValid(sources[i].type)
            || !LaiueContentNameIsSafe(sources[i].name)
            || !LaiueContentNameMatches(sources[i].type, sources[i].name)
            || !LaiueContentBuildPath(sources[i].type, sources[i].name,
                NULL, path, LAIUE_CONTENT_PATH_CAPACITY))
        {
            succeeded = false;
            break;
        }
        PlatformPathInformation information;
        succeeded = PlatformGetPathInformation(path, &information)
            && information.exists && !information.isSymbolicLink;
        if (!succeeded) break;
        succeeded = information.isDirectory
            ? AddDirectory(&writer, sources[i].type,
                path, sources[i].name, 0)
            : AddFile(&writer, sources[i].type, sources[i].name, path);
    }
    PlatformFree(path);
    if (succeeded)
    {
        WriteU32(writer.bytes, BUNDLE_MAGIC);
        WriteU16(writer.bytes + 4U, BUNDLE_VERSION);
        WriteU16(writer.bytes + 6U, (uint16_t)writer.files);
        WriteU64(writer.bytes + 8U, writer.size);
        succeeded = PlatformSha256(
            writer.bytes, writer.size, output->sha256);
    }
    if (!succeeded)
    {
        PlatformFree(writer.bytes);
        return false;
    }
    output->bytes = writer.bytes;
    output->size = writer.size;
    return true;
}

void LaiueContentBundleRelease(LaiueContentBundle* bundle)
{
    if (bundle == NULL) return;
    PlatformFree(bundle->bytes);
    memset(bundle, 0, sizeof(*bundle));
}

static bool DecodeSafePath(const uint8_t* bytes, uint16_t length,
    wchar_t* output, uint32_t capacity, wchar_t* top, uint32_t topCapacity)
{
    if (length == 0 || length >= BUNDLE_MAX_PATH_UTF8
        || !PlatformUtf8ToWide(
            (const char*)bytes, length, output, capacity, NULL))
        return false;
    uint32_t written = WideLength(output);
    uint32_t segmentStart = 0;
    uint32_t topLength = 0;
    for (uint32_t i = 0; i <= written; ++i)
    {
        if (output[i] != L'/' && output[i] != L'\\'
            && output[i] != L'\0')
            continue;
        wchar_t saved = output[i];
        output[i] = L'\0';
        if (!LaiueContentNameIsSafe(output + segmentStart)) return false;
        if (segmentStart == 0)
        {
            topLength = i;
            if (topLength + 1U > topCapacity) return false;
            memcpy(top, output,
                ((size_t)topLength + 1U) * sizeof(wchar_t));
        }
        output[i] = saved == L'\\' ? L'/' : saved;
        segmentStart = i + 1U;
    }
    return topLength != 0;
}

static bool RootEquals(const BundleRoot* root, LaiueContentType type,
    const wchar_t* name)
{
    return root->type == type
        && WideCompareInsensitive(root->name, name) == 0;
}

static bool BundlePathAlreadySeen(const uint8_t* bytes, uint32_t beforeIndex,
    LaiueContentType type, const wchar_t* path)
{
    uint64_t offset = BUNDLE_HEADER_SIZE;
    wchar_t previousPath[384];
    wchar_t previousTop[128];
    for (uint32_t i = 0; i < beforeIndex; ++i)
    {
        LaiueContentType previousType = (LaiueContentType)bytes[offset];
        uint16_t previousLength = ReadU16(bytes + offset + 2U);
        uint64_t previousSize = ReadU64(bytes + offset + 4U);
        offset += BUNDLE_RECORD_HEADER_SIZE;
        if (previousType == type
            && DecodeSafePath(bytes + offset, previousLength,
                previousPath, 384U, previousTop, 128U)
            && WideCompareInsensitive(previousPath, path) == 0)
            return true;
        offset += previousLength + previousSize;
    }
    return false;
}

static bool ValidateBundle(const uint8_t* bytes, uint64_t size,
    BundleRoot* roots, uint32_t* rootCount)
{
    if (bytes == NULL || size < BUNDLE_HEADER_SIZE
        || size > LAIUE_CONTENT_BUNDLE_MAX_BYTES
        || ReadU32(bytes) != BUNDLE_MAGIC
        || (ReadU16(bytes + 4U) != BUNDLE_VERSION
            && ReadU16(bytes + 4U) != BUNDLE_LEGACY_VERSION)
        || ReadU64(bytes + 8U) != size)
        return false;
    uint32_t fileCount = ReadU16(bytes + 6U);
    if (fileCount > BUNDLE_MAX_FILES) return false;
    uint64_t offset = BUNDLE_HEADER_SIZE;
    *rootCount = 0;
    wchar_t path[384];
    wchar_t top[128];
    for (uint32_t i = 0; i < fileCount; ++i)
    {
        if (size - offset < BUNDLE_RECORD_HEADER_SIZE) return false;
        LaiueContentType type = (LaiueContentType)bytes[offset];
        uint16_t pathLength = ReadU16(bytes + offset + 2U);
        uint64_t fileSize = ReadU64(bytes + offset + 4U);
        offset += BUNDLE_RECORD_HEADER_SIZE;
        if (!SourceTypeValid(type) || pathLength == 0
            || pathLength > size - offset
            || fileSize > size - offset - pathLength
            || !DecodeSafePath(bytes + offset, pathLength,
                path, 384U, top, 128U)
            || !LaiueContentNameMatches(type, top)
            || BundlePathAlreadySeen(bytes, i, type, path))
            return false;
        bool directory = path[WideLength(top)] == L'/';
        uint32_t rootIndex = 0;
        while (rootIndex < *rootCount
            && !RootEquals(&roots[rootIndex], type, top))
            ++rootIndex;
        if (rootIndex == *rootCount)
        {
            if (*rootCount >= LAIUE_CONTENT_BUNDLE_MAX_SOURCES) return false;
            roots[rootIndex].type = type;
            roots[rootIndex].directory = directory;
            memcpy(roots[rootIndex].name, top,
                ((size_t)WideLength(top) + 1U) * sizeof(wchar_t));
            ++*rootCount;
        }
        else if (roots[rootIndex].directory != directory)
            return false;
        offset += pathLength + fileSize;
    }
    return offset == size;
}

static bool BuildSiblingPath(LaiueContentType type, const wchar_t* name,
    const wchar_t* suffix, wchar_t* output, uint32_t capacity)
{
    if (!LaiueContentBuildPath(type, NULL, NULL, output, capacity))
        return false;
    uint32_t length = WideLength(output);
    return AppendWide(output, capacity, &length, L"/")
        && AppendWide(output, capacity, &length, name)
        && AppendWide(output, capacity, &length, suffix);
}

static bool DeleteTree(const wchar_t* path)
{
    PlatformPathInformation information;
    if (!PlatformGetPathInformation(path, &information)) return false;
    if (!information.exists) return true;
    if (information.isSymbolicLink)
        return information.isDirectory
            ? PlatformRemoveDirectory(path) : PlatformDeleteFile(path);
    if (!information.isDirectory) return PlatformDeleteFile(path);

    PlatformDirectoryIterator* iterator =
        PlatformAllocate(sizeof(*iterator), false);
    PlatformDirectoryEntry* entry =
        PlatformAllocate(sizeof(*entry), false);
    wchar_t* child = AllocatePathBuffer();
    if (iterator == NULL || entry == NULL || child == NULL)
    {
        PlatformFree(child);
        PlatformFree(entry);
        PlatformFree(iterator);
        return false;
    }
    if (!PlatformDirectoryOpen(iterator, path))
    {
        PlatformFree(child);
        PlatformFree(entry);
        PlatformFree(iterator);
        return false;
    }
    bool succeeded = true;
    while (PlatformDirectoryNext(iterator, entry))
    {
        if (!JoinPath(path, entry->name,
                child, LAIUE_CONTENT_PATH_CAPACITY)
            || !DeleteTree(child))
        {
            succeeded = false;
            break;
        }
    }
    PlatformDirectoryClose(iterator);
    PlatformFree(child);
    PlatformFree(entry);
    PlatformFree(iterator);
    return succeeded && PlatformRemoveDirectory(path);
}

static bool EnsureParentDirectories(wchar_t* path)
{
    uint32_t length = WideLength(path);
    uint32_t start = path[0] == L'/' ? 1U : 3U;
    for (uint32_t i = start; i < length; ++i)
    {
        if (path[i] != L'/' && path[i] != L'\\') continue;
        wchar_t saved = path[i];
        path[i] = L'\0';
        bool okay = PlatformCreateDirectory(path);
        path[i] = saved;
        if (!okay) return false;
    }
    return true;
}

static bool ExtractBundle(const uint8_t* bytes, uint64_t size,
    const BundleRoot* roots, uint32_t rootCount)
{
    wchar_t* path = AllocatePathBuffer();
    if (path == NULL) return false;
    wchar_t decoded[384];
    wchar_t top[128];
    bool succeeded = true;
    for (uint32_t i = 0; i < rootCount && succeeded; ++i)
    {
        succeeded = BuildSiblingPath(roots[i].type, roots[i].name,
            L".download", path, LAIUE_CONTENT_PATH_CAPACITY)
            && DeleteTree(path);
    }
    uint32_t fileCount = ReadU16(bytes + 6U);
    uint64_t offset = BUNDLE_HEADER_SIZE;
    for (uint32_t i = 0; i < fileCount && succeeded; ++i)
    {
        LaiueContentType type = (LaiueContentType)bytes[offset];
        uint16_t pathLength = ReadU16(bytes + offset + 2U);
        uint64_t fileSize = ReadU64(bytes + offset + 4U);
        offset += BUNDLE_RECORD_HEADER_SIZE;
        succeeded = DecodeSafePath(bytes + offset, pathLength,
            decoded, 384U, top, 128U)
            && BuildSiblingPath(type, top, L".download",
                path, LAIUE_CONTENT_PATH_CAPACITY);
        offset += pathLength;
        uint32_t topLength = WideLength(top);
        if (succeeded && decoded[topLength] == L'/')
        {
            uint32_t length = WideLength(path);
            succeeded = AppendWide(path, LAIUE_CONTENT_PATH_CAPACITY,
                &length, decoded + topLength);
        }
        if (succeeded) succeeded = EnsureParentDirectories(path);
        if (succeeded)
            succeeded = PlatformWriteEntireFile(
                path, bytes + offset, fileSize);
        offset += fileSize;
    }
    PlatformFree(path);
    return succeeded && offset == size;
}

static bool CommitRoots(const BundleRoot* roots, uint32_t rootCount)
{
    wchar_t* final = AllocatePathBuffer();
    wchar_t* staging = AllocatePathBuffer();
    wchar_t* previous = AllocatePathBuffer();
    if (final == NULL || staging == NULL || previous == NULL)
    {
        PlatformFree(previous);
        PlatformFree(staging);
        PlatformFree(final);
        return false;
    }
    bool succeeded = true;
    for (uint32_t i = 0; i < rootCount && succeeded; ++i)
    {
        succeeded = LaiueContentBuildPath(roots[i].type, roots[i].name,
                NULL, final, LAIUE_CONTENT_PATH_CAPACITY)
            && BuildSiblingPath(roots[i].type, roots[i].name, L".download",
                staging, LAIUE_CONTENT_PATH_CAPACITY)
            && BuildSiblingPath(roots[i].type, roots[i].name, L".previous",
                previous, LAIUE_CONTENT_PATH_CAPACITY)
            && DeleteTree(previous);
        bool hadExisting = succeeded && PlatformPathExists(final);
        if (succeeded && hadExisting)
            succeeded = PlatformMoveReplace(final, previous);
        if (succeeded)
        {
            succeeded = PlatformMoveReplace(staging, final);
            if (!succeeded && hadExisting)
                PlatformMoveReplace(previous, final);
        }
    }
    PlatformFree(previous);
    PlatformFree(staging);
    PlatformFree(final);
    return succeeded;
}

bool LaiueContentBundleInstall(const uint8_t* bytes, uint64_t size)
{
    BundleRoot* roots = PlatformAllocate(
        LAIUE_CONTENT_BUNDLE_MAX_SOURCES * sizeof(BundleRoot), true);
    if (roots == NULL) return false;
    uint32_t rootCount = 0;
    bool succeeded = ValidateBundle(bytes, size, roots, &rootCount)
        && ExtractBundle(bytes, size, roots, rootCount)
        && CommitRoots(roots, rootCount);
    PlatformFree(roots);
    return succeeded;
}
