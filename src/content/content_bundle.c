#include "content/content_bundle.h"
#include "content/content_catalog.h"

#include <windows.h>
#include <bcrypt.h>
#include <string.h>

#define BUNDLE_MAGIC 0x3142434cU /* LCB1 */
#define BUNDLE_VERSION 1U
#define BUNDLE_HEADER_SIZE 16U
#define BUNDLE_RECORD_HEADER_SIZE 12U
#define BUNDLE_MAX_FILES 4096U
#define BUNDLE_MAX_PATH_UTF8 512U
#define BUNDLE_MAX_DEPTH 12U
#define BUNDLE_INITIAL_CAPACITY 65536U

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

static void WriteU16(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void WriteU32(uint8_t* output, uint32_t value)
{
    for (uint32_t i = 0; i < 4U; ++i) output[i] = (uint8_t)(value >> (i * 8U));
}

static void WriteU64(uint8_t* output, uint64_t value)
{
    for (uint32_t i = 0; i < 8U; ++i) output[i] = (uint8_t)(value >> (i * 8U));
}

static uint16_t ReadU16(const uint8_t* input)
{
    return (uint16_t)(input[0] | ((uint16_t)input[1] << 8));
}

static uint32_t ReadU32(const uint8_t* input)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4U; ++i) value |= (uint32_t)input[i] << (i * 8U);
    return value;
}

static uint64_t ReadU64(const uint8_t* input)
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8U; ++i) value |= (uint64_t)input[i] << (i * 8U);
    return value;
}

static uint32_t WideLength(const wchar_t* text)
{
    uint32_t length = 0;
    while (text[length] != L'\0') ++length;
    return length;
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

static bool WriterReserve(BundleWriter* writer, uint64_t additional)
{
    if (additional > LAIUE_CONTENT_BUNDLE_MAX_BYTES - writer->size) return false;
    uint64_t required = writer->size + additional;
    if (required <= writer->capacity) return true;
    uint64_t capacity = writer->capacity;
    while (capacity < required)
    {
        uint64_t next = capacity * 2U;
        capacity = next > LAIUE_CONTENT_BUNDLE_MAX_BYTES
            ? LAIUE_CONTENT_BUNDLE_MAX_BYTES : next;
        if (capacity < required && capacity == LAIUE_CONTENT_BUNDLE_MAX_BYTES)
        {
            return false;
        }
    }
    uint8_t* resized = HeapReAlloc(GetProcessHeap(), 0,
        writer->bytes, (size_t)capacity);
    if (resized == NULL) return false;
    writer->bytes = resized;
    writer->capacity = capacity;
    return true;
}

static bool ComputeSha256(const uint8_t* bytes, uint64_t size,
    uint8_t output[LAIUE_CONTENT_BUNDLE_HASH_SIZE])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    bool succeeded = BCryptOpenAlgorithmProvider(&algorithm,
        BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0
        && BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0;
    uint64_t offset = 0;
    while (succeeded && offset < size)
    {
        ULONG part = size - offset > 0xffffffffULL
            ? 0xffffffffU : (ULONG)(size - offset);
        succeeded = BCryptHashData(hash, (PUCHAR)(bytes + offset), part, 0) >= 0;
        offset += part;
    }
    if (succeeded)
    {
        succeeded = BCryptFinishHash(hash, output,
            LAIUE_CONTENT_BUNDLE_HASH_SIZE, 0) >= 0;
    }
    if (hash != NULL) BCryptDestroyHash(hash);
    if (algorithm != NULL) BCryptCloseAlgorithmProvider(algorithm, 0);
    return succeeded;
}

static bool AddFile(BundleWriter* writer, LaiueContentType type,
    const wchar_t* relativePath, const wchar_t* absolutePath)
{
    if (writer->files >= BUNDLE_MAX_FILES) return false;
    char utf8[BUNDLE_MAX_PATH_UTF8];
    int32_t pathLength = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        relativePath, -1, utf8, BUNDLE_MAX_PATH_UTF8, NULL, NULL);
    if (pathLength <= 1 || pathLength > (int32_t)BUNDLE_MAX_PATH_UTF8)
    {
        return false;
    }
    --pathLength;

    HANDLE file = CreateFileW(absolutePath, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fileSize;
    bool succeeded = GetFileSizeEx(file, &fileSize)
        && fileSize.QuadPart >= 0
        && (uint64_t)fileSize.QuadPart <= LAIUE_CONTENT_BUNDLE_MAX_BYTES
        && WriterReserve(writer, BUNDLE_RECORD_HEADER_SIZE
            + (uint32_t)pathLength + (uint64_t)fileSize.QuadPart);
    if (succeeded)
    {
        uint8_t* header = writer->bytes + writer->size;
        header[0] = (uint8_t)type;
        header[1] = 0;
        WriteU16(header + 2, (uint16_t)pathLength);
        WriteU64(header + 4, (uint64_t)fileSize.QuadPart);
        writer->size += BUNDLE_RECORD_HEADER_SIZE;
        memcpy(writer->bytes + writer->size, utf8, (size_t)pathLength);
        writer->size += (uint32_t)pathLength;
        uint64_t remaining = (uint64_t)fileSize.QuadPart;
        while (remaining != 0)
        {
            DWORD part = remaining > 1024U * 1024U
                ? 1024U * 1024U : (DWORD)remaining;
            DWORD read = 0;
            if (!ReadFile(file, writer->bytes + writer->size, part, &read, NULL)
                || read != part)
            {
                succeeded = false;
                break;
            }
            writer->size += read;
            remaining -= read;
        }
    }
    CloseHandle(file);
    if (succeeded) ++writer->files;
    return succeeded;
}

static bool AddDirectory(BundleWriter* writer, LaiueContentType type,
    const wchar_t* absolute, const wchar_t* relative, uint32_t depth)
{
    if (depth >= BUNDLE_MAX_DEPTH) return false;
    uint32_t absoluteLength = WideLength(absolute);
    wchar_t* search = HeapAlloc(GetProcessHeap(), 0,
        ((size_t)absoluteLength + 3U) * sizeof(wchar_t));
    if (search == NULL) return false;
    memcpy(search, absolute, (size_t)absoluteLength * sizeof(wchar_t));
    search[absoluteLength] = L'\\';
    search[absoluteLength + 1U] = L'*';
    search[absoluteLength + 2U] = L'\0';

    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(search, &data);
    HeapFree(GetProcessHeap(), 0, search);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool succeeded = true;
    do
    {
        if (data.cFileName[0] == L'.'
            && (data.cFileName[1] == L'\0'
                || (data.cFileName[1] == L'.' && data.cFileName[2] == L'\0'))) continue;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
            || !LaiueContentNameIsSafe(data.cFileName))
        {
            succeeded = false;
            break;
        }
        uint32_t childLength = WideLength(data.cFileName);
        uint32_t relativeLength = WideLength(relative);
        wchar_t* childAbsolute = HeapAlloc(GetProcessHeap(), 0,
            ((size_t)absoluteLength + childLength + 2U) * sizeof(wchar_t));
        wchar_t* childRelative = HeapAlloc(GetProcessHeap(), 0,
            ((size_t)relativeLength + childLength + 2U) * sizeof(wchar_t));
        if (childAbsolute == NULL || childRelative == NULL)
        {
            if (childAbsolute != NULL) HeapFree(GetProcessHeap(), 0, childAbsolute);
            if (childRelative != NULL) HeapFree(GetProcessHeap(), 0, childRelative);
            succeeded = false;
            break;
        }
        memcpy(childAbsolute, absolute, (size_t)absoluteLength * sizeof(wchar_t));
        childAbsolute[absoluteLength] = L'\\';
        memcpy(childAbsolute + absoluteLength + 1U, data.cFileName,
            ((size_t)childLength + 1U) * sizeof(wchar_t));
        memcpy(childRelative, relative, (size_t)relativeLength * sizeof(wchar_t));
        childRelative[relativeLength] = L'\\';
        memcpy(childRelative + relativeLength + 1U, data.cFileName,
            ((size_t)childLength + 1U) * sizeof(wchar_t));
        succeeded = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            ? AddDirectory(writer, type, childAbsolute, childRelative, depth + 1U)
            : AddFile(writer, type, childRelative, childAbsolute);
        HeapFree(GetProcessHeap(), 0, childRelative);
        HeapFree(GetProcessHeap(), 0, childAbsolute);
        if (!succeeded) break;
    }
    while (FindNextFileW(find, &data));
    FindClose(find);
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
        || (sourceCount != 0 && sources == NULL)) return false;
    memset(output, 0, sizeof(*output));
    BundleWriter writer;
    memset(&writer, 0, sizeof(writer));
    writer.bytes = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        BUNDLE_INITIAL_CAPACITY);
    if (writer.bytes == NULL) return false;
    writer.capacity = BUNDLE_INITIAL_CAPACITY;
    writer.size = BUNDLE_HEADER_SIZE;

    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    bool succeeded = path != NULL;
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
        DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            succeeded = false;
            break;
        }
        succeeded = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            ? AddDirectory(&writer, sources[i].type, path, sources[i].name, 0)
            : AddFile(&writer, sources[i].type, sources[i].name, path);
    }
    if (path != NULL) HeapFree(GetProcessHeap(), 0, path);
    if (succeeded)
    {
        WriteU32(writer.bytes, BUNDLE_MAGIC);
        WriteU16(writer.bytes + 4, BUNDLE_VERSION);
        WriteU16(writer.bytes + 6, (uint16_t)writer.files);
        WriteU64(writer.bytes + 8, writer.size);
        succeeded = ComputeSha256(writer.bytes, writer.size, output->sha256);
    }
    if (!succeeded)
    {
        HeapFree(GetProcessHeap(), 0, writer.bytes);
        return false;
    }
    output->bytes = writer.bytes;
    output->size = writer.size;
    return true;
}

void LaiueContentBundleRelease(LaiueContentBundle* bundle)
{
    if (bundle == NULL) return;
    if (bundle->bytes != NULL) HeapFree(GetProcessHeap(), 0, bundle->bytes);
    memset(bundle, 0, sizeof(*bundle));
}

static bool DecodeSafePath(const uint8_t* bytes, uint16_t length,
    wchar_t* output, uint32_t capacity, wchar_t* top, uint32_t topCapacity)
{
    if (length == 0 || length >= BUNDLE_MAX_PATH_UTF8) return false;
    int32_t written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        (const char*)bytes, length, output, (int32_t)capacity - 1);
    if (written <= 0 || (uint32_t)written >= capacity) return false;
    output[written] = L'\0';
    uint32_t segmentStart = 0;
    uint32_t topLength = 0;
    for (uint32_t i = 0; i <= (uint32_t)written; ++i)
    {
        if (output[i] != L'\\' && output[i] != L'\0') continue;
        wchar_t saved = output[i];
        output[i] = L'\0';
        if (!LaiueContentNameIsSafe(output + segmentStart)) return false;
        if (segmentStart == 0)
        {
            topLength = i;
            if (topLength + 1U > topCapacity) return false;
            memcpy(top, output, ((size_t)topLength + 1U) * sizeof(wchar_t));
        }
        output[i] = saved;
        segmentStart = i + 1U;
    }
    return topLength != 0;
}

static bool RootEquals(const BundleRoot* root, LaiueContentType type,
    const wchar_t* name)
{
    if (root->type != type) return false;
    uint32_t i = 0;
    while (root->name[i] != L'\0' && root->name[i] == name[i]) ++i;
    return root->name[i] == name[i];
}

static bool ValidateBundle(const uint8_t* bytes, uint64_t size,
    BundleRoot* roots, uint32_t* rootCount)
{
    if (bytes == NULL || size < BUNDLE_HEADER_SIZE
        || size > LAIUE_CONTENT_BUNDLE_MAX_BYTES
        || ReadU32(bytes) != BUNDLE_MAGIC || ReadU16(bytes + 4) != BUNDLE_VERSION
        || ReadU64(bytes + 8) != size) return false;
    uint32_t fileCount = ReadU16(bytes + 6);
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
            || !LaiueContentNameMatches(type, top)) return false;
        bool directory = path[WideLength(top)] == L'\\';
        uint32_t rootIndex = 0;
        while (rootIndex < *rootCount
            && !RootEquals(&roots[rootIndex], type, top)) ++rootIndex;
        if (rootIndex == *rootCount)
        {
            if (*rootCount >= LAIUE_CONTENT_BUNDLE_MAX_SOURCES) return false;
            roots[rootIndex].type = type;
            roots[rootIndex].directory = directory;
            memcpy(roots[rootIndex].name, top,
                ((size_t)WideLength(top) + 1U) * sizeof(wchar_t));
            ++*rootCount;
        }
        else if (roots[rootIndex].directory != directory) return false;
        offset += pathLength + fileSize;
    }
    return offset == size;
}

static bool BuildSiblingPath(LaiueContentType type, const wchar_t* name,
    const wchar_t* suffix, wchar_t* output, uint32_t capacity)
{
    if (!LaiueContentBuildPath(type, NULL, NULL, output, capacity)) return false;
    uint32_t length = WideLength(output);
    return AppendWide(output, capacity, &length, L"\\")
        && AppendWide(output, capacity, &length, name)
        && AppendWide(output, capacity, &length, suffix);
}

static bool DeleteTree(const wchar_t* path)
{
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            ? RemoveDirectoryW(path) != 0 : DeleteFileW(path) != 0;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path) != 0;
    }
    uint32_t length = WideLength(path);
    wchar_t* search = HeapAlloc(GetProcessHeap(), 0,
        ((size_t)length + 3U) * sizeof(wchar_t));
    if (search == NULL) return false;
    memcpy(search, path, (size_t)length * sizeof(wchar_t));
    search[length] = L'\\'; search[length + 1U] = L'*'; search[length + 2U] = L'\0';
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(search, &data);
    bool succeeded = true;
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (data.cFileName[0] == L'.' && (data.cFileName[1] == L'\0'
                || (data.cFileName[1] == L'.' && data.cFileName[2] == L'\0'))) continue;
            uint32_t childLength = WideLength(data.cFileName);
            wchar_t* child = HeapAlloc(GetProcessHeap(), 0,
                ((size_t)length + childLength + 2U) * sizeof(wchar_t));
            if (child == NULL) { succeeded = false; break; }
            memcpy(child, path, (size_t)length * sizeof(wchar_t));
            child[length] = L'\\';
            memcpy(child + length + 1U, data.cFileName,
                ((size_t)childLength + 1U) * sizeof(wchar_t));
            succeeded = DeleteTree(child);
            HeapFree(GetProcessHeap(), 0, child);
            if (!succeeded) break;
        }
        while (FindNextFileW(find, &data));
        FindClose(find);
    }
    HeapFree(GetProcessHeap(), 0, search);
    return succeeded && RemoveDirectoryW(path) != 0;
}

static bool EnsureParentDirectories(wchar_t* path)
{
    uint32_t length = WideLength(path);
    for (uint32_t i = 3U; i < length; ++i)
    {
        if (path[i] != L'\\') continue;
        path[i] = L'\0';
        bool okay = CreateDirectoryW(path, NULL)
            || GetLastError() == ERROR_ALREADY_EXISTS;
        path[i] = L'\\';
        if (!okay) return false;
    }
    return true;
}

static bool ExtractBundle(const uint8_t* bytes, uint64_t size,
    const BundleRoot* roots, uint32_t rootCount)
{
    wchar_t* path = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    wchar_t decoded[384];
    wchar_t top[128];
    bool succeeded = path != NULL;
    for (uint32_t i = 0; i < rootCount && succeeded; ++i)
    {
        succeeded = BuildSiblingPath(roots[i].type, roots[i].name,
            L".download", path, LAIUE_CONTENT_PATH_CAPACITY)
            && DeleteTree(path);
    }
    uint32_t fileCount = ReadU16(bytes + 6);
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
        if (succeeded && decoded[topLength] == L'\\')
        {
            uint32_t length = WideLength(path);
            succeeded = AppendWide(path, LAIUE_CONTENT_PATH_CAPACITY,
                &length, decoded + topLength);
        }
        if (succeeded) succeeded = EnsureParentDirectories(path);
        HANDLE file = succeeded ? CreateFileW(path, GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        if (file == INVALID_HANDLE_VALUE) succeeded = false;
        uint64_t writtenTotal = 0;
        while (succeeded && writtenTotal < fileSize)
        {
            DWORD part = fileSize - writtenTotal > 1024U * 1024U
                ? 1024U * 1024U : (DWORD)(fileSize - writtenTotal);
            DWORD written = 0;
            succeeded = WriteFile(file, bytes + offset + writtenTotal,
                part, &written, NULL) && written == part;
            writtenTotal += written;
        }
        if (file != INVALID_HANDLE_VALUE)
        {
            if (succeeded) succeeded = FlushFileBuffers(file) != 0;
            CloseHandle(file);
        }
        offset += fileSize;
    }
    HeapFree(GetProcessHeap(), 0, path);
    return succeeded && offset == size;
}

static bool CommitRoots(const BundleRoot* roots, uint32_t rootCount)
{
    wchar_t* final = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    wchar_t* staging = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    wchar_t* previous = HeapAlloc(GetProcessHeap(), 0,
        (size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t));
    bool succeeded = final != NULL && staging != NULL && previous != NULL;
    for (uint32_t i = 0; i < rootCount && succeeded; ++i)
    {
        succeeded = LaiueContentBuildPath(roots[i].type, roots[i].name,
                NULL, final, LAIUE_CONTENT_PATH_CAPACITY)
            && BuildSiblingPath(roots[i].type, roots[i].name, L".download",
                staging, LAIUE_CONTENT_PATH_CAPACITY)
            && BuildSiblingPath(roots[i].type, roots[i].name, L".previous",
                previous, LAIUE_CONTENT_PATH_CAPACITY)
            && DeleteTree(previous);
        DWORD existing = succeeded ? GetFileAttributesW(final) : INVALID_FILE_ATTRIBUTES;
        bool hadExisting = existing != INVALID_FILE_ATTRIBUTES;
        if (succeeded && hadExisting)
        {
            succeeded = MoveFileExW(final, previous,
                MOVEFILE_WRITE_THROUGH) != 0;
        }
        if (succeeded)
        {
            succeeded = MoveFileExW(staging, final,
                MOVEFILE_WRITE_THROUGH) != 0;
            if (!succeeded && hadExisting)
            {
                MoveFileExW(previous, final, MOVEFILE_WRITE_THROUGH);
            }
        }
    }
    if (previous != NULL) HeapFree(GetProcessHeap(), 0, previous);
    if (staging != NULL) HeapFree(GetProcessHeap(), 0, staging);
    if (final != NULL) HeapFree(GetProcessHeap(), 0, final);
    return succeeded;
}

bool LaiueContentBundleInstall(const uint8_t* bytes, uint64_t size)
{
    BundleRoot* roots = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        LAIUE_CONTENT_BUNDLE_MAX_SOURCES * sizeof(BundleRoot));
    if (roots == NULL) return false;
    uint32_t rootCount = 0;
    bool succeeded = ValidateBundle(bytes, size, roots, &rootCount)
        && ExtractBundle(bytes, size, roots, rootCount)
        && CommitRoots(roots, rootCount);
    HeapFree(GetProcessHeap(), 0, roots);
    return succeeded;
}
