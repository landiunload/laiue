#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct ServerConfiguration
{
    uint16_t port;
    uint16_t maximumPeers;
    int64_t worldSeed;
    bool allowContentDownloads;
} ServerConfiguration;

void ServerConfigurationLoad(ServerConfiguration* configuration);
