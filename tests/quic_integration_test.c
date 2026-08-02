#define _POSIX_C_SOURCE 200809L

#include "network/network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TEST_CASE_TIMEOUT_MS 10000U
#define DNS_CASE_TIMEOUT_MS 5000U
#define TEST_PUMP_SLEEP_NS 1000000L

typedef struct ReadyScenarioState
{
    bool serverVerified;
    bool modsSubmitted;
    bool serverConnected;
    bool snapshotBeginReceived;
    bool snapshotChunkReceived;
    bool constructResetReceived;
    bool constructBodyReceived;
    bool constructBlocksReceived;
    bool constructStateReceived;
    bool snapshotEndReceived;
    bool inventoryReceived;
    bool worldTimeReceived;
    bool playerJoinedReceived;
    bool playerStateReceived;
    bool blockDropReceived;
    bool readyReceived;
    bool acknowledgementSent;
    bool inputSent;
    bool inputReceived;
    bool leverEditSent;
    bool leverEditReceived;
    bool liveSnapshotSent;
    bool liveSnapshotBeginReceived;
    bool liveSnapshotChunkReceived;
    bool liveConstructResetReceived;
    bool liveConstructBodyReceived;
    bool liveConstructBlocksReceived;
    bool futureConstructStateReceived;
    bool liveSnapshotEndReceived;
    bool liveInputSent;
    bool liveInputReceived;
    uint32_t peerId;
} ReadyScenarioState;

typedef struct SecureCredentials
{
    char directory[64];
    char certificatePath[96];
    char privateKeyPath[96];
} SecureCredentials;

static uint32_t integrationChecks;

static bool Expect(bool condition, const char *message)
{
    ++integrationChecks;
    if (condition)
    {
        return true;
    }
    fprintf(stderr, "QUIC integration check failed: %s\n", message);
    return false;
}

static uint64_t MonotonicMilliseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000U +
           (uint64_t)value.tv_nsec / 1000000U;
}

static void PumpSleep(void)
{
    const struct timespec delay = {0, TEST_PUMP_SLEEP_NS};
    while (nanosleep(&delay, NULL) != 0 && errno == EINTR)
    {
    }
}

static bool ApproximatelyEqual(float left, float right)
{
    float difference = left - right;
    if (difference < 0.0f)
    {
        difference = -difference;
    }
    return difference <= 0.001f;
}

static bool PrivateKeyIsSecure(const char *path)
{
    struct stat information;
    return path != NULL && lstat(path, &information) == 0 &&
           S_ISREG(information.st_mode) && !S_ISLNK(information.st_mode) &&
           (information.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

static bool CopyRegularFileSecurely(
    const char *sourcePath, const char *destinationPath)
{
    int source = open(sourcePath, O_RDONLY | O_CLOEXEC);
    if (source < 0)
    {
        return false;
    }
    struct stat sourceInformation;
    if (fstat(source, &sourceInformation) != 0 ||
        !S_ISREG(sourceInformation.st_mode))
    {
        close(source);
        return false;
    }

    int destination = open(
        destinationPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (destination < 0)
    {
        close(source);
        return false;
    }

    bool succeeded = fchmod(destination, 0600) == 0;
    uint8_t buffer[4096];
    while (succeeded)
    {
        ssize_t readSize = read(source, buffer, sizeof(buffer));
        if (readSize == 0)
        {
            break;
        }
        if (readSize < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            succeeded = false;
            break;
        }
        ssize_t written = 0;
        while (written < readSize)
        {
            ssize_t writeSize = write(
                destination, buffer + written,
                (size_t)(readSize - written));
            if (writeSize < 0 && errno == EINTR)
            {
                continue;
            }
            if (writeSize <= 0)
            {
                succeeded = false;
                break;
            }
            written += writeSize;
        }
    }
    if (close(destination) != 0)
    {
        succeeded = false;
    }
    close(source);
    if (!succeeded)
    {
        unlink(destinationPath);
    }
    return succeeded;
}

static void DestroySecureCredentials(SecureCredentials *credentials)
{
    if (credentials == NULL)
    {
        return;
    }
    if (credentials->privateKeyPath[0] != '\0')
    {
        unlink(credentials->privateKeyPath);
    }
    if (credentials->certificatePath[0] != '\0')
    {
        unlink(credentials->certificatePath);
    }
    if (credentials->directory[0] != '\0')
    {
        rmdir(credentials->directory);
    }
    memset(credentials, 0, sizeof(*credentials));
}

static bool PrepareSecureCredentials(
    const char *certificateSource, const char *privateKeySource,
    SecureCredentials *credentials)
{
    if (certificateSource == NULL || privateKeySource == NULL ||
        credentials == NULL)
    {
        return false;
    }
    memset(credentials, 0, sizeof(*credentials));
    memcpy(credentials->directory,
           "/tmp/laiue-quic-integration-XXXXXX",
           sizeof("/tmp/laiue-quic-integration-XXXXXX"));
    if (mkdtemp(credentials->directory) == NULL)
    {
        memset(credentials, 0, sizeof(*credentials));
        return false;
    }
    int certificateLength = snprintf(
        credentials->certificatePath,
        sizeof(credentials->certificatePath), "%s/server.pem",
        credentials->directory);
    int keyLength = snprintf(
        credentials->privateKeyPath,
        sizeof(credentials->privateKeyPath), "%s/server-key.pem",
        credentials->directory);
    if (certificateLength <= 0 ||
        (size_t)certificateLength >=
            sizeof(credentials->certificatePath) ||
        keyLength <= 0 ||
        (size_t)keyLength >= sizeof(credentials->privateKeyPath) ||
        !CopyRegularFileSecurely(
            certificateSource, credentials->certificatePath) ||
        !CopyRegularFileSecurely(
            privateKeySource, credentials->privateKeyPath) ||
        !PrivateKeyIsSecure(credentials->privateKeyPath))
    {
        DestroySecureCredentials(credentials);
        return false;
    }
    return true;
}

static bool FindFreeDualStackPort(uint16_t *outPort)
{
    if (outPort == NULL)
    {
        return false;
    }
    int socketHandle = socket(AF_INET6, SOCK_DGRAM, 0);
    if (socketHandle < 0)
    {
        return false;
    }

    int ipv6Only = 0;
    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    bool succeeded =
        setsockopt(socketHandle, IPPROTO_IPV6, IPV6_V6ONLY,
                   &ipv6Only, sizeof(ipv6Only)) == 0 &&
        bind(socketHandle, (const struct sockaddr *)&address,
             sizeof(address)) == 0;
    socklen_t addressSize = sizeof(address);
    if (succeeded)
    {
        succeeded =
            getsockname(socketHandle, (struct sockaddr *)&address,
                        &addressSize) == 0 &&
            address.sin6_port != 0;
    }
    if (succeeded)
    {
        *outPort = ntohs(address.sin6_port);
    }
    close(socketHandle);
    return succeeded;
}

static bool Ipv6LoopbackAvailable(void)
{
    int socketHandle = socket(AF_INET6, SOCK_DGRAM, 0);
    if (socketHandle < 0)
    {
        return false;
    }
    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    bool available =
        bind(socketHandle, (const struct sockaddr *)&address,
             sizeof(address)) == 0;
    close(socketHandle);
    return available;
}

static NetworkServer *CreateDualStackServer(
    const char *certificatePath, const char *privateKeyPath,
    uint16_t requestedPort, uint16_t *outPort)
{
    uint32_t maximumAttempts = requestedPort == 0 ? 8U : 1U;
    for (uint32_t attempt = 0;
         attempt < maximumAttempts; ++attempt)
    {
        uint16_t port = requestedPort;
        if (port == 0 && !FindFreeDualStackPort(&port))
        {
            return NULL;
        }
        NetworkServerConfiguration configuration;
        NetworkServerConfigurationInitialize(&configuration);
        configuration.addressFamily = NETWORK_ADDRESS_FAMILY_DUAL;
        configuration.listenAddress = "*";
        configuration.port = port;
        configuration.maximumPeers = 2U;
        configuration.worldSeed = INT64_C(0x102030405060708);
        configuration.certificateFile = certificatePath;
        configuration.privateKeyFile = privateKeyPath;
        configuration.handshakeTimeoutMs = 5000U;
        configuration.idleTimeoutMs = 5000U;
        NetworkServer *server = NetworkServerCreate(&configuration);
        if (server != NULL)
        {
            *outPort = port;
            return server;
        }
    }
    return NULL;
}

static bool ConfigureClient(
    NetworkClientConfiguration *configuration, const char *host,
    uint16_t port, NetworkAddressFamily family, NetworkTrustMode trustMode,
    const char *pinHex)
{
    NetworkClientConfigurationInitialize(configuration);
    char endpoint[LAIUE_NETWORK_HOST_CAPACITY + 16U];
    int endpointLength =
        family == NETWORK_ADDRESS_FAMILY_IPV6
            ? snprintf(endpoint, sizeof(endpoint), "[%s]:%u",
                       host, (unsigned int)port)
            : snprintf(endpoint, sizeof(endpoint), "%s:%u",
                       host, (unsigned int)port);
    if (endpointLength <= 0 ||
        (size_t)endpointLength >= sizeof(endpoint) ||
        NetworkEndpointParse(endpoint, LAIUE_NETWORK_DEFAULT_PORT,
                             &configuration->endpoint) !=
            NETWORK_ENDPOINT_PARSE_OK)
    {
        return false;
    }
    configuration->addressFamily = family;
    configuration->trustMode = trustMode;
    configuration->handshakeTimeoutMs = 5000U;
    configuration->idleTimeoutMs = 5000U;
    if (trustMode == NETWORK_TRUST_SHA256)
    {
        if (pinHex == NULL || strlen(pinHex) != 64U)
        {
            return false;
        }
        char trust[sizeof("sha256:") + 64U];
        int trustLength =
            snprintf(trust, sizeof(trust), "sha256:%s", pinHex);
        NetworkTrustMode parsedMode = NETWORK_TRUST_SYSTEM;
        if (trustLength != (int)(sizeof(trust) - 1U) ||
            !NetworkTrustParse(
                trust, &parsedMode, configuration->certificateSha256) ||
            parsedMode != NETWORK_TRUST_SHA256)
        {
            return false;
        }
    }
    return NetworkClientConfigurationIsValid(configuration);
}

static bool SendInitialSnapshot(
    NetworkServer *server, uint32_t peerId)
{
    const NetworkSnapshotInfo snapshot = {
        .snapshotId = UINT64_C(0x1122334455667788),
        .worldRevision = UINT64_C(17),
        .serverTick = 1234U,
        .chunkCount = 1U,
        .peerId = peerId,
        .worldSeed = INT64_C(0x102030405060708),
        .worldTime = UINT64_C(4567),
        .requiresReadyBarrier = true,
    };
    const NetworkChunkDelta chunk = {
        .chunk = {0, 0, 0},
        .revision = UINT64_C(17),
        .partIndex = 0U,
        .partCount = 1U,
        .editCount = 1U,
        .edits = {{1U, 2U, 3U, 1U}},
    };
    NetworkInventoryState inventory;
    memset(&inventory, 0, sizeof(inventory));
    inventory.selectedHotbarSlot = 2U;
    inventory.slots[2].item = 3U;
    inventory.slots[2].count = 8U;
    const NetworkPlayerState playerState = {
        .serverTick = 1234U,
        .peerId = peerId,
        .lastProcessedInputSequence = 0U,
        .position = {1.0, 2.0, 3.0},
        .yaw = 0.5f,
        .pitch = -0.25f,
        .locomotionVelocityX = 2.5,
        .locomotionVelocityY = -1.25,
        .verticalVelocity = 4.0,
        .externalVelocityX = 0.5,
        .externalVelocityY = -0.75,
        .jumpBufferRemaining = 0.1,
        .coyoteTimeRemaining = 0.05,
        .colliderCrouchProgress = 0.25,
        .eyeCrouchProgress = 0.5,
        .airJumpsRemaining = 1,
        .crouchingRequested = true,
        .grounded = true,
    };
    const NetworkBlockDrop drop = {
        .id = 9U,
        .position = {4.0, 5.0, 6.0},
        .block = 1U,
    };
    const NetworkConstructReset constructReset = {
        .bodyCount = 1U,
    };
    const NetworkConstructBody constructBody = {
        .id = UINT64_C(77),
        .revision = UINT64_C(7),
        .origin = {8.0, 9.0, 10.0},
        .velocity = {0.5, 0.0, -0.25},
        .blockCount = 2U,
    };
    NetworkConstructBlockBatch constructBlocks;
    memset(&constructBlocks, 0, sizeof(constructBlocks));
    constructBlocks.id = constructBody.id;
    constructBlocks.revision = constructBody.revision;
    constructBlocks.blockCount = constructBody.blockCount;
    constructBlocks.blocks[0].local[0] = -1;
    constructBlocks.blocks[0].material = 1U;
    constructBlocks.blocks[1].mountNormal[1] = 1;
    constructBlocks.blocks[1].kind = 1U;
    const NetworkConstructState constructState = {
        .serverTick = snapshot.serverTick,
        .id = constructBody.id,
        .revision = constructBody.revision,
        .origin = {8.25, 9.0, 10.0},
        .velocity = {0.5, 0.0, -0.25},
        .heldBy = peerId,
    };
    return NetworkServerSendSnapshotBegin(server, peerId, &snapshot) &&
           NetworkServerSendSnapshotChunk(server, peerId, &chunk) &&
           !NetworkServerSendConstructBody(
               server, peerId, &constructBody) &&
           NetworkServerSendConstructReset(
               server, peerId, &constructReset) &&
           NetworkServerSendConstructBody(
               server, peerId, &constructBody) &&
           !NetworkServerSendSnapshotEnd(
               server, peerId, snapshot.snapshotId,
               snapshot.worldRevision) &&
           NetworkServerSendConstructBlocks(
               server, peerId, &constructBlocks) &&
           NetworkServerSendWorldTime(
               server, peerId, snapshot.worldTime) &&
           NetworkServerSendInventory(server, peerId, &inventory) &&
           NetworkServerSendPlayerJoined(server, peerId, peerId) &&
           NetworkServerSendPlayerState(
               server, peerId, &playerState) &&
           NetworkServerSendBlockDrop(server, peerId, &drop) &&
           NetworkServerSendConstructState(
               server, peerId, &constructState) &&
           NetworkServerSendSnapshotEnd(
               server, peerId, snapshot.snapshotId,
               snapshot.worldRevision);
}

static bool SendLiveSnapshot(
    NetworkServer *server, uint32_t peerId)
{
    const NetworkSnapshotInfo snapshot = {
        .snapshotId = UINT64_C(0x8877665544332211),
        .worldRevision = UINT64_C(18),
        .serverTick = 1235U,
        .chunkCount = 1U,
        .peerId = peerId,
        .worldSeed = INT64_C(0x102030405060708),
        .worldTime = UINT64_C(4568),
        .requiresReadyBarrier = false,
    };
    const NetworkChunkDelta chunk = {
        .chunk = {1, 0, 0},
        .revision = UINT64_C(18),
        .partIndex = 0U,
        .partCount = 1U,
        .editCount = 1U,
        .edits = {{4U, 5U, 6U, 2U}},
    };
    const NetworkConstructReset constructReset = {
        .bodyCount = 1U,
    };
    const NetworkConstructBody constructBody = {
        .id = UINT64_C(77),
        .revision = UINT64_C(8),
        .origin = {8.5, 9.0, 10.0},
        .velocity = {0.5, 0.0, -0.25},
        .blockCount = 2U,
    };
    NetworkConstructBlockBatch constructBlocks;
    memset(&constructBlocks, 0, sizeof(constructBlocks));
    constructBlocks.id = constructBody.id;
    constructBlocks.revision = constructBody.revision;
    constructBlocks.blockCount = constructBody.blockCount;
    constructBlocks.blocks[0].local[0] = -1;
    constructBlocks.blocks[0].material = 1U;
    constructBlocks.blocks[1].mountNormal[1] = 1;
    constructBlocks.blocks[1].kind = 1U;
    const NetworkConstructState futureState = {
        .serverTick = 1236U,
        .id = constructBody.id,
        .revision = constructBody.revision,
        .origin = {9.0, 9.0, 10.0},
        .velocity = {0.5, 0.0, -0.25},
        .heldBy = peerId,
    };
    // Control and auxiliary snapshot streams have no cross-stream ordering.
    // Queue the new-revision pose first to exercise that legal race.
    return NetworkServerSendConstructState(
               server, peerId, &futureState) &&
           NetworkServerSendSnapshotBegin(server, peerId, &snapshot) &&
           NetworkServerSendSnapshotChunk(server, peerId, &chunk) &&
           NetworkServerSendConstructReset(
               server, peerId, &constructReset) &&
           NetworkServerSendConstructBody(
               server, peerId, &constructBody) &&
           NetworkServerSendConstructBlocks(
               server, peerId, &constructBlocks) &&
           NetworkServerSendSnapshotEnd(
               server, peerId, snapshot.snapshotId,
               snapshot.worldRevision);
}

static bool HandleReadyServerEvents(
    NetworkServer *server, ReadyScenarioState *state)
{
    NetworkServerEvent event;
    while (NetworkServerPollEvent(server, &event))
    {
        if (event.type == NETWORK_SERVER_EVENT_CONNECTED)
        {
            if (state->serverConnected ||
                !SendInitialSnapshot(server, event.peerId))
            {
                return false;
            }
            state->serverConnected = true;
            state->peerId = event.peerId;
        }
        else if (event.type == NETWORK_SERVER_EVENT_INPUT)
        {
            if (event.peerId != state->peerId)
            {
                return false;
            }
            if (event.data.input.sequence == 41U)
            {
                if (!state->acknowledgementSent ||
                    state->inputReceived ||
                    !ApproximatelyEqual(
                        event.data.input.movementX, 0.25f) ||
                    !ApproximatelyEqual(
                        event.data.input.movementY, -0.5f) ||
                    !ApproximatelyEqual(
                        event.data.input.yaw, 0.75f) ||
                    !ApproximatelyEqual(
                        event.data.input.pitch, -0.25f) ||
                    !event.data.input.jumpPressed ||
                    event.data.input.jumpHeld ||
                    !event.data.input.sprintHeld ||
                    event.data.input.crouchHeld ||
                    !event.data.input.useHeld ||
                    !SendLiveSnapshot(server, event.peerId))
                {
                    return false;
                }
                state->inputReceived = true;
                state->liveSnapshotSent = true;
            }
            else if (event.data.input.sequence == 42U)
            {
                if (!state->liveSnapshotSent ||
                    !state->liveInputSent ||
                    state->liveInputReceived)
                {
                    return false;
                }
                state->liveInputReceived = true;
            }
            else
            {
                return false;
            }
        }
        else if (event.type == NETWORK_SERVER_EVENT_EDIT_INTENT)
        {
            if (event.peerId != state->peerId ||
                !state->leverEditSent || state->leverEditReceived ||
                event.data.editIntent.breakBlock ||
                !event.data.editIntent.placeBlock ||
                event.data.editIntent.placementBlock != 3U)
            {
                return false;
            }
            state->leverEditReceived = true;
        }
        else if (event.type == NETWORK_SERVER_EVENT_DISCONNECTED &&
                 !state->liveInputReceived)
        {
            return false;
        }
    }
    return true;
}

static bool HandleReadyClientEvents(
    NetworkClient *client, ReadyScenarioState *state)
{
    NetworkClientEvent event;
    while (NetworkClientPollEvent(client, &event))
    {
        switch (event.type)
        {
            case NETWORK_CLIENT_EVENT_SERVER_VERIFIED:
                state->serverVerified = true;
                break;
            case NETWORK_CLIENT_EVENT_SERVER_MODS:
                if (!state->serverVerified ||
                    state->modsSubmitted ||
                    event.data.serverMods.count != 0U ||
                    !NetworkClientSubmitMods(client, NULL, 0U))
                {
                    return false;
                }
                state->modsSubmitted = true;
                break;
            case NETWORK_CLIENT_EVENT_SNAPSHOT_BEGIN:
                if (event.data.snapshot.snapshotId ==
                    UINT64_C(0x1122334455667788))
                {
                    if (!state->serverConnected ||
                        state->snapshotBeginReceived ||
                        event.data.snapshot.chunkCount != 1U ||
                        !event.data.snapshot.requiresReadyBarrier)
                    {
                        return false;
                    }
                    state->snapshotBeginReceived = true;
                }
                else if (event.data.snapshot.snapshotId ==
                         UINT64_C(0x8877665544332211))
                {
                    const NetworkInputCommand input = {
                        .sequence = 42U,
                        .movementX = -0.25f,
                    };
                    if (!state->readyReceived ||
                        !state->liveSnapshotSent ||
                        state->liveSnapshotBeginReceived ||
                        event.data.snapshot.chunkCount != 1U ||
                        event.data.snapshot.requiresReadyBarrier ||
                        NetworkClientGetState(client) !=
                            NETWORK_CONNECTION_READY ||
                        !NetworkClientSendInput(client, &input))
                    {
                        return false;
                    }
                    state->liveSnapshotBeginReceived = true;
                    state->liveInputSent = true;
                }
                else
                {
                    return false;
                }
                break;
            case NETWORK_CLIENT_EVENT_SNAPSHOT_CHUNK:
                if (!state->snapshotChunkReceived)
                {
                    if (!state->snapshotBeginReceived ||
                        event.data.chunkDelta.chunk[0] != 0 ||
                        event.data.chunkDelta.chunk[1] != 0 ||
                        event.data.chunkDelta.chunk[2] != 0 ||
                        event.data.chunkDelta.revision != UINT64_C(17) ||
                        event.data.chunkDelta.partIndex != 0U ||
                        event.data.chunkDelta.partCount != 1U ||
                        event.data.chunkDelta.editCount != 1U ||
                        event.data.chunkDelta.edits[0].localX != 1U ||
                        event.data.chunkDelta.edits[0].localY != 2U ||
                        event.data.chunkDelta.edits[0].localZ != 3U ||
                        event.data.chunkDelta.edits[0].replacement != 1U)
                    {
                        return false;
                    }
                    state->snapshotChunkReceived = true;
                }
                else
                {
                    if (!state->liveSnapshotBeginReceived ||
                        state->liveSnapshotChunkReceived ||
                        NetworkClientGetState(client) !=
                            NETWORK_CONNECTION_READY ||
                        event.data.chunkDelta.chunk[0] != 1 ||
                        event.data.chunkDelta.revision != UINT64_C(18) ||
                        event.data.chunkDelta.editCount != 1U ||
                        event.data.chunkDelta.edits[0].localX != 4U ||
                        event.data.chunkDelta.edits[0].localY != 5U ||
                        event.data.chunkDelta.edits[0].localZ != 6U ||
                        event.data.chunkDelta.edits[0].replacement != 2U)
                    {
                        return false;
                    }
                    state->liveSnapshotChunkReceived = true;
                }
                break;
            case NETWORK_CLIENT_EVENT_CONSTRUCT_RESET:
                if (event.data.constructReset.bodyCount != 1U)
                {
                    return false;
                }
                if (!state->snapshotEndReceived)
                {
                    if (!state->snapshotBeginReceived ||
                        state->constructResetReceived)
                    {
                        return false;
                    }
                    state->constructResetReceived = true;
                }
                else
                {
                    if (!state->liveSnapshotBeginReceived ||
                        state->liveConstructResetReceived)
                    {
                        return false;
                    }
                    state->liveConstructResetReceived = true;
                }
                break;
            case NETWORK_CLIENT_EVENT_CONSTRUCT_BODY:
                if (event.data.constructBody.id != UINT64_C(77) ||
                    event.data.constructBody.blockCount != 2U)
                {
                    return false;
                }
                if (!state->snapshotEndReceived)
                {
                    if (!state->constructResetReceived ||
                        state->constructBodyReceived ||
                        event.data.constructBody.revision != UINT64_C(7))
                    {
                        return false;
                    }
                    state->constructBodyReceived = true;
                }
                else
                {
                    if (!state->liveConstructResetReceived ||
                        state->liveConstructBodyReceived ||
                        event.data.constructBody.revision != UINT64_C(8))
                    {
                        return false;
                    }
                    state->liveConstructBodyReceived = true;
                }
                break;
            case NETWORK_CLIENT_EVENT_CONSTRUCT_BLOCKS:
                if (event.data.constructBlocks.id != UINT64_C(77) ||
                    event.data.constructBlocks.firstBlock != 0U ||
                    event.data.constructBlocks.blockCount != 2U ||
                    event.data.constructBlocks.blocks[0].local[0] != -1 ||
                    event.data.constructBlocks.blocks[0].material != 1U ||
                    event.data.constructBlocks.blocks[1].kind != 1U ||
                    event.data.constructBlocks.blocks[1].mountNormal[1] != 1)
                {
                    return false;
                }
                if (!state->snapshotEndReceived)
                {
                    if (!state->constructBodyReceived ||
                        state->constructBlocksReceived ||
                        event.data.constructBlocks.revision != UINT64_C(7))
                    {
                        return false;
                    }
                    state->constructBlocksReceived = true;
                }
                else
                {
                    if (!state->liveConstructBodyReceived ||
                        state->liveConstructBlocksReceived ||
                        event.data.constructBlocks.revision != UINT64_C(8))
                    {
                        return false;
                    }
                    state->liveConstructBlocksReceived = true;
                }
                break;
            case NETWORK_CLIENT_EVENT_SNAPSHOT_END:
                if (event.data.snapshot.snapshotId ==
                    UINT64_C(0x1122334455667788))
                {
                    if (!state->snapshotChunkReceived ||
                        !state->constructBlocksReceived ||
                        state->snapshotEndReceived ||
                        event.data.snapshot.worldRevision != UINT64_C(17) ||
                        !event.data.snapshot.requiresReadyBarrier)
                    {
                        return false;
                    }
                    state->snapshotEndReceived = true;
                }
                else if (event.data.snapshot.snapshotId ==
                         UINT64_C(0x8877665544332211))
                {
                    if (!state->liveSnapshotChunkReceived ||
                        !state->liveConstructBlocksReceived ||
                        state->liveSnapshotEndReceived ||
                        event.data.snapshot.worldRevision != UINT64_C(18) ||
                        event.data.snapshot.requiresReadyBarrier ||
                        NetworkClientGetState(client) !=
                            NETWORK_CONNECTION_READY)
                    {
                        return false;
                    }
                    state->liveSnapshotEndReceived = true;
                }
                else
                {
                    return false;
                }
                break;
            case NETWORK_CLIENT_EVENT_INVENTORY_STATE:
                if (!state->snapshotEndReceived ||
                    event.data.inventory.selectedHotbarSlot != 2U ||
                    event.data.inventory.slots[2].item != 3U ||
                    event.data.inventory.slots[2].count != 8U)
                {
                    return false;
                }
                state->inventoryReceived = true;
                break;
            case NETWORK_CLIENT_EVENT_WORLD_TIME:
                if (!state->snapshotEndReceived ||
                    event.data.worldTime != UINT64_C(4567))
                {
                    return false;
                }
                state->worldTimeReceived = true;
                break;
            case NETWORK_CLIENT_EVENT_PLAYER_JOINED:
                if (!state->snapshotEndReceived ||
                    event.data.peerId != state->peerId)
                {
                    return false;
                }
                state->playerJoinedReceived = true;
                break;
            case NETWORK_CLIENT_EVENT_PLAYER_STATE:
                if (!state->snapshotEndReceived ||
                    event.data.playerState.peerId != state->peerId ||
                    event.data.playerState.serverTick != 1234U ||
                    event.data.playerState.lastProcessedInputSequence != 0U ||
                    event.data.playerState.locomotionVelocityX != 2.5 ||
                    event.data.playerState.locomotionVelocityY != -1.25 ||
                    event.data.playerState.verticalVelocity != 4.0 ||
                    event.data.playerState.externalVelocityX != 0.5 ||
                    event.data.playerState.externalVelocityY != -0.75 ||
                    event.data.playerState.jumpBufferRemaining != 0.1 ||
                    event.data.playerState.coyoteTimeRemaining != 0.05 ||
                    event.data.playerState.colliderCrouchProgress != 0.25 ||
                    event.data.playerState.eyeCrouchProgress != 0.5 ||
                    event.data.playerState.airJumpsRemaining != 1 ||
                    !event.data.playerState.crouchingRequested ||
                    !event.data.playerState.grounded)
                {
                    return false;
                }
                state->playerStateReceived = true;
                break;
            case NETWORK_CLIENT_EVENT_BLOCK_DROP_SPAWN:
                if (!state->snapshotEndReceived ||
                    event.data.blockDrop.id != 9U ||
                    event.data.blockDrop.block != 1U)
                {
                    return false;
                }
                state->blockDropReceived = true;
                break;
            case NETWORK_CLIENT_EVENT_CONSTRUCT_STATE:
                if (event.data.constructState.revision == UINT64_C(7))
                {
                    if (!state->snapshotEndReceived ||
                        state->constructStateReceived ||
                        event.data.constructState.serverTick != 1234U ||
                        event.data.constructState.id != UINT64_C(77) ||
                        event.data.constructState.origin[0] != 8.25 ||
                        event.data.constructState.heldBy != state->peerId)
                    {
                        return false;
                    }
                    state->constructStateReceived = true;
                }
                else if (event.data.constructState.revision == UINT64_C(8))
                {
                    if (!state->readyReceived || !state->liveSnapshotSent ||
                        state->futureConstructStateReceived ||
                        event.data.constructState.serverTick != 1236U ||
                        event.data.constructState.id != UINT64_C(77) ||
                        event.data.constructState.origin[0] != 9.0 ||
                        event.data.constructState.heldBy != state->peerId)
                    {
                        return false;
                    }
                    state->futureConstructStateReceived = true;
                }
                else
                {
                    return false;
                }
                break;
            case NETWORK_CLIENT_EVENT_READY:
            {
                if (!state->snapshotEndReceived ||
                    !state->inventoryReceived ||
                    !state->worldTimeReceived ||
                    !state->playerJoinedReceived ||
                    !state->playerStateReceived ||
                    !state->blockDropReceived ||
                    !state->constructStateReceived ||
                    state->readyReceived ||
                    event.data.ready.peerId != state->peerId ||
                    !NetworkClientAcknowledgeReady(client))
                {
                    return false;
                }
                state->readyReceived = true;
                state->acknowledgementSent = true;
                const NetworkInputCommand input = {
                    .sequence = 41U,
                    .movementX = 0.25f,
                    .movementY = -0.5f,
                    .yaw = 0.75f,
                    .pitch = -0.25f,
                    .jumpPressed = true,
                    .jumpHeld = false,
                    .sprintHeld = true,
                    .crouchHeld = false,
                    .useHeld = true,
                };
                if (!NetworkClientSendInput(client, &input))
                {
                    return false;
                }
                state->inputSent = true;
                const float leverDirection[3] = {0.0f, 0.0f, -1.0f};
                if (!NetworkClientSendEditIntent(
                        client, false, true, 3U, leverDirection))
                {
                    return false;
                }
                state->leverEditSent = true;
                break;
            }
            case NETWORK_CLIENT_EVENT_DISCONNECTED:
                return false;
            default:
                break;
        }
    }
    return true;
}

static bool RunReadyScenario(
    const char *certificatePath, const char *privateKeyPath,
    const char *host, NetworkAddressFamily family,
    NetworkTrustMode trustMode, const char *pinHex,
    uint16_t requestedPort)
{
    uint16_t port = 0;
    NetworkServer *server = CreateDualStackServer(
        certificatePath, privateKeyPath, requestedPort, &port);
    if (server == NULL)
    {
        fprintf(stderr, "Could not start the dual-stack QUIC listener\n");
        return false;
    }

    NetworkClientConfiguration clientConfiguration;
    bool configured = ConfigureClient(
        &clientConfiguration, host, port, family, trustMode, pinHex);
    NetworkClient *client =
        configured ? NetworkClientCreate(&clientConfiguration) : NULL;
    if (client == NULL)
    {
        NetworkServerDestroy(server);
        fprintf(stderr, "Could not create the QUIC client\n");
        return false;
    }
    const NetworkInputCommand prematureInput = {
        .sequence = 1U,
        .movementX = 1.0f,
    };
    if (NetworkClientSendInput(client, &prematureInput))
    {
        NetworkClientDestroy(client);
        NetworkServerDestroy(server);
        fprintf(stderr, "Client accepted input before READY/SyncApplied\n");
        return false;
    }

    ReadyScenarioState state;
    memset(&state, 0, sizeof(state));
    uint64_t deadline = MonotonicMilliseconds() + TEST_CASE_TIMEOUT_MS;
    bool succeeded = true;
    while ((!state.liveInputReceived || !state.leverEditReceived ||
            !state.futureConstructStateReceived ||
            !state.liveSnapshotEndReceived) &&
           MonotonicMilliseconds() <= deadline)
    {
        NetworkServerUpdate(server);
        NetworkClientUpdate(client);
        if (!HandleReadyServerEvents(server, &state) ||
            !HandleReadyClientEvents(client, &state))
        {
            succeeded = false;
            break;
        }
        PumpSleep();
    }
    succeeded =
        succeeded && state.serverVerified && state.modsSubmitted &&
        state.serverConnected && state.snapshotBeginReceived &&
        state.snapshotChunkReceived && state.constructResetReceived &&
        state.constructBodyReceived && state.constructBlocksReceived &&
        state.constructStateReceived && state.snapshotEndReceived &&
        state.inventoryReceived && state.worldTimeReceived &&
        state.playerJoinedReceived && state.playerStateReceived &&
        state.blockDropReceived && state.readyReceived &&
        state.acknowledgementSent && state.inputSent &&
        state.inputReceived && state.leverEditSent &&
        state.leverEditReceived && state.liveSnapshotSent &&
        state.liveSnapshotBeginReceived &&
        state.liveSnapshotChunkReceived &&
        state.liveConstructResetReceived &&
        state.liveConstructBodyReceived &&
        state.liveConstructBlocksReceived &&
        state.futureConstructStateReceived &&
        state.liveSnapshotEndReceived &&
        state.liveInputSent && state.liveInputReceived;

    NetworkClientDestroy(client);
    NetworkServerDestroy(server);
    return succeeded;
}

static bool RunCertificateFailureScenario(
    const char *certificatePath, const char *privateKeyPath,
    NetworkTrustMode trustMode, const char *pinHex)
{
    uint16_t port = 0;
    NetworkServer *server = CreateDualStackServer(
        certificatePath, privateKeyPath, 0, &port);
    if (server == NULL)
    {
        return false;
    }

    char wrongPin[65] = {0};
    const char *selectedPin = NULL;
    if (trustMode == NETWORK_TRUST_SHA256)
    {
        if (pinHex == NULL || strlen(pinHex) != 64U)
        {
            NetworkServerDestroy(server);
            return false;
        }
        memcpy(wrongPin, pinHex, sizeof(wrongPin));
        wrongPin[0] = wrongPin[0] == '0' ? '1' : '0';
        selectedPin = wrongPin;
    }
    NetworkClientConfiguration clientConfiguration;
    bool configured = ConfigureClient(
        &clientConfiguration, "127.0.0.1", port,
        NETWORK_ADDRESS_FAMILY_IPV4, trustMode, selectedPin);
    NetworkClient *client =
        configured ? NetworkClientCreate(&clientConfiguration) : NULL;
    if (client == NULL)
    {
        NetworkServerDestroy(server);
        return false;
    }

    bool certificateDisconnect = false;
    bool protocolOrGameDataObserved = false;
    uint64_t deadline = MonotonicMilliseconds() + TEST_CASE_TIMEOUT_MS;
    while (!certificateDisconnect &&
           MonotonicMilliseconds() <= deadline)
    {
        NetworkServerUpdate(server);
        NetworkClientUpdate(client);

        NetworkServerEvent serverEvent;
        while (NetworkServerPollEvent(server, &serverEvent))
        {
            if (serverEvent.type == NETWORK_SERVER_EVENT_CONNECTED ||
                serverEvent.type == NETWORK_SERVER_EVENT_INPUT)
            {
                protocolOrGameDataObserved = true;
            }
        }
        NetworkClientEvent clientEvent;
        while (NetworkClientPollEvent(client, &clientEvent))
        {
            if (clientEvent.type == NETWORK_CLIENT_EVENT_DISCONNECTED)
            {
                certificateDisconnect =
                    clientEvent.data.disconnectReason ==
                    NETWORK_DISCONNECT_CERTIFICATE;
                if (!certificateDisconnect)
                {
                    fprintf(
                        stderr,
                        "Expected certificate disconnect, got reason %u\n",
                        (unsigned int)
                            clientEvent.data.disconnectReason);
                    protocolOrGameDataObserved = true;
                }
            }
            else
            {
                fprintf(
                    stderr,
                    "Unexpected event before certificate rejection: %u\n",
                    (unsigned int)clientEvent.type);
                protocolOrGameDataObserved = true;
            }
        }
        PumpSleep();
    }

    NetworkClientDestroy(client);
    NetworkServerDestroy(server);
    return certificateDisconnect && !protocolOrGameDataObserved;
}

static bool RunDnsFailureScenario(void)
{
    NetworkClientConfiguration configuration;
    NetworkClientConfigurationInitialize(&configuration);
    if (NetworkEndpointParse(
            "laiue-quic-dns-regression.invalid",
            LAIUE_NETWORK_DEFAULT_PORT, &configuration.endpoint) !=
            NETWORK_ENDPOINT_PARSE_OK ||
        configuration.endpoint.kind != NETWORK_ENDPOINT_DNS)
    {
        return false;
    }
    configuration.addressFamily = NETWORK_ADDRESS_FAMILY_AUTO;
    configuration.trustMode = NETWORK_TRUST_SYSTEM;
    configuration.handshakeTimeoutMs = 1500U;
    configuration.idleTimeoutMs = 1500U;

    NetworkClient *client = NetworkClientCreate(&configuration);
    if (client == NULL)
    {
        fprintf(stderr,
                "DNS failure returned NULL instead of an async client\n");
        return false;
    }

    bool dnsDisconnect = false;
    bool protocolOrGenericFailureObserved = false;
    uint64_t deadline =
        MonotonicMilliseconds() + DNS_CASE_TIMEOUT_MS;
    while (!dnsDisconnect &&
           MonotonicMilliseconds() <= deadline)
    {
        NetworkClientUpdate(client);
        NetworkClientEvent event;
        while (NetworkClientPollEvent(client, &event))
        {
            if (event.type == NETWORK_CLIENT_EVENT_DISCONNECTED)
            {
                dnsDisconnect =
                    event.data.disconnectReason ==
                    NETWORK_DISCONNECT_DNS;
                if (!dnsDisconnect)
                {
                    fprintf(
                        stderr,
                        "Expected DNS disconnect, got reason %u\n",
                        (unsigned int)event.data.disconnectReason);
                    protocolOrGenericFailureObserved = true;
                }
            }
            else
            {
                fprintf(
                    stderr,
                    "Unexpected event before DNS failure: %u\n",
                    (unsigned int)event.type);
                protocolOrGenericFailureObserved = true;
            }
        }
        PumpSleep();
    }

    NetworkClientDestroy(client);
    return dnsDisconnect && !protocolOrGenericFailureObserved;
}

int main(int argumentCount, char **arguments)
{
    if (argumentCount != 5)
    {
        fprintf(stderr,
                "Usage: %s MODE certificate.pem private-key.pem "
                "sha256-hex\n",
                argumentCount > 0 ? arguments[0] : "quic-integration");
        return 2;
    }
    const char *mode = arguments[1];
    if (strcmp(mode, "dns-failure") == 0)
    {
        bool succeeded =
            Expect(NetworkGetSecureTransportStatus() ==
                       NETWORK_SECURE_TRANSPORT_AVAILABLE,
                   "MsQuic is not available") &&
            Expect(RunDnsFailureScenario(),
                   "reserved .invalid name did not produce an exact "
                   "asynchronous DNS disconnect");
        if (succeeded)
        {
            fprintf(stdout, "QUIC %s checks passed: %u\n",
                    mode, integrationChecks);
        }
        return succeeded ? 0 : 1;
    }

    SecureCredentials credentials;
    if (!PrepareSecureCredentials(
            arguments[2], arguments[3], &credentials))
    {
        fprintf(stderr,
                "Could not stage secure test credentials under /tmp\n");
        return 1;
    }
    const char *certificatePath = credentials.certificatePath;
    const char *privateKeyPath = credentials.privateKeyPath;
    const char *pinHex = arguments[4];

    bool succeeded =
        Expect(NetworkGetSecureTransportStatus() ==
                    NETWORK_SECURE_TRANSPORT_AVAILABLE,
                "MsQuic is not available") &&
        Expect(PrivateKeyIsSecure(privateKeyPath),
               "test private key is not a regular 0600 file") &&
        Expect(strlen(pinHex) == 64U,
               "certificate fingerprint is not SHA-256 hex");
    if (!succeeded)
    {
        // The shared preconditions already reported the failure.
    }
    else if (strcmp(mode, "pin") == 0)
    {
        succeeded = Expect(RunReadyScenario(
                    certificatePath, privateKeyPath, "127.0.0.1",
                    NETWORK_ADDRESS_FAMILY_IPV4,
                    NETWORK_TRUST_SHA256, pinHex,
                    LAIUE_NETWORK_DEFAULT_PORT),
               "IPv4 exact-pin connection did not reach READY/input");
        if (succeeded && Ipv6LoopbackAvailable())
        {
            succeeded = Expect(RunReadyScenario(
                            certificatePath, privateKeyPath, "::1",
                            NETWORK_ADDRESS_FAMILY_IPV6,
                            NETWORK_TRUST_SHA256, pinHex, 0),
                        "IPv6 client could not use the dual-stack listener");
        }
        else if (succeeded)
        {
            fprintf(stdout,
                    "IPv6 loopback is unavailable; optional ::1 case "
                    "skipped\n");
        }
    }
    else if (strcmp(mode, "system") == 0)
    {
        succeeded = Expect(RunReadyScenario(
                               certificatePath, privateKeyPath,
                               "127.0.0.1",
                               NETWORK_ADDRESS_FAMILY_IPV4,
                               NETWORK_TRUST_SYSTEM, NULL, 0),
                           "IPv4 system-trust connection did not reach "
                           "READY/input");
        if (succeeded)
        {
            succeeded = Expect(
                RunReadyScenario(
                    certificatePath, privateKeyPath, "localhost",
                    NETWORK_ADDRESS_FAMILY_AUTO,
                    NETWORK_TRUST_SYSTEM, NULL, 0),
                "DNS system-trust connection did not preserve SNI/SAN "
                "while using the resolved QUIC address");
        }
    }
    else if (strcmp(mode, "wrong-pin") == 0)
    {
        succeeded = Expect(RunCertificateFailureScenario(
                               certificatePath, privateKeyPath,
                               NETWORK_TRUST_SHA256, pinHex),
                           "wrong pin was not rejected before "
                           "protocol/game data");
    }
    else if (strcmp(mode, "untrusted-system") == 0)
    {
        succeeded = Expect(RunCertificateFailureScenario(
                               certificatePath, privateKeyPath,
                               NETWORK_TRUST_SYSTEM, NULL),
                           "untrusted private CA was accepted without "
                           "a pin or system trust");
    }
    else
    {
        fprintf(stderr, "Unknown integration mode: %s\n", mode);
        succeeded = false;
    }

    if (succeeded)
    {
        fprintf(stdout, "QUIC %s checks passed: %u\n",
                mode, integrationChecks);
    }
    DestroySecureCredentials(&credentials);
    return succeeded ? 0 : 1;
}
