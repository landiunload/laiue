#include "net/net_service.h"

static const LaiueNetServiceV1 service = {
    .structSize = sizeof(LaiueNetServiceV1),
    .abiVersion = LAIUE_NET_SERVICE_ABI_VERSION_1,
    .addressParse = LaiueNetAddressParse,
    .addressFormat = LaiueNetAddressFormat,
    .addressEqual = LaiueNetAddressEqual,
    .endpointCreate = LaiueNetEndpointCreate,
    .endpointDestroy = LaiueNetEndpointDestroy,
    .connect = LaiueNetConnect,
    .disconnect = LaiueNetDisconnect,
    .send = LaiueNetSend,
    .update = LaiueNetUpdate,
    .pollEvent = LaiueNetPollEvent,
    .peerGetInfo = LaiueNetPeerGetInfo,
    .peerSetUserData = LaiueNetPeerSetUserData,
    .endpointGetStats = LaiueNetEndpointGetStats,
    .generateKeyPair = LaiueNetGenerateKeyPair,
    .publicKey = LaiueNetPublicKey,
    .clockMicroseconds = LaiueNetClockMicroseconds,
    .udpOpen = LaiueNetUdpOpen,
    .udpClose = LaiueNetUdpClose,
    .udpGetDriver = LaiueNetUdpGetDriver,
    .udpGetLocalAddress = LaiueNetUdpGetLocalAddress,
    .resolve = LaiueNetResolve,
    .simulatorCreate = LaiueNetSimulatorCreate,
    .simulatorDestroy = LaiueNetSimulatorDestroy,
    .simulatorOpen = LaiueNetSimulatorOpen,
    .simulatorSetConditions = LaiueNetSimulatorSetConditions,
    .simulatorSetFilter = LaiueNetSimulatorSetFilter,
    .simulatorAdvance = LaiueNetSimulatorAdvance,
};

typedef struct NetModuleState
{
    const LaiueModuleHostV1 *host;
} NetModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL || host->free == NULL)
        return 0u;
    *outContext = NULL;
    NetModuleState *state = host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    NetModuleState *state = context;
    const LaiueModuleServiceV1 published = {
        .name = LAIUE_NET_SERVICE_NAME,
        .version = LAIUE_NET_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return state != NULL && state->host != NULL &&
                   state->host->publishService(state->host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    NetModuleState *state = context;
    if (state != NULL && state->host != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_NET_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    NetModuleState *state = context;
    if (state == NULL)
        return;
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    if (host != NULL)
        host->free(host->context, state);
}

static const char *const provides[] = {LAIUE_NET_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "laiue.net",
            .version = "1.0.0",
            .providesServices = provides,
            .providesCount = 1u,
        },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueNetServiceV1 *LaiueNetGetStaticServiceV1(void)
{
    return &service;
}

const LaiueModuleApiV1 *LaiueNetGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
