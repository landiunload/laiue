#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define FILE_BYTES 8193U

typedef struct FileTestState
{
    wchar_t root[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t file[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t empty[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t missing[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t link[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t special[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t large[LAIUE_PLATFORM_PATH_CAPACITY];
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    uint8_t pattern[FILE_BYTES];
    uint8_t buffer[FILE_BYTES + 3U];
} FileTestState;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Platform file check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void Join(wchar_t *output, const wchar_t *directory, const wchar_t *name)
{
    uint32_t length = 0U;
    for (; directory[length] != L'\0'; ++length)
    {
        Expect(length + 1U < LAIUE_PLATFORM_PATH_CAPACITY, "directory path overflow");
        output[length] = directory[length];
    }
    Expect(length + 1U < LAIUE_PLATFORM_PATH_CAPACITY, "separator path overflow");
    output[length++] = L'/';
    for (uint32_t index = 0U; name[index] != L'\0'; ++index)
    {
        Expect(length + 1U < LAIUE_PLATFORM_PATH_CAPACITY, "file path overflow");
        output[length++] = name[index];
    }
    output[length] = L'\0';
}

static void ExpectRejected(const wchar_t *path, uint64_t limit, void *buffer, uint32_t capacity,
                           const char *message)
{
    uint32_t bytes = UINT32_MAX;
    uint64_t size = UINT64_MAX;
    Expect(!PlatformReadFilePrefix(path, limit, buffer, capacity, &bytes, &size), message);
    Expect(bytes == 0U && size == 0U, "failure must clear both outputs");
}

static void TestPrefixes(FileTestState *state)
{
    static const uint32_t capacities[] = {0U,    1U,    2U,    15U,   16U,   17U,
                                          4095U, 4096U, 4097U, 8192U, 8193U, 8194U};
    for (uint32_t index = 0U; index < FILE_BYTES; ++index)
        state->pattern[index] = (uint8_t)(index * 137U + 19U);
    Expect(PlatformWriteEntireFile(state->file, state->pattern, FILE_BYTES), "write data");
    Expect(PlatformWriteEntireFile(state->empty, NULL, 0U), "write empty file");

    for (uint32_t test = 0U; test < sizeof(capacities) / sizeof(capacities[0]); ++test)
    {
        uint32_t capacity = capacities[test];
        uint32_t wanted = capacity < FILE_BYTES ? capacity : FILE_BYTES;
        for (uint32_t index = 0U; index < sizeof(state->buffer); ++index)
            state->buffer[index] = 0xc7U;
        uint32_t bytes = UINT32_MAX;
        uint64_t size = UINT64_MAX;
        Expect(PlatformReadFilePrefix(state->file, FILE_BYTES, state->buffer + 1U, capacity, &bytes,
                                      &size),
               "prefix read");
        Expect(bytes == wanted && size == FILE_BYTES, "prefix lengths");
        Expect(state->buffer[0] == 0xc7U, "prefix leading guard");
        for (uint32_t index = 0U; index < wanted; ++index)
            Expect(state->buffer[index + 1U] == state->pattern[index], "prefix byte mismatch");
        for (uint32_t index = wanted + 1U; index < sizeof(state->buffer); ++index)
            Expect(state->buffer[index] == 0xc7U, "read exceeded requested prefix or EOF");

        Expect(
            PlatformReadFilePrefix(state->empty, 0U, state->buffer + 1U, capacity, &bytes, &size) &&
                bytes == 0U && size == 0U,
            "empty file with zero size limit");
    }

    uint32_t bytes = UINT32_MAX;
    uint64_t size = UINT64_MAX;
    Expect(PlatformReadFilePrefix(state->file, UINT64_MAX, NULL, 0U, &bytes, &size) &&
               bytes == 0U && size == FILE_BYTES,
           "size-only query");
    Expect(PlatformReadFilePrefix(state->empty, 0U, NULL, 0U, &bytes, &size) && bytes == 0U &&
               size == 0U,
           "empty size-only query");
    ExpectRejected(state->file, FILE_BYTES - 1U, state->buffer, 1U, "oversized file accepted");
    ExpectRejected(state->file, FILE_BYTES - 1U, NULL, 0U, "size-only query ignored limit");
    ExpectRejected(state->file, 0U, NULL, 0U, "nonempty file accepted at zero limit");
    ExpectRejected(state->file, UINT64_MAX, NULL, 1U, "NULL buffer accepted");
    ExpectRejected(NULL, UINT64_MAX, state->buffer, 1U, "NULL path accepted");
    ExpectRejected(L"", UINT64_MAX, state->buffer, 1U, "empty path accepted");
    ExpectRejected(state->missing, UINT64_MAX, state->buffer, 1U, "missing file accepted");
    ExpectRejected(state->root, UINT64_MAX, NULL, 0U, "directory accepted");

    size = UINT64_MAX;
    Expect(!PlatformReadFilePrefix(state->file, FILE_BYTES, state->buffer, 1U, NULL, &size) &&
               size == 0U,
           "NULL byte-count output must fail and clear size");
    bytes = UINT32_MAX;
    Expect(!PlatformReadFilePrefix(state->file, FILE_BYTES, state->buffer, 1U, &bytes, NULL) &&
               bytes == 0U,
           "NULL size output must fail and clear byte count");
    Expect(!PlatformReadFilePrefix(state->file, FILE_BYTES, state->buffer, 1U, NULL, NULL),
           "missing outputs accepted");
}

static void TestSpecialFiles(FileTestState *state)
{
#if defined(_WIN32)
    BOOLEAN linked =
        CreateSymbolicLinkW(state->link, state->file, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (!linked && GetLastError() == ERROR_INVALID_PARAMETER)
        linked = CreateSymbolicLinkW(state->link, state->file, 0U);
    if (linked)
    {
        ExpectRejected(state->link, UINT64_MAX, NULL, 0U, "symbolic link accepted");
        Expect(PlatformDeleteFile(state->link), "delete symbolic link");
    }
    else
    {
        Expect(GetLastError() == ERROR_PRIVILEGE_NOT_HELD, "create symbolic link");
        LaiueTestRuntimeWrite("file symlink check skipped: Windows privilege unavailable\n");
    }
    ExpectRejected(L"NUL", UINT64_MAX, NULL, 0U, "character device accepted");
#else
    Expect(PlatformWideToUtf8(state->link, state->nativePath, sizeof(state->nativePath), NULL),
           "symbolic link native path");
    Expect(symlink("data.bin", state->nativePath) == 0, "create symbolic link");
    ExpectRejected(state->link, UINT64_MAX, NULL, 0U, "symbolic link accepted");
    Expect(PlatformDeleteFile(state->link), "delete symbolic link");
    Expect(PlatformWideToUtf8(state->special, state->nativePath, sizeof(state->nativePath), NULL),
           "FIFO native path");
    Expect(mkfifo(state->nativePath, 0600) == 0, "create FIFO");
    /* No writer exists: omitting O_NONBLOCK in the reader would hang here. */
    ExpectRejected(state->special, UINT64_MAX, NULL, 0U, "FIFO accepted");
    Expect(PlatformDeleteFile(state->special), "delete FIFO");
    ExpectRejected(L"/dev/null", UINT64_MAX, NULL, 0U, "character device accepted");
#endif
}

static void TestLargeFile(FileTestState *state)
{
    const uint64_t largeSize = (uint64_t)UINT32_MAX + 37U;
#if defined(_WIN32)
    HANDLE file =
        CreateFileW(state->large, GENERIC_WRITE, 0U, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    Expect(file != INVALID_HANDLE_VALUE, "create sparse file");
    DWORD returned = 0U;
    if (!DeviceIoControl(file, FSCTL_SET_SPARSE, NULL, 0U, NULL, 0U, &returned, NULL))
    {
        Expect(CloseHandle(file) != FALSE && PlatformDeleteFile(state->large),
               "remove unsupported sparse file");
        LaiueTestRuntimeWrite("large file check skipped: sparse files unavailable\n");
        return;
    }
    LARGE_INTEGER end;
    end.QuadPart = (LONGLONG)largeSize;
    Expect(SetFilePointerEx(file, end, NULL, FILE_BEGIN) != FALSE && SetEndOfFile(file) != FALSE,
           "size sparse file");
    Expect(CloseHandle(file) != FALSE, "close sparse file");
#else
    Expect(PlatformWideToUtf8(state->large, state->nativePath, sizeof(state->nativePath), NULL),
           "sparse native path");
    int file = open(state->nativePath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    Expect(file >= 0, "create sparse file");
    Expect(ftruncate(file, (off_t)largeSize) == 0, "size sparse file");
    Expect(close(file) == 0, "close sparse file");
#endif
    uint32_t bytes = UINT32_MAX;
    uint64_t size = UINT64_MAX;
    Expect(PlatformReadFilePrefix(state->large, largeSize, state->buffer, 17U, &bytes, &size),
           "large-file bounded prefix");
    Expect(bytes == 17U && size == largeSize, "large-file size truncated to 32 bits");
    for (uint32_t index = 0U; index < bytes; ++index)
        Expect(state->buffer[index] == 0U, "sparse prefix must contain zeros");
    ExpectRejected(state->large, largeSize - 1U, NULL, 0U, "large-file limit truncated");
    Expect(PlatformDeleteFile(state->large), "delete sparse file");
}

static bool GetFixtureRoot(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY])
{
    static char configuredRoot[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    uint32_t length = PlatformGetEnvironmentUtf8(
        "LAIUE_PLATFORM_FILE_TEST_ROOT", configuredRoot, sizeof(configuredRoot));
    if (length != 0U)
    {
        return PlatformUtf8ToWide(configuredRoot, length, output,
                                  LAIUE_PLATFORM_PATH_CAPACITY, NULL);
    }
    return PlatformExecutableDirectory(output, LAIUE_PLATFORM_PATH_CAPACITY);
}

LAIUE_TEST_ENTRY(PlatformFileTestEntryPoint)
{
    FileTestState *state = PlatformAllocate(sizeof(*state), false);
    Expect(state != NULL, "scratch allocation");
    Expect(GetFixtureRoot(state->file), "fixture root");
    wchar_t name[] = L"platform_file_test_0000000000000000";
    static const wchar_t digits[] = L"0123456789abcdef";
    uint8_t random[8];
    Expect(PlatformRandomBytes(random, sizeof(random)), "unique fixture name");
    for (uint32_t index = 0U; index < sizeof(random); ++index)
    {
        name[19U + index * 2U] = digits[random[index] >> 4U];
        name[20U + index * 2U] = digits[random[index] & 15U];
    }
    Join(state->root, state->file, name);
    Join(state->file, state->root, L"data.bin");
    Join(state->empty, state->root, L"empty.bin");
    Join(state->missing, state->root, L"missing.bin");
    Join(state->link, state->root, L"link.bin");
    Join(state->special, state->root, L"special");
    Join(state->large, state->root, L"large.bin");
    Expect(PlatformCreateDirectory(state->root), "fixture directory");
    TestPrefixes(state);
    TestSpecialFiles(state);
    TestLargeFile(state);
    Expect(PlatformDeleteFile(state->file) && PlatformDeleteFile(state->empty), "delete fixtures");
    Expect(PlatformRemoveDirectory(state->root), "delete fixture directory");
    PlatformFree(state);
    LaiueTestRuntimeWrite("platform_file_test passed\n");
    LAIUE_TEST_SUCCESS();
}
