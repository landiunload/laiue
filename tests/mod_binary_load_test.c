#include "platform/system.h"
#include "test_runtime.h"

#include <stdint.h>

#if defined(_WIN32)
#define LAIUE_TEST_MOD_SUFFIX L".windows-x86_64.dll"
#elif defined(LAIUE_LINUX_LIBC_MUSL)
#define LAIUE_TEST_MOD_SUFFIX L".linux-x86_64-musl.so"
#else
#define LAIUE_TEST_MOD_SUFFIX L".linux-x86_64-gnu.so"
#endif

static const wchar_t* const modNames[] = {
    L"auto_bridge",
    L"builder_lib",
    L"daylight_lock",
    L"double_jump",
    L"spiral_tower",
};

static bool AppendText(
    wchar_t* destination, uint32_t capacity,
    uint32_t* length, const wchar_t* source)
{
    uint32_t sourceLength = 0;
    while (source[sourceLength] != L'\0')
    {
        ++sourceLength;
    }
    if (*length + sourceLength >= capacity)
    {
        return false;
    }
    for (uint32_t index = 0; index < sourceLength; ++index)
    {
        destination[*length + index] = source[index];
    }
    *length += sourceLength;
    destination[*length] = L'\0';
    return true;
}

static bool BuildModPath(
    const wchar_t* executableDirectory,
    const wchar_t* modName,
    wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY])
{
    uint32_t length = 0;
    path[0] = L'\0';
    return AppendText(
               path, LAIUE_PLATFORM_PATH_CAPACITY,
               &length, executableDirectory) &&
           AppendText(
               path, LAIUE_PLATFORM_PATH_CAPACITY,
               &length, L"/mods/") &&
           AppendText(
               path, LAIUE_PLATFORM_PATH_CAPACITY,
               &length, modName) &&
           AppendText(
               path, LAIUE_PLATFORM_PATH_CAPACITY,
               &length, L".lmp/") &&
           AppendText(
               path, LAIUE_PLATFORM_PATH_CAPACITY,
               &length, modName) &&
           AppendText(
               path, LAIUE_PLATFORM_PATH_CAPACITY,
               &length, LAIUE_TEST_MOD_SUFFIX);
}

LAIUE_TEST_ENTRY(ModBinaryLoadTestEntryPoint)
{
    wchar_t *executableDirectory = PlatformAllocate(
        (size_t)LAIUE_PLATFORM_PATH_CAPACITY *
            sizeof(*executableDirectory),
        false);
    wchar_t *path = PlatformAllocate(
        (size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(*path),
        false);
    if (executableDirectory == NULL || path == NULL)
    {
        PlatformFree(path);
        PlatformFree(executableDirectory);
        LaiueTestRuntimeWrite(
            "mod binary load: allocation failed\r\n");
        LaiueTestRuntimeExit(1);
    }
    if (!PlatformExecutableDirectory(
            executableDirectory, LAIUE_PLATFORM_PATH_CAPACITY))
    {
        PlatformFree(path);
        PlatformFree(executableDirectory);
        LaiueTestRuntimeWrite(
            "mod binary load: executable directory unavailable\r\n");
        LaiueTestRuntimeExit(1);
    }

    for (uint32_t index = 0;
         index < sizeof(modNames) / sizeof(modNames[0]); ++index)
    {
        if (!BuildModPath(
                executableDirectory, modNames[index], path))
        {
            PlatformFree(path);
            PlatformFree(executableDirectory);
            LaiueTestRuntimeWrite(
                "mod binary load: path overflow\r\n");
            LaiueTestRuntimeExit(1);
        }
        PlatformDynamicLibrary library =
            PlatformDynamicLibraryOpen(path);
        if (library == NULL ||
            PlatformDynamicLibrarySymbol(
                library, "LaiueModInit") == NULL)
        {
            PlatformDynamicLibraryClose(library);
            PlatformFree(path);
            PlatformFree(executableDirectory);
            LaiueTestRuntimeWrite(
                "mod binary load: missing library/export\r\n");
            LaiueTestRuntimeExit(1);
        }
        PlatformDynamicLibraryClose(library);
    }

    PlatformFree(path);
    PlatformFree(executableDirectory);
    LaiueTestRuntimeWrite(
        "Native example mod binaries: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
