# The transport is portable; only the UDP driver needs sockets. An external
# platform adapter without them still gets the transport and brings its own
# datagram driver.
if(LAIUE_PLATFORM_EXTERNAL AND NOT LAIUE_PLATFORM_ADAPTER_HAS_SOCKETS)
    set(laiue_net_udp_source net_udp_none.c)
else()
    set(laiue_net_udp_source net_udp.c)
endif()

laiue_add_module(net
    RUNTIME_ROLE CORE
    SOURCES
        net.h
        net_service.h
        net_internal.h
        net_crypto.h
        net_crypto.c
        net_wire.c
        net_address.c
        net_channel.c
        net_congestion.c
        net_rate.c
        net_bbr.c
        net_pmtu.c
        net_packet.c
        net_handshake.c
        net_endpoint.c
        net_simulator.c
        ${laiue_net_udp_source}
        module_entry.c
        ../api.h
)
