#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include "network/network.h"

#define SERVER_LIST_MAX_ENTRIES 8U
#define SERVER_LIST_TEXT_CAPACITY 128U

typedef struct ServerListEntry
{
    wchar_t name[SERVER_LIST_TEXT_CAPACITY];
    wchar_t address[SERVER_LIST_TEXT_CAPACITY];
    wchar_t endpointText[SERVER_LIST_TEXT_CAPACITY];
    uint16_t port;
    NetworkEndpoint endpoint;
    NetworkTrustMode trustMode;
    uint8_t certificateSha256[LAIUE_NETWORK_CERTIFICATE_PIN_SIZE];
} ServerListEntry;

typedef struct ServerList
{
    ServerListEntry entries[SERVER_LIST_MAX_ENTRIES];
    uint32_t count;
} ServerList;

// Формат v2: name|endpoint|system или name|endpoint|sha256:<64 hex>.
// Legacy name|address|port читается как system trust.
bool ServerListLoad(ServerList* list);
