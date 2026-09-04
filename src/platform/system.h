#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define LAIUE_PLATFORM_PATH_CAPACITY 1024U
#define LAIUE_PLATFORM_DIRECTORY_STORAGE_WORDS 160U
/* The storage covers the largest known native primitive: pthread_rwlock_t is
 * 200 bytes on macOS against 56 on glibc and musl and 8 for SRWLOCK, so the
 * headroom follows Darwin. */
#define LAIUE_PLATFORM_RWLOCK_STORAGE_WORDS 32U
/* SRWLOCK и CONDITION_VARIABLE занимают по указателю, pthread-примитивы —
 * от 40 до 64 байт в зависимости от libc. Запас берётся с той же логикой,
 * что и у rwlock, а точный размер сверяется _Static_assert в реализации. */
#define LAIUE_PLATFORM_MUTEX_STORAGE_WORDS 16U
#define LAIUE_PLATFORM_CONDITION_STORAGE_WORDS 16U

typedef struct PlatformRwLock
{
    uintptr_t storage[LAIUE_PLATFORM_RWLOCK_STORAGE_WORDS];
} PlatformRwLock;

/* Мьютекс отделён от rwlock намеренно: условная переменная POSIX умеет
 * ждать только на обычном мьютексе, а разделяемый захват нужен движку
 * ровно в одном диагностическом месте. */
typedef struct PlatformMutex
{
    uintptr_t storage[LAIUE_PLATFORM_MUTEX_STORAGE_WORDS];
} PlatformMutex;

typedef struct PlatformConditionVariable
{
    uintptr_t storage[LAIUE_PLATFORM_CONDITION_STORAGE_WORDS];
} PlatformConditionVariable;

typedef struct PlatformThread
{
    uintptr_t storage[2];
} PlatformThread;

/* Возвращаемое значение доходит только до PlatformThreadJoin; сообщать об
 * ошибке рабочему потоку следует через собственное состояние. */
typedef uint32_t (*PlatformThreadEntry)(void *context);

typedef struct PlatformDirectoryIterator
{
    uintptr_t storage[LAIUE_PLATFORM_DIRECTORY_STORAGE_WORDS];
} PlatformDirectoryIterator;

typedef struct PlatformDirectoryEntry
{
    wchar_t name[LAIUE_PLATFORM_PATH_CAPACITY];
    uint64_t size;
    bool isDirectory;
    bool isSymbolicLink;
} PlatformDirectoryEntry;

typedef struct PlatformPathInformation
{
    uint64_t size;
    // Время последней записи в наносекундах от эпохи платформы. Годится
    // для сравнения двух файлов между собой — «этот новее того», — но
    // не для показа пользователю: часы и точность у файловых систем
    // разные. Ноль означает, что время получить не удалось.
    uint64_t modifiedTime;
    bool exists;
    bool isDirectory;
    bool isSymbolicLink;
} PlatformPathInformation;

typedef void* PlatformDynamicLibrary;

void* PlatformAllocate(size_t size, bool zeroInitialize);
void* PlatformReallocate(void* memory, size_t size, bool zeroNewMemory);
void PlatformFree(void* memory);

bool PlatformRwLockInitialize(PlatformRwLock* lock);
void PlatformRwLockDestroy(PlatformRwLock* lock);
void PlatformRwLockAcquireShared(PlatformRwLock* lock);
void PlatformRwLockReleaseShared(PlatformRwLock* lock);
void PlatformRwLockAcquireExclusive(PlatformRwLock* lock);
void PlatformRwLockReleaseExclusive(PlatformRwLock* lock);

bool PlatformMutexInitialize(PlatformMutex *mutex);
void PlatformMutexDestroy(PlatformMutex *mutex);
void PlatformMutexLock(PlatformMutex *mutex);
void PlatformMutexUnlock(PlatformMutex *mutex);

bool PlatformConditionVariableInitialize(PlatformConditionVariable *condition);
void PlatformConditionVariableDestroy(PlatformConditionVariable *condition);
/* Вызывается с захваченным mutex; на время ожидания он отпускается. */
void PlatformConditionVariableWait(PlatformConditionVariable *condition, PlatformMutex *mutex);
void PlatformConditionVariableWakeOne(PlatformConditionVariable *condition);
void PlatformConditionVariableWakeAll(PlatformConditionVariable *condition);

/* Поток запускается сразу. PlatformThreadJoin обязателен: он и дожидается
 * завершения, и освобождает описатель. */
bool PlatformThreadStart(PlatformThread *thread, PlatformThreadEntry entry, void *context);
void PlatformThreadJoin(PlatformThread *thread);

/* Число логических процессоров, доступных процессу; не меньше единицы. */
uint32_t PlatformLogicalProcessorCount(void);

uint32_t PlatformAtomicLoadU32Acquire(const volatile uint32_t *value);
uint32_t PlatformAtomicIncrementU32(volatile uint32_t *value);
int64_t PlatformAtomicLoadI64(const volatile int64_t *value);
int64_t PlatformAtomicAddI64(volatile int64_t *value, int64_t addend);
int64_t PlatformAtomicIncrementI64(volatile int64_t *value);
bool PlatformAtomicCompareExchangeU32(volatile uint32_t *value, uint32_t *expected,
                                      uint32_t desired);
void PlatformAtomicStoreU32Release(volatile uint32_t *value, uint32_t desired);

double PlatformMonotonicSeconds(void);
uint64_t PlatformMonotonicMilliseconds(void);
void PlatformSleepMilliseconds(uint32_t milliseconds);

bool PlatformInstallTerminationHandler(void);
void PlatformRemoveTerminationHandler(void);
bool PlatformTerminationRequested(void);
void PlatformWriteConsoleUtf8(const char* message);

uint32_t PlatformGetEnvironmentUtf8(
    const char* name, char* output, uint32_t capacity);
bool PlatformExecutableDirectory(wchar_t* output, uint32_t capacity);

bool PlatformWideToUtf8(const wchar_t* input, char* output,
                        uint32_t capacity, uint32_t* outLength);
bool PlatformUtf8ToWide(const char* input, uint32_t length, wchar_t* output,
                        uint32_t capacity, uint32_t* outLength);

bool PlatformCreateDirectory(const wchar_t* path);
bool PlatformPathExists(const wchar_t* path);
bool PlatformGetPathInformation(
    const wchar_t* path, PlatformPathInformation* information);
bool PlatformDeleteFile(const wchar_t* path);
bool PlatformRemoveDirectory(const wchar_t* path);
bool PlatformMoveReplace(const wchar_t* source, const wchar_t* destination);
bool PlatformValidatePrivateKeyFile(const wchar_t* path);
bool PlatformReadEntireFile(const wchar_t* path, uint64_t maximumBytes,
                            uint8_t** outBytes, uint64_t* outSize);
bool PlatformWriteEntireFile(const wchar_t* path, const void* bytes,
                             uint64_t size);
bool PlatformWriteFileAtomic(const wchar_t* path, const void* bytes,
                             uint64_t size);
bool PlatformAppendFile(const wchar_t* path, const void* bytes, uint64_t size);

bool PlatformDirectoryOpen(
    PlatformDirectoryIterator* iterator, const wchar_t* path);
bool PlatformDirectoryNext(
    PlatformDirectoryIterator* iterator, PlatformDirectoryEntry* entry);
void PlatformDirectoryClose(PlatformDirectoryIterator* iterator);

bool PlatformSha256(const void* bytes, uint64_t size, uint8_t output[32]);
bool PlatformRandomBytes(void* output, uint32_t size);
bool PlatformConstantTimeEqual(
    const void* first, const void* second, size_t size);

PlatformDynamicLibrary PlatformDynamicLibraryOpen(const wchar_t* path);
void* PlatformDynamicLibrarySymbol(
    PlatformDynamicLibrary library, const char* name);
void PlatformDynamicLibraryClose(PlatformDynamicLibrary library);
