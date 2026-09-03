#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
/* Darwin exposes O_NOFOLLOW only at the full BSD level. Under a bare
 * _POSIX_C_SOURCE the macro disappears, and the symlink swap guard would
 * silently disappear with it. */
#define _DARWIN_C_SOURCE 1
#endif

#include "platform/system.h"
#include "platform/sha256.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <Security/Security.h>
#include <mach-o/dyld.h>
#else
#include <sys/random.h>
#endif

typedef struct PosixDirectoryIterator
{
    DIR *directory;
    char *path;
} PosixDirectoryIterator;

_Static_assert(sizeof(pthread_rwlock_t) <= sizeof(PlatformRwLock),
               "PlatformRwLock storage is too small");
_Static_assert(sizeof(PosixDirectoryIterator) <= sizeof(PlatformDirectoryIterator),
               "PlatformDirectoryIterator storage is too small");

static volatile sig_atomic_t g_terminationRequested;

static void PlatformSignalHandler(int signalNumber)
{
    (void)signalNumber;
    g_terminationRequested = 1;
}

void *PlatformAllocate(size_t size, bool zeroInitialize)
{
    if (size == 0)
        size = 1;
    return zeroInitialize ? calloc(1, size) : malloc(size);
}

void *PlatformReallocate(void *memory, size_t size, bool zeroNewMemory)
{
    (void)zeroNewMemory;
    if (size == 0)
        size = 1;
    return realloc(memory, size);
}

void PlatformFree(void *memory)
{
    free(memory);
}

bool PlatformRwLockInitialize(PlatformRwLock *lock)
{
    if (lock == NULL)
        return false;
    memset(lock, 0, sizeof(*lock));
    return pthread_rwlock_init((pthread_rwlock_t *)lock, NULL) == 0;
}

void PlatformRwLockDestroy(PlatformRwLock *lock)
{
    pthread_rwlock_destroy((pthread_rwlock_t *)lock);
}

void PlatformRwLockAcquireShared(PlatformRwLock *lock)
{
    while (pthread_rwlock_rdlock((pthread_rwlock_t *)lock) == EINTR)
    {
    }
}

void PlatformRwLockReleaseShared(PlatformRwLock *lock)
{
    pthread_rwlock_unlock((pthread_rwlock_t *)lock);
}

void PlatformRwLockAcquireExclusive(PlatformRwLock *lock)
{
    while (pthread_rwlock_wrlock((pthread_rwlock_t *)lock) == EINTR)
    {
    }
}

void PlatformRwLockReleaseExclusive(PlatformRwLock *lock)
{
    pthread_rwlock_unlock((pthread_rwlock_t *)lock);
}

uint32_t PlatformAtomicLoadU32Acquire(const volatile uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

bool PlatformAtomicCompareExchangeU32(volatile uint32_t *value, uint32_t *expected,
                                      uint32_t desired)
{
    return __atomic_compare_exchange_n(value, expected, desired, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

void PlatformAtomicStoreU32Release(volatile uint32_t *value, uint32_t desired)
{
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
}

double PlatformMonotonicSeconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0.0;
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

uint64_t PlatformMonotonicMilliseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000ULL + (uint64_t)value.tv_nsec / 1000000ULL;
}

void PlatformSleepMilliseconds(uint32_t milliseconds)
{
    struct timespec request = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    while (nanosleep(&request, &request) != 0 && errno == EINTR)
    {
    }
}

bool PlatformInstallTerminationHandler(void)
{
    g_terminationRequested = 0;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = PlatformSignalHandler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0)
        return false;
    signal(SIGPIPE, SIG_IGN);
    return true;
}

void PlatformRemoveTerminationHandler(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

bool PlatformTerminationRequested(void)
{
    return g_terminationRequested != 0;
}

void PlatformWriteConsoleUtf8(const char *message)
{
    if (message == NULL)
        return;
    size_t length = strlen(message);
    while (length > 0)
    {
        ssize_t written = write(STDOUT_FILENO, message, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        message += written;
        length -= (size_t)written;
    }
}

uint32_t PlatformGetEnvironmentUtf8(const char *name, char *output, uint32_t capacity)
{
    if (name == NULL || output == NULL || capacity == 0)
        return 0;
    const char *value = getenv(name);
    if (value == NULL)
        return 0;
    size_t length = strlen(value);
    if (length == 0 || length >= capacity)
        return 0;
    memcpy(output, value, length + 1U);
    return (uint32_t)length;
}

bool PlatformExecutableDirectory(wchar_t *output, uint32_t capacity)
{
    if (output == NULL || capacity == 0)
        return false;
#if defined(LAIUE_MOBILE_PLATFORM)
    // A mobile process executable is not an application resource or writable
    // data root. The application shell must inject an OS-provided container
    // directory into LaiueContentCatalogCreate instead of accepting a subtly
    // wrong /system/bin or read-only bundle path.
    output[0] = L'\0';
    return false;
#else
    char path[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
#if defined(__APPLE__)
    uint32_t pathCapacity = (uint32_t)sizeof(path);
    if (_NSGetExecutablePath(path, &pathCapacity) != 0)
        return false;
    size_t pathLength = strlen(path);
    if (pathLength == 0 || pathLength >= sizeof(path))
        return false;
    uint32_t length = (uint32_t)pathLength;
#else
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1U);
    if (length <= 0 || (size_t)length >= sizeof(path))
        return false;
    path[length] = '\0';
#endif
    while (length > 0 && path[length - 1] != '/')
        --length;
    if (length <= 0)
        return false;
    path[length - 1] = '\0';
    return PlatformUtf8ToWide(path, (uint32_t)(length - 1), output, capacity, NULL);
#endif
}

static bool EncodeUtf8Scalar(uint32_t scalar, char *output, uint32_t capacity, uint32_t *offset)
{
    uint32_t required = scalar <= 0x7fU ? 1U : scalar <= 0x7ffU ? 2U : scalar <= 0xffffU ? 3U : 4U;
    if (*offset + required >= capacity)
        return false;
    if (required == 1U)
        output[(*offset)++] = (char)scalar;
    else if (required == 2U)
    {
        output[(*offset)++] = (char)(0xc0U | (scalar >> 6U));
        output[(*offset)++] = (char)(0x80U | (scalar & 0x3fU));
    }
    else if (required == 3U)
    {
        output[(*offset)++] = (char)(0xe0U | (scalar >> 12U));
        output[(*offset)++] = (char)(0x80U | ((scalar >> 6U) & 0x3fU));
        output[(*offset)++] = (char)(0x80U | (scalar & 0x3fU));
    }
    else
    {
        output[(*offset)++] = (char)(0xf0U | (scalar >> 18U));
        output[(*offset)++] = (char)(0x80U | ((scalar >> 12U) & 0x3fU));
        output[(*offset)++] = (char)(0x80U | ((scalar >> 6U) & 0x3fU));
        output[(*offset)++] = (char)(0x80U | (scalar & 0x3fU));
    }
    return true;
}

bool PlatformWideToUtf8(const wchar_t *input, char *output, uint32_t capacity, uint32_t *outLength)
{
    if (input == NULL || output == NULL || capacity == 0)
        return false;
    uint32_t offset = 0;
    for (uint32_t i = 0; input[i] != L'\0'; ++i)
    {
        uint32_t scalar = (uint32_t)input[i];
        if (scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU) ||
            !EncodeUtf8Scalar(scalar, output, capacity, &offset))
            return false;
    }
    output[offset] = '\0';
    if (outLength != NULL)
        *outLength = offset;
    return true;
}

static bool DecodeUtf8Scalar(const uint8_t *input, uint32_t length, uint32_t *offset,
                             uint32_t *scalar)
{
    uint8_t first = input[(*offset)++];
    if (first < 0x80U)
    {
        *scalar = first;
        return true;
    }
    uint32_t count;
    uint32_t value;
    uint32_t minimum;
    if ((first & 0xe0U) == 0xc0U)
    {
        count = 1;
        value = first & 0x1fU;
        minimum = 0x80U;
    }
    else if ((first & 0xf0U) == 0xe0U)
    {
        count = 2;
        value = first & 0x0fU;
        minimum = 0x800U;
    }
    else if ((first & 0xf8U) == 0xf0U)
    {
        count = 3;
        value = first & 0x07U;
        minimum = 0x10000U;
    }
    else
        return false;
    if (*offset + count > length)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t next = input[(*offset)++];
        if ((next & 0xc0U) != 0x80U)
            return false;
        value = (value << 6U) | (next & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU))
        return false;
    *scalar = value;
    return true;
}

bool PlatformUtf8ToWide(const char *input, uint32_t length, wchar_t *output, uint32_t capacity,
                        uint32_t *outLength)
{
    if (input == NULL || output == NULL || capacity == 0)
        return false;
    uint32_t inputOffset = 0;
    uint32_t outputOffset = 0;
    while (inputOffset < length)
    {
        uint32_t scalar;
        if (!DecodeUtf8Scalar((const uint8_t *)input, length, &inputOffset, &scalar) ||
            outputOffset + 1U >= capacity)
            return false;
        output[outputOffset++] = (wchar_t)scalar;
    }
    output[outputOffset] = L'\0';
    if (outLength != NULL)
        *outLength = outputOffset;
    return true;
}

static bool WidePathToUtf8(const wchar_t *path, char output[LAIUE_PLATFORM_PATH_CAPACITY * 4U])
{
    if (!PlatformWideToUtf8(path, output, LAIUE_PLATFORM_PATH_CAPACITY * 4U, NULL))
        return false;
    for (uint32_t i = 0; output[i] != '\0'; ++i)
        if (output[i] == '\\')
            output[i] = '/';
    return true;
}

bool PlatformCreateDirectory(const wchar_t *path)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    if (!WidePathToUtf8(path, nativePath))
        return false;
    if (mkdir(nativePath, 0755) == 0)
        return true;
    return errno == EEXIST;
}

bool PlatformPathExists(const wchar_t *path)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    struct stat status;
    return WidePathToUtf8(path, nativePath) && lstat(nativePath, &status) == 0;
}

bool PlatformGetPathInformation(const wchar_t *path, PlatformPathInformation *information)
{
    if (path == NULL || information == NULL)
        return false;
    memset(information, 0, sizeof(*information));
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    if (!WidePathToUtf8(path, nativePath))
        return false;
    struct stat status;
    if (lstat(nativePath, &status) != 0)
        return errno == ENOENT || errno == ENOTDIR;
    information->exists = true;
    information->isDirectory = S_ISDIR(status.st_mode);
    information->isSymbolicLink = S_ISLNK(status.st_mode);
    information->size = status.st_size < 0 ? 0 : (uint64_t)status.st_size;
    return true;
}

bool PlatformDeleteFile(const wchar_t *path)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    return WidePathToUtf8(path, nativePath) && unlink(nativePath) == 0;
}

bool PlatformRemoveDirectory(const wchar_t *path)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    return WidePathToUtf8(path, nativePath) && rmdir(nativePath) == 0;
}

bool PlatformMoveReplace(const wchar_t *source, const wchar_t *destination)
{
    char nativeSource[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    char nativeDestination[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    return WidePathToUtf8(source, nativeSource) && WidePathToUtf8(destination, nativeDestination) &&
           rename(nativeSource, nativeDestination) == 0;
}

bool PlatformValidatePrivateKeyFile(const wchar_t *path)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    struct stat status;
    return WidePathToUtf8(path, nativePath) && lstat(nativePath, &status) == 0 &&
           S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode) && (status.st_mode & 0077) == 0;
}

bool PlatformReadEntireFile(const wchar_t *path, uint64_t maximumBytes, uint8_t **outBytes,
                            uint64_t *outSize)
{
    if (outBytes == NULL || outSize == NULL)
        return false;
    *outBytes = NULL;
    *outSize = 0;
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    if (!WidePathToUtf8(path, nativePath))
        return false;
    int file = open(nativePath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0)
        return false;
    struct stat status;
    bool valid = fstat(file, &status) == 0 && S_ISREG(status.st_mode) && status.st_size >= 0 &&
                 (uint64_t)status.st_size <= maximumBytes;
    uint8_t *bytes =
        valid ? PlatformAllocate(status.st_size == 0 ? 1U : (size_t)status.st_size, false) : NULL;
    uint64_t completed = 0;
    while (valid && completed < (uint64_t)status.st_size)
    {
        ssize_t result =
            read(file, bytes + completed, (size_t)((uint64_t)status.st_size - completed));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            valid = false;
        else
            completed += (uint64_t)result;
    }
    close(file);
    if (!valid)
    {
        PlatformFree(bytes);
        return false;
    }
    *outBytes = bytes;
    *outSize = (uint64_t)status.st_size;
    return true;
}

static bool WriteAll(int file, const void *bytes, uint64_t size)
{
    const uint8_t *source = bytes;
    uint64_t completed = 0;
    while (completed < size)
    {
        ssize_t result = write(file, source + completed, (size_t)(size - completed));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        completed += (uint64_t)result;
    }
    return true;
}

bool PlatformWriteEntireFile(const wchar_t *path, const void *bytes, uint64_t size)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    if (!WidePathToUtf8(path, nativePath))
        return false;
    int file = open(nativePath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (file < 0)
        return false;
    bool succeeded = WriteAll(file, bytes, size) && fsync(file) == 0;
    close(file);
    return succeeded;
}

// rename() атомарен по видимости, но сама запись в каталог остаётся в кэше.
// Без fsync каталога потеря питания уносит файл, об успешной записи которого
// вызывающий код уже отчитался, — вместе с прежней версией, которую rename заменил.
// Windows-ветка получает ту же гарантию через MOVEFILE_WRITE_THROUGH.
static bool SyncDirectoryOfFile(const char *filePath)
{
    char directoryPath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    const char *lastSeparator = strrchr(filePath, '/');
    if (lastSeparator == NULL)
    {
        directoryPath[0] = '.';
        directoryPath[1] = '\0';
    }
    else
    {
        // Файл лежит прямо в корне: каталог — сам «/», а не пустая строка.
        size_t directoryLength = (size_t)(lastSeparator - filePath);
        if (directoryLength == 0)
            directoryLength = 1U;
        if (directoryLength >= sizeof(directoryPath))
            return false;
        memcpy(directoryPath, filePath, directoryLength);
        directoryPath[directoryLength] = '\0';
    }

    int directory = open(directoryPath, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directory < 0)
        return false;
    bool synced = fsync(directory) == 0;
    // EINVAL от fsync каталога означает, что файловая система такого не умеет.
    // Сделать больше нечего, и объявлять из-за этого состоявшуюся замену
    // неудачей — врать вызывающему в обратную сторону.
    if (!synced && errno == EINVAL)
        synced = true;
    close(directory);
    return synced;
}

bool PlatformWriteFileAtomic(const wchar_t *path, const void *bytes, uint64_t size)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    if (!WidePathToUtf8(path, nativePath))
        return false;
    char temporary[sizeof(nativePath) + 16U];
    int written = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", nativePath);
    if (written <= 0 || (size_t)written >= sizeof(temporary))
        return false;
    int file = mkstemp(temporary);
    if (file < 0)
        return false;
    fchmod(file, 0644);
    bool succeeded = WriteAll(file, bytes, size) && fsync(file) == 0;
    close(file);
    if (!succeeded || rename(temporary, nativePath) != 0)
    {
        unlink(temporary);
        return false;
    }
    return SyncDirectoryOfFile(nativePath);
}

bool PlatformAppendFile(const wchar_t *path, const void *bytes, uint64_t size)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    if (!WidePathToUtf8(path, nativePath))
        return false;
    int file = open(nativePath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (file < 0)
        return false;
    bool succeeded = WriteAll(file, bytes, size);
    close(file);
    return succeeded;
}

bool PlatformDirectoryOpen(PlatformDirectoryIterator *iterator, const wchar_t *path)
{
    if (iterator == NULL || path == NULL)
        return false;
    memset(iterator, 0, sizeof(*iterator));
    PosixDirectoryIterator *state = (PosixDirectoryIterator *)iterator;
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    if (!WidePathToUtf8(path, nativePath))
        return false;
    size_t length = strlen(nativePath);
    state->path = malloc(length + 1U);
    if (state->path == NULL)
        return false;
    memcpy(state->path, nativePath, length + 1U);
    state->directory = opendir(state->path);
    if (state->directory == NULL)
    {
        free(state->path);
        state->path = NULL;
        return false;
    }
    return true;
}

bool PlatformDirectoryNext(PlatformDirectoryIterator *iterator, PlatformDirectoryEntry *entry)
{
    if (iterator == NULL || entry == NULL)
        return false;
    PosixDirectoryIterator *state = (PosixDirectoryIterator *)iterator;
    for (;;)
    {
        errno = 0;
        struct dirent *result = readdir(state->directory);
        if (result == NULL)
            return false;
        if ((result->d_name[0] == '.' && result->d_name[1] == '\0') ||
            (result->d_name[0] == '.' && result->d_name[1] == '.' && result->d_name[2] == '\0'))
            continue;
        size_t baseLength = strlen(state->path);
        size_t nameLength = strlen(result->d_name);
        char child[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
        if (baseLength + nameLength + 2U > sizeof(child))
            continue;
        memcpy(child, state->path, baseLength);
        if (baseLength > 0 && child[baseLength - 1U] != '/')
            child[baseLength++] = '/';
        memcpy(child + baseLength, result->d_name, nameLength + 1U);
        struct stat status;
        if (lstat(child, &status) != 0)
            continue;
        memset(entry, 0, sizeof(*entry));
        if (!PlatformUtf8ToWide(result->d_name, (uint32_t)nameLength, entry->name,
                                LAIUE_PLATFORM_PATH_CAPACITY, NULL))
            continue;
        entry->size = status.st_size < 0 ? 0 : (uint64_t)status.st_size;
        entry->isDirectory = S_ISDIR(status.st_mode);
        entry->isSymbolicLink = S_ISLNK(status.st_mode);
        return true;
    }
}

void PlatformDirectoryClose(PlatformDirectoryIterator *iterator)
{
    if (iterator == NULL)
        return;
    PosixDirectoryIterator *state = (PosixDirectoryIterator *)iterator;
    if (state->directory != NULL)
        closedir(state->directory);
    free(state->path);
    memset(iterator, 0, sizeof(*iterator));
}

bool PlatformSha256(const void *bytes, uint64_t size, uint8_t output[32])
{
    return LaiueSha256Compute(bytes, size, output);
}

bool PlatformRandomBytes(void *output, uint32_t size)
{
#if defined(__APPLE__)
    return SecRandomCopyBytes(kSecRandomDefault, (size_t)size, output) == errSecSuccess;
#else
    uint8_t *bytes = output;
    uint32_t completed = 0;
    while (completed < size)
    {
        ssize_t result = getrandom(bytes + completed, size - completed, 0);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        completed += (uint32_t)result;
    }
    return true;
#endif
}

bool PlatformConstantTimeEqual(const void *first, const void *second, size_t size)
{
    const uint8_t *a = first;
    const uint8_t *b = second;
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i)
        difference |= a[i] ^ b[i];
    return difference == 0;
}

PlatformDynamicLibrary PlatformDynamicLibraryOpen(const wchar_t *path)
{
    char nativePath[LAIUE_PLATFORM_PATH_CAPACITY * 4U];
    if (!WidePathToUtf8(path, nativePath))
        return NULL;
    return dlopen(nativePath, RTLD_NOW | RTLD_LOCAL);
}

void *PlatformDynamicLibrarySymbol(PlatformDynamicLibrary library, const char *name)
{
    return library == NULL ? NULL : dlsym(library, name);
}

void PlatformDynamicLibraryClose(PlatformDynamicLibrary library)
{
    if (library != NULL)
        dlclose(library);
}
