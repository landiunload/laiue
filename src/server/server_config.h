#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SERVER_CONFIG_ADDRESS_CAPACITY 256U
#define SERVER_CONFIG_PATH_CAPACITY 1024U
#define SERVER_CONFIG_THUMBPRINT_CAPACITY 65U

typedef enum ServerAddressFamily
{
    SERVER_ADDRESS_FAMILY_DUAL,
    SERVER_ADDRESS_FAMILY_IPV4,
    SERVER_ADDRESS_FAMILY_IPV6,
} ServerAddressFamily;

typedef struct ServerConfiguration
{
    uint16_t port;
    uint16_t maximumPeers;
    int64_t worldSeed;
    bool allowContentDownloads;
    ServerAddressFamily addressFamily;
    char listenAddress[SERVER_CONFIG_ADDRESS_CAPACITY];
    char certificateFile[SERVER_CONFIG_PATH_CAPACITY];
    char privateKeyFile[SERVER_CONFIG_PATH_CAPACITY];
    char certificateThumbprint[SERVER_CONFIG_THUMBPRINT_CAPACITY];
} ServerConfiguration;

void ServerConfigurationLoad(ServerConfiguration* configuration);
