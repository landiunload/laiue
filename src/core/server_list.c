#include "core/server_list.h"

#include <windows.h>
#include <string.h>

#define SERVER_LIST_FILE_MAX_BYTES 16384U

static bool BuildServersPath(wchar_t* output, uint32_t capacity)
{
    DWORD length = GetModuleFileNameW(NULL, output, capacity);
    if (length == 0 || length >= capacity) return false;
    while (length != 0 && output[length - 1U] != L'\\'
        && output[length - 1U] != L'/') --length;
    static const wchar_t name[] = L"servers.txt";
    if (length + (uint32_t)(sizeof(name) / sizeof(name[0])) > capacity)
    {
        return false;
    }
    memcpy(output + length, name, sizeof(name));
    return true;
}

static bool SliceEquals(const char* bytes, uint32_t length, const char* text)
{
    uint32_t i = 0;
    while (i < length && text[i] != '\0' && bytes[i] == text[i]) ++i;
    return i == length && text[i] == '\0';
}

static bool ParsePort(const char* bytes, uint32_t length, uint16_t* output)
{
    if (length == 0) return false;
    uint32_t value = 0;
    for (uint32_t i = 0; i < length; ++i)
    {
        if (bytes[i] < '0' || bytes[i] > '9') return false;
        value = value * 10U + (uint32_t)(bytes[i] - '0');
        if (value > 65535U) return false;
    }
    if (value == 0) return false;
    *output = (uint16_t)value;
    return true;
}

static bool CopyUtf8(const char* bytes, uint32_t length,
    wchar_t* output, uint32_t capacity)
{
    int32_t written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes, (int32_t)length, output, (int32_t)capacity - 1);
    if (written <= 0) return false;
    output[written] = L'\0';
    return true;
}

static bool ParseLine(const char* line, uint32_t length,
    ServerListEntry* entry)
{
    uint32_t first = 0;
    while (first < length && line[first] != '|') ++first;
    if (first == 0 || first == length) return false;
    uint32_t second = first + 1U;
    while (second < length && line[second] != '|') ++second;
    if (second == first + 1U || second == length) return false;
    uint32_t addressLength = second - first - 1U;
    const char* address = line + first + 1U;
    if (!SliceEquals(address, addressLength, "127.0.0.1")
        && !SliceEquals(address, addressLength, "localhost")) return false;

    memset(entry, 0, sizeof(*entry));
    return CopyUtf8(line, first, entry->name, SERVER_LIST_TEXT_CAPACITY)
        && CopyUtf8(address, addressLength, entry->address,
            SERVER_LIST_TEXT_CAPACITY)
        && ParsePort(line + second + 1U, length - second - 1U,
            &entry->port);
}

bool ServerListLoad(ServerList* list)
{
    if (list == NULL) return false;
    memset(list, 0, sizeof(*list));
    wchar_t path[MAX_PATH];
    HANDLE file = BuildServersPath(path, MAX_PATH)
        ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
        : INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size;
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0
            && size.QuadPart <= SERVER_LIST_FILE_MAX_BYTES)
        {
            uint32_t length = (uint32_t)size.QuadPart;
            char* bytes = HeapAlloc(GetProcessHeap(), 0, length);
            DWORD read = 0;
            if (bytes != NULL && ReadFile(file, bytes, length, &read, NULL)
                && read == length)
            {
                uint32_t offset = length >= 3U
                    && (uint8_t)bytes[0] == 0xefU
                    && (uint8_t)bytes[1] == 0xbbU
                    && (uint8_t)bytes[2] == 0xbfU ? 3U : 0U;
                while (offset < length && list->count < SERVER_LIST_MAX_ENTRIES)
                {
                    uint32_t end = offset;
                    while (end < length && bytes[end] != '\n') ++end;
                    uint32_t last = end;
                    while (last > offset && (bytes[last - 1U] == '\r'
                        || bytes[last - 1U] == ' ' || bytes[last - 1U] == '\t')) --last;
                    if (last > offset && bytes[offset] != '#'
                        && ParseLine(bytes + offset, last - offset,
                            &list->entries[list->count])) ++list->count;
                    offset = end + 1U;
                }
            }
            if (bytes != NULL) HeapFree(GetProcessHeap(), 0, bytes);
        }
        CloseHandle(file);
    }

    if (list->count == 0)
    {
        ServerListEntry* entry = &list->entries[0];
        memcpy(entry->name, L"Локальный сервер", sizeof(L"Локальный сервер"));
        memcpy(entry->address, L"127.0.0.1", sizeof(L"127.0.0.1"));
        entry->port = 27180U;
        list->count = 1;
    }
    return true;
}
