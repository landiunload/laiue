#include "platform/system.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <string.h>

typedef struct WindowsDirectoryIterator
{
    HANDLE handle;
    WIN32_FIND_DATAW current;
    bool hasCurrent;
} WindowsDirectoryIterator;

_Static_assert(sizeof(SRWLOCK) <= sizeof(PlatformRwLock),
    "PlatformRwLock storage is too small");
_Static_assert(sizeof(uint32_t) == sizeof(LONG), "platform atomics require 32-bit LONG");
_Static_assert(sizeof(WindowsDirectoryIterator) <= sizeof(PlatformDirectoryIterator),
    "PlatformDirectoryIterator storage is too small");

static volatile LONG g_terminationRequested;

static BOOL WINAPI PlatformConsoleHandler(DWORD controlType)
{
    if (controlType == CTRL_C_EVENT || controlType == CTRL_BREAK_EVENT
        || controlType == CTRL_CLOSE_EVENT || controlType == CTRL_SHUTDOWN_EVENT)
    {
        InterlockedExchange(&g_terminationRequested, 1);
        return TRUE;
    }
    return FALSE;
}

void* PlatformAllocate(size_t size, bool zeroInitialize)
{
    if (size == 0) size = 1;
    return HeapAlloc(GetProcessHeap(),
        zeroInitialize ? HEAP_ZERO_MEMORY : 0, size);
}

void* PlatformReallocate(void* memory, size_t size, bool zeroNewMemory)
{
    if (memory == NULL) return PlatformAllocate(size, zeroNewMemory);
    if (size == 0) size = 1;
    return HeapReAlloc(GetProcessHeap(),
        zeroNewMemory ? HEAP_ZERO_MEMORY : 0, memory, size);
}

void PlatformFree(void* memory)
{
    if (memory != NULL) HeapFree(GetProcessHeap(), 0, memory);
}

bool PlatformRwLockInitialize(PlatformRwLock* lock)
{
    if (lock == NULL) return false;
    memset(lock, 0, sizeof(*lock));
    InitializeSRWLock((PSRWLOCK)lock);
    return true;
}

void PlatformRwLockDestroy(PlatformRwLock* lock)
{
    (void)lock;
}

void PlatformRwLockAcquireShared(PlatformRwLock* lock)
{
    AcquireSRWLockShared((PSRWLOCK)lock);
}

void PlatformRwLockReleaseShared(PlatformRwLock* lock)
{
    ReleaseSRWLockShared((PSRWLOCK)lock);
}

void PlatformRwLockAcquireExclusive(PlatformRwLock* lock)
{
    AcquireSRWLockExclusive((PSRWLOCK)lock);
}

void PlatformRwLockReleaseExclusive(PlatformRwLock* lock)
{
    ReleaseSRWLockExclusive((PSRWLOCK)lock);
}

uint32_t PlatformAtomicLoadU32Acquire(const volatile uint32_t *value)
{
    return (uint32_t)InterlockedCompareExchange((volatile LONG *)value, 0, 0);
}

bool PlatformAtomicCompareExchangeU32(volatile uint32_t *value, uint32_t *expected,
                                      uint32_t desired)
{
    LONG previous =
        InterlockedCompareExchange((volatile LONG *)value, (LONG)desired, (LONG)*expected);
    if ((uint32_t)previous == *expected)
    {
        return true;
    }
    *expected = (uint32_t)previous;
    return false;
}

void PlatformAtomicStoreU32Release(volatile uint32_t *value, uint32_t desired)
{
    InterlockedExchange((volatile LONG *)value, (LONG)desired);
}

double PlatformMonotonicSeconds(void)
{
    static LONGLONG frequency;
    if (frequency == 0)
    {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        InterlockedCompareExchange64(&frequency, value.QuadPart, 0);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency;
}

uint64_t PlatformMonotonicMilliseconds(void)
{
    return GetTickCount64();
}

void PlatformSleepMilliseconds(uint32_t milliseconds)
{
    Sleep(milliseconds);
}

bool PlatformInstallTerminationHandler(void)
{
    InterlockedExchange(&g_terminationRequested, 0);
    return SetConsoleCtrlHandler(PlatformConsoleHandler, TRUE) != FALSE;
}

void PlatformRemoveTerminationHandler(void)
{
    SetConsoleCtrlHandler(PlatformConsoleHandler, FALSE);
}

bool PlatformTerminationRequested(void)
{
    return InterlockedCompareExchange(&g_terminationRequested, 0, 0) != 0;
}

void PlatformWriteConsoleUtf8(const char* message)
{
    if (message == NULL) return;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == NULL || output == INVALID_HANDLE_VALUE) return;
    DWORD length = 0;
    while (message[length] != '\0') ++length;
    DWORD written = 0;
    WriteFile(output, message, length, &written, NULL);
}

uint32_t PlatformGetEnvironmentUtf8(
    const char* name, char* output, uint32_t capacity)
{
    if (name == NULL || output == NULL || capacity == 0) return 0;
    DWORD length = GetEnvironmentVariableA(name, output, capacity);
    return length > 0 && length < capacity ? (uint32_t)length : 0;
}

bool PlatformExecutableDirectory(wchar_t* output, uint32_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    DWORD length = GetModuleFileNameW(NULL, output, capacity);
    if (length == 0 || length >= capacity) return false;
    while (length > 0 && output[length - 1U] != L'\\'
        && output[length - 1U] != L'/')
        --length;
    if (length == 0) return false;
    output[length - 1U] = L'\0';
    return true;
}

bool PlatformWideToUtf8(const wchar_t* input, char* output,
                        uint32_t capacity, uint32_t* outLength)
{
    if (input == NULL || output == NULL || capacity == 0) return false;
    int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input, -1, output, (int)capacity, NULL, NULL);
    if (written <= 0) return false;
    if (outLength != NULL) *outLength = (uint32_t)written - 1U;
    return true;
}

bool PlatformUtf8ToWide(const char* input, uint32_t length, wchar_t* output,
                        uint32_t capacity, uint32_t* outLength)
{
    if (input == NULL || output == NULL || capacity == 0
        || length > INT32_MAX) return false;
    int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input, (int)length, output, (int)capacity - 1);
    if (written <= 0 && length != 0) return false;
    output[written] = L'\0';
    if (outLength != NULL) *outLength = (uint32_t)written;
    return true;
}

bool PlatformCreateDirectory(const wchar_t* path)
{
    if (CreateDirectoryW(path, NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool PlatformPathExists(const wchar_t* path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool PlatformGetPathInformation(
    const wchar_t* path, PlatformPathInformation* information)
{
    if (path == NULL || information == NULL) return false;
    memset(information, 0, sizeof(*information));
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
        return true;
    information->exists = true;
    information->isDirectory =
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    information->isSymbolicLink =
        (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    information->size = ((uint64_t)data.nFileSizeHigh << 32U)
        | data.nFileSizeLow;
    return true;
}

bool PlatformDeleteFile(const wchar_t* path)
{
    return DeleteFileW(path) != FALSE;
}

bool PlatformRemoveDirectory(const wchar_t* path)
{
    return RemoveDirectoryW(path) != FALSE;
}

bool PlatformMoveReplace(const wchar_t* source, const wchar_t* destination)
{
    return MoveFileExW(source, destination,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool PlatformValidatePrivateKeyFile(const wchar_t* path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & (FILE_ATTRIBUTE_DIRECTORY
            | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool PlatformReadEntireFile(const wchar_t* path, uint64_t maximumBytes,
                            uint8_t** outBytes, uint64_t* outSize)
{
    if (outBytes == NULL || outSize == NULL) return false;
    *outBytes = NULL;
    *outSize = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION information;
    LARGE_INTEGER size = {0};
    bool valid = GetFileInformationByHandle(file, &information)
        && !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && GetFileSizeEx(file, &size) && size.QuadPart >= 0
        && (uint64_t)size.QuadPart <= maximumBytes;
    uint8_t* bytes = valid
        ? PlatformAllocate((size_t)(size.QuadPart == 0 ? 1 : size.QuadPart), false)
        : NULL;
    uint64_t completed = 0;
    while (valid && completed < (uint64_t)size.QuadPart)
    {
        DWORD part = (uint64_t)size.QuadPart - completed > 0x7ffff000ULL
            ? 0x7ffff000U : (DWORD)((uint64_t)size.QuadPart - completed);
        DWORD read = 0;
        if (!ReadFile(file, bytes + completed, part, &read, NULL) || read == 0)
            valid = false;
        else
            completed += read;
    }
    CloseHandle(file);
    if (!valid)
    {
        PlatformFree(bytes);
        return false;
    }
    *outBytes = bytes;
    *outSize = (uint64_t)size.QuadPart;
    return true;
}

bool PlatformWriteEntireFile(const wchar_t* path, const void* bytes,
                             uint64_t size)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    const uint8_t* source = bytes;
    uint64_t completed = 0;
    bool succeeded = true;
    while (completed < size)
    {
        DWORD part = size - completed > 0x7ffff000ULL
            ? 0x7ffff000U : (DWORD)(size - completed);
        DWORD written = 0;
        if (!WriteFile(file, source + completed, part, &written, NULL)
            || written == 0)
        {
            succeeded = false;
            break;
        }
        completed += written;
    }
    succeeded = succeeded && FlushFileBuffers(file);
    CloseHandle(file);
    return succeeded;
}

static bool BuildTemporaryPath(
    const wchar_t* path, wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY])
{
    uint32_t length = 0;
    while (path[length] != L'\0'
        && length + 5U < LAIUE_PLATFORM_PATH_CAPACITY)
    {
        output[length] = path[length];
        ++length;
    }
    if (path[length] != L'\0') return false;
    output[length++] = L'.';
    output[length++] = L't';
    output[length++] = L'm';
    output[length++] = L'p';
    output[length] = L'\0';
    return true;
}

bool PlatformWriteFileAtomic(const wchar_t* path, const void* bytes,
                             uint64_t size)
{
    wchar_t temporary[LAIUE_PLATFORM_PATH_CAPACITY];
    if (!BuildTemporaryPath(path, temporary)) return false;
    if (!PlatformWriteEntireFile(temporary, bytes, size))
    {
        PlatformDeleteFile(temporary);
        return false;
    }
    if (!PlatformMoveReplace(temporary, path))
    {
        PlatformDeleteFile(temporary);
        return false;
    }
    return true;
}

bool PlatformAppendFile(const wchar_t* path, const void* bytes, uint64_t size)
{
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool succeeded = size <= UINT32_MAX
        && WriteFile(file, bytes, (DWORD)size, &written, NULL)
        && written == (DWORD)size;
    CloseHandle(file);
    return succeeded;
}

bool PlatformDirectoryOpen(
    PlatformDirectoryIterator* iterator, const wchar_t* path)
{
    if (iterator == NULL || path == NULL) return false;
    memset(iterator, 0, sizeof(*iterator));
    wchar_t search[LAIUE_PLATFORM_PATH_CAPACITY];
    uint32_t length = 0;
    while (path[length] != L'\0'
        && length + 3U < LAIUE_PLATFORM_PATH_CAPACITY)
    {
        search[length] = path[length];
        ++length;
    }
    if (path[length] != L'\0') return false;
    if (length > 0 && search[length - 1U] != L'\\'
        && search[length - 1U] != L'/')
        search[length++] = L'\\';
    search[length++] = L'*';
    search[length] = L'\0';

    WindowsDirectoryIterator* state = (WindowsDirectoryIterator*)iterator;
    state->handle = FindFirstFileW(search, &state->current);
    state->hasCurrent = state->handle != INVALID_HANDLE_VALUE;
    return state->hasCurrent;
}

bool PlatformDirectoryNext(
    PlatformDirectoryIterator* iterator, PlatformDirectoryEntry* entry)
{
    if (iterator == NULL || entry == NULL) return false;
    WindowsDirectoryIterator* state = (WindowsDirectoryIterator*)iterator;
    while (state->hasCurrent)
    {
        WIN32_FIND_DATAW data = state->current;
        state->hasCurrent = FindNextFileW(state->handle, &state->current) != FALSE;
        if ((data.cFileName[0] == L'.' && data.cFileName[1] == L'\0')
            || (data.cFileName[0] == L'.' && data.cFileName[1] == L'.'
                && data.cFileName[2] == L'\0'))
            continue;
        memset(entry, 0, sizeof(*entry));
        uint32_t length = 0;
        while (data.cFileName[length] != L'\0'
            && length + 1U < LAIUE_PLATFORM_PATH_CAPACITY)
        {
            entry->name[length] = data.cFileName[length];
            ++length;
        }
        if (data.cFileName[length] != L'\0') continue;
        entry->name[length] = L'\0';
        entry->size = ((uint64_t)data.nFileSizeHigh << 32U)
            | data.nFileSizeLow;
        entry->isDirectory =
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry->isSymbolicLink =
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        return true;
    }
    return false;
}

void PlatformDirectoryClose(PlatformDirectoryIterator* iterator)
{
    if (iterator == NULL) return;
    WindowsDirectoryIterator* state = (WindowsDirectoryIterator*)iterator;
    if (state->handle != NULL && state->handle != INVALID_HANDLE_VALUE)
        FindClose(state->handle);
    memset(iterator, 0, sizeof(*iterator));
}

bool PlatformSha256(const void* bytes, uint64_t size, uint8_t output[32])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    bool succeeded = BCryptOpenAlgorithmProvider(&algorithm,
        BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0
        && BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0;
    const uint8_t* input = bytes;
    uint64_t offset = 0;
    while (succeeded && offset < size)
    {
        ULONG part = size - offset > 0xffffffffULL
            ? 0xffffffffU : (ULONG)(size - offset);
        succeeded = BCryptHashData(hash, (PUCHAR)(input + offset), part, 0) >= 0;
        offset += part;
    }
    if (succeeded)
        succeeded = BCryptFinishHash(hash, output, 32U, 0) >= 0;
    if (hash != NULL) BCryptDestroyHash(hash);
    if (algorithm != NULL) BCryptCloseAlgorithmProvider(algorithm, 0);
    return succeeded;
}

bool PlatformRandomBytes(void* output, uint32_t size)
{
    return BCryptGenRandom(NULL, output, size,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool PlatformConstantTimeEqual(
    const void* first, const void* second, size_t size)
{
    const uint8_t* a = first;
    const uint8_t* b = second;
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}

PlatformDynamicLibrary PlatformDynamicLibraryOpen(const wchar_t* path)
{
    return (PlatformDynamicLibrary)LoadLibraryExW(
        path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
            | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

void* PlatformDynamicLibrarySymbol(
    PlatformDynamicLibrary library, const char* name)
{
    return (void*)GetProcAddress((HMODULE)library, name);
}

void PlatformDynamicLibraryClose(PlatformDynamicLibrary library)
{
    if (library != NULL) FreeLibrary((HMODULE)library);
}
