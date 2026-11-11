#pragma once

/* Таблица сервиса laiue.net для загрузки модуля по имени. Функции совпадают
 * с одноимёнными из net.h. */

#include "api.h"
#include "mod/module_api.h"
#include "net/net.h"

#include <stdint.h>

#define LAIUE_NET_SERVICE_NAME "laiue.net"
#define LAIUE_NET_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueNetServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueNetStatus (*addressParse)(const char *text, LaiueNetAddress *outAddress);
    uint32_t (*addressFormat)(const LaiueNetAddress *address, char *output, uint32_t capacity);
    bool (*addressEqual)(const LaiueNetAddress *first, const LaiueNetAddress *second);
    LaiueNetStatus (*endpointCreate)(const LaiueNetEndpointDesc *desc,
                                     LaiueNetEndpoint **outEndpoint);
    void (*endpointDestroy)(LaiueNetEndpoint *endpoint);
    LaiueNetStatus (*connect)(LaiueNetEndpoint *endpoint, const LaiueNetConnectDesc *desc,
                              LaiueNetPeer *outPeer);
    LaiueNetStatus (*disconnect)(LaiueNetEndpoint *endpoint, LaiueNetPeer peer, uint32_t code,
                                 uint32_t flags);
    LaiueNetStatus (*send)(LaiueNetEndpoint *endpoint, LaiueNetPeer peer, uint32_t channel,
                           LaiueNetDelivery delivery, const void *bytes, uint32_t size);
    LaiueNetStatus (*update)(LaiueNetEndpoint *endpoint, uint64_t nowMicroseconds);
    bool (*pollEvent)(LaiueNetEndpoint *endpoint, LaiueNetEvent *outEvent);
    LaiueNetStatus (*peerGetInfo)(const LaiueNetEndpoint *endpoint, LaiueNetPeer peer,
                                  LaiueNetPeerInfo *outInfo);
    LaiueNetStatus (*peerSetUserData)(LaiueNetEndpoint *endpoint, LaiueNetPeer peer,
                                      void *userData);
    LaiueNetStatus (*endpointGetStats)(const LaiueNetEndpoint *endpoint,
                                       LaiueNetEndpointStats *outStats);
    LaiueNetStatus (*generateKeyPair)(uint8_t privateKey[LAIUE_NET_KEY_BYTES],
                                      uint8_t publicKey[LAIUE_NET_KEY_BYTES]);
    LaiueNetStatus (*publicKey)(const uint8_t privateKey[LAIUE_NET_KEY_BYTES],
                                uint8_t publicKey[LAIUE_NET_KEY_BYTES]);
    uint64_t (*clockMicroseconds)(void);
    LaiueNetStatus (*udpOpen)(const LaiueNetUdpDesc *desc, LaiueNetUdp **outUdp);
    void (*udpClose)(LaiueNetUdp *udp);
    LaiueNetStatus (*udpGetDriver)(LaiueNetUdp *udp, LaiueNetDriverV1 *outDriver);
    LaiueNetStatus (*udpGetLocalAddress)(const LaiueNetUdp *udp, LaiueNetAddress *outAddress);
    LaiueNetStatus (*resolve)(const char *host, uint16_t port, LaiueNetAddress *outAddresses,
                              uint32_t capacity, uint32_t *outCount);
    LaiueNetStatus (*simulatorCreate)(const LaiueNetSimulatorDesc *desc,
                                      LaiueNetSimulator **outSimulator);
    void (*simulatorDestroy)(LaiueNetSimulator *simulator);
    LaiueNetStatus (*simulatorOpen)(LaiueNetSimulator *simulator, const LaiueNetAddress *address,
                                    LaiueNetDriverV1 *outDriver);
    LaiueNetStatus (*simulatorSetConditions)(LaiueNetSimulator *simulator,
                                             const LaiueNetSimulatorConditions *conditions);
    LaiueNetStatus (*simulatorSetFilter)(LaiueNetSimulator *simulator,
                                         LaiueNetSimulatorFilter filter, void *context);
    LaiueNetStatus (*simulatorAdvance)(LaiueNetSimulator *simulator, uint64_t nowMicroseconds);
    uintptr_t reserved[8];
} LaiueNetServiceV1;

LAIUE_NET_API const LaiueNetServiceV1 *LaiueNetGetStaticServiceV1(void);
LAIUE_NET_API const LaiueModuleApiV1 *LaiueNetGetStaticModuleApiV1(void);
