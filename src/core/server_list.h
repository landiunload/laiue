#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#define SERVER_LIST_MAX_ENTRIES 8U
#define SERVER_LIST_TEXT_CAPACITY 64U

typedef struct ServerListEntry
{
    wchar_t name[SERVER_LIST_TEXT_CAPACITY];
    wchar_t address[SERVER_LIST_TEXT_CAPACITY];
    uint16_t port;
} ServerListEntry;

typedef struct ServerList
{
    ServerListEntry entries[SERVER_LIST_MAX_ENTRIES];
    uint32_t count;
} ServerList;

// До появления аутентифицированного TLS-транспорта разрешён только
// loopback. Список уже отделён от UI и позже сможет принять удалённый адрес.
bool ServerListLoad(ServerList* list);
