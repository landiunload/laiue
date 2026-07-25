#include "network/network.h"

#include <stddef.h>

#if defined(LAIUE_HAS_MSQUIC)
NetworkSecureTransportStatus NetworkMsQuicGetStatus(void);
NetworkClient *NetworkMsQuicClientCreate(
    const NetworkClientConfiguration *configuration);
NetworkServer *NetworkMsQuicServerCreate(
    const NetworkServerConfiguration *configuration);
#endif

NetworkSecureTransportStatus NetworkGetSecureTransportStatus(void)
{
#if defined(LAIUE_HAS_MSQUIC)
    return NetworkMsQuicGetStatus();
#else
    return NETWORK_SECURE_TRANSPORT_UNAVAILABLE;
#endif
}

NetworkClient *NetworkClientCreate(
    const NetworkClientConfiguration *configuration)
{
    if (!NetworkClientConfigurationIsValid(configuration))
    {
        return NULL;
    }
#if defined(LAIUE_HAS_MSQUIC)
    return NetworkMsQuicClientCreate(configuration);
#else
    // Deliberately no fallback to TCP. A missing QUIC/TLS dependency is a
    // startup error, never permission to expose plaintext remotely.
    return NULL;
#endif
}

NetworkServer *NetworkServerCreate(
    const NetworkServerConfiguration *configuration)
{
    if (!NetworkServerConfigurationIsValid(configuration))
    {
        return NULL;
    }
#if defined(LAIUE_HAS_MSQUIC)
    return NetworkMsQuicServerCreate(configuration);
#else
    // See NetworkClientCreate: production networking is fail-closed.
    return NULL;
#endif
}
