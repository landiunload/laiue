// Фаззер транспорта laiue.net. Первый байт выбирает, кем притворяется вход:
//
// чётный — сетью без ключей. Датаграммы приходят серверу и клиенту:
//          запросы, ответы рукопожатия, короткие пакеты с чужими CID. Метку
//          обратного пути сервер выдаёт сам, и по флагу фаззер подставляет
//          её в запрос, так что мутации доходят до приёма соединения.
//          Настоящие датаграммы сторон можно переслать друг другу, испортив
//          бит, — так проходит и полное рукопожатие.
// нечётный — собеседником, который знает ключи. Фаззер заводит принятого
//          участника и шифрует каждый свой пакет его ключом приёма: мутации
//          доходят до разбора кадров, подтверждений, обнаружения потерь,
//          сборки сообщений и событий.
//
// Дальше идут записи: байт действия и его данные. Время движет фаззер, а
// случайные байты конечных точек детерминированы, поэтому падение
// воспроизводится тем же входом.
//
// Транспорт собран в фаззер из исходников модуля: кадры разбираются только
// после расшифровки, а ключи участника доступны лишь изнутри.

#include "net/net_crypto.h"
#include "net/net_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_INPUT_LIMIT (256u * 1024u)
#define FUZZ_QUEUE 32u
#define FUZZ_HANDLES 8u
#define FUZZ_PROTOCOL UINT64_C(0x7A7A75662E74656E)
#define FUZZ_SERVER_CID UINT64_C(0x0123456789ABCDEF)
#define FUZZ_CLIENT_CID UINT64_C(0x1122334455667788)
#define FUZZ_START (UINT64_C(1000000))

/* ---- Вход ------------------------------------------------------------------------------ */

typedef struct FuzzInput
{
    const uint8_t *data;
    size_t size;
    size_t at;
} FuzzInput;

static bool InputLeft(const FuzzInput *input)
{
    return input->at < input->size;
}

static uint8_t TakeByte(FuzzInput *input)
{
    return input->at < input->size ? input->data[input->at++] : 0u;
}

/* Длина в два байта, не больше оставшегося и limit. */
static uint32_t TakeLength(FuzzInput *input, uint32_t limit)
{
    uint32_t length = TakeByte(input);
    length |= (uint32_t)TakeByte(input) << 8u;
    const size_t left = input->size - input->at;
    if (length > left)
        length = (uint32_t)left;
    return length < limit ? length : limit;
}

static const uint8_t *TakeBytes(FuzzInput *input, uint32_t size)
{
    const uint8_t *bytes = input->data + input->at;
    input->at += size;
    return bytes;
}

/* ---- Сеть фаззера ---------------------------------------------------------------------- */

typedef struct FuzzDatagram
{
    LaiueNetAddress address;
    uint32_t size;
    uint8_t bytes[NET_DATAGRAM_MAXIMUM + 1u];
} FuzzDatagram;

/* Кольцо: полное вытесняет старейшую датаграмму, как перегруженный путь. */
typedef struct FuzzQueue
{
    FuzzDatagram items[FUZZ_QUEUE];
    uint32_t head;
    uint32_t count;
} FuzzQueue;

static FuzzDatagram *QueuePush(FuzzQueue *queue)
{
    if (queue->count == FUZZ_QUEUE)
    {
        queue->head = (queue->head + 1u) % FUZZ_QUEUE;
        --queue->count;
    }
    FuzzDatagram *datagram = &queue->items[(queue->head + queue->count) % FUZZ_QUEUE];
    ++queue->count;
    return datagram;
}

static const FuzzDatagram *QueuePop(FuzzQueue *queue)
{
    if (queue->count == 0u)
        return NULL;
    const FuzzDatagram *datagram = &queue->items[queue->head];
    queue->head = (queue->head + 1u) % FUZZ_QUEUE;
    --queue->count;
    return datagram;
}

typedef struct FuzzPort
{
    LaiueNetAddress address;
    LaiueNetDriverV1 driver;
    LaiueNetEndpoint *endpoint;
    uint32_t maxPeers;
    uint64_t randomState;
    FuzzQueue inbox;
    FuzzQueue outbox;
    LaiueNetPeer handles[FUZZ_HANDLES];
    uint32_t handleNext;
    /* Клиент: CID из его последнего запроса. Сервер: последний запрос извне,
     * к которому подходит выданная в ответ метка. */
    uint64_t helloCid;
    uint8_t hello[NET_HELLO_FIXED_SIZE];
    LaiueNetAddress helloFrom;
    bool helloSeen;
} FuzzPort;

static FuzzPort ports[2];
static volatile uint8_t fuzzSink;

static int32_t PortSend(void *context, const LaiueNetAddress *to, const void *bytes, uint32_t size)
{
    FuzzPort *port = context;
    /* Транспорт не отправляет пустых датаграмм и больших, чем обещал. */
    if (size == 0u || size > NET_DATAGRAM_MAXIMUM)
        abort();
    const uint8_t *data = bytes;
    if (data[0] == NET_PACKET_HELLO && size >= NET_HELLO_FIXED_SIZE)
        port->helloCid = NetLoad64(data + 12);
    FuzzDatagram *datagram = QueuePush(&port->outbox);
    datagram->address = *to;
    datagram->size = size;
    memcpy(datagram->bytes, data, size);
    return 1;
}

static int32_t PortReceive(void *context, LaiueNetAddress *outFrom, void *buffer, uint32_t capacity)
{
    FuzzPort *port = context;
    const FuzzDatagram *datagram = QueuePop(&port->inbox);
    if (datagram == NULL)
        return 0;
    const uint32_t size = datagram->size < capacity ? datagram->size : capacity;
    memcpy(buffer, datagram->bytes, size);
    *outFrom = datagram->address;
    return (int32_t)size;
}

static bool FuzzRandom(void *context, void *output, uint32_t size)
{
    uint64_t *state = context;
    uint8_t *bytes = output;
    for (uint32_t index = 0u; index < size; ++index)
    {
        *state ^= *state << 13u;
        *state ^= *state >> 7u;
        *state ^= *state << 17u;
        bytes[index] = (uint8_t)(*state >> 32u);
    }
    return true;
}

static LaiueNetAddress MakeAddress(uint8_t host, uint16_t port)
{
    LaiueNetAddress address;
    memset(&address, 0, sizeof(address));
    address.family = LAIUE_NET_FAMILY_IPV4;
    address.ip[0] = 10u;
    address.ip[3] = host;
    address.port = port;
    return address;
}

static void PortBegin(FuzzPort *port, uint8_t host, uint64_t seed)
{
    memset(port, 0, sizeof(*port));
    port->address = MakeAddress(host, (uint16_t)(4000u + host));
    port->driver.structSize = sizeof(port->driver);
    port->driver.context = port;
    port->driver.send = PortSend;
    port->driver.receive = PortReceive;
    port->randomState = seed;
}

static bool PortOpen(FuzzPort *port, LaiueNetEndpointDesc *desc)
{
    desc->structSize = sizeof(*desc);
    desc->driver = &port->driver;
    desc->protocolId = FUZZ_PROTOCOL;
    desc->random = FuzzRandom;
    desc->randomContext = &port->randomState;
    port->maxPeers = desc->maxPeers == 0u ? 64u : desc->maxPeers;
    return LaiueNetEndpointCreate(desc, &port->endpoint) == LAIUE_NET_OK;
}

/* Забирает события и проверяет то, что обещано приложению. */
static void PortDrain(FuzzPort *port)
{
    LaiueNetEvent event;
    memset(&event, 0, sizeof(event));
    event.structSize = sizeof(event);
    while (LaiueNetPollEvent(port->endpoint, &event))
    {
        if (event.type == LAIUE_NET_EVENT_CONNECTED)
        {
            port->handles[port->handleNext % FUZZ_HANDLES] = event.peer;
            ++port->handleNext;
        }
        else if (event.type == LAIUE_NET_EVENT_MESSAGE)
        {
            if (event.channel >= LAIUE_NET_MAX_CHANNELS || event.delivery > LAIUE_NET_SEQUENCED ||
                (event.size != 0u && event.data == NULL))
                abort();
            /* Санитайзер проверяет, что байты события живы и все на месте. */
            const uint8_t *data = event.data;
            uint8_t sum = 0u;
            for (uint32_t index = 0u; index < event.size; ++index)
                sum ^= data[index];
            fuzzSink = sum;
        }
        else if (event.type != LAIUE_NET_EVENT_DISCONNECTED)
        {
            abort();
        }
        memset(&event, 0, sizeof(event));
        event.structSize = sizeof(event);
    }
    LaiueNetEndpointStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.structSize = sizeof(stats);
    if (LaiueNetEndpointGetStats(port->endpoint, &stats) != LAIUE_NET_OK ||
        stats.peerCount > port->maxPeers)
        abort();
}

static void PortStep(FuzzPort *port, uint64_t now)
{
    if (port->endpoint == NULL)
        return;
    (void)LaiueNetUpdate(port->endpoint, now);
    PortDrain(port);
}

static void PortEnd(FuzzPort *port)
{
    LaiueNetEndpointDestroy(port->endpoint);
    port->endpoint = NULL;
}

static uint64_t AdvanceTime(FuzzInput *input, uint8_t action, uint64_t now)
{
    /* Мелкий шаг — миллисекунды, крупный — до шестнадцати секунд: так
     * достижимы и таймеры потерь, и тайм-ауты соединения. */
    const uint64_t unit = (action & 0x08u) != 0u ? 64000u : 1000u;
    return now + ((uint64_t)TakeByte(input) + 1u) * unit;
}

/* Сообщение, отключение и сведения по одному из известных описателей. */
static void ApplicationCall(FuzzInput *input, FuzzPort *port, uint8_t action)
{
    const LaiueNetPeer peer = port->handles[TakeByte(input) % FUZZ_HANDLES];
    const uint8_t choice = TakeByte(input);
    if ((action & 0x10u) == 0u)
    {
        const uint32_t channel = choice % (LAIUE_NET_MAX_CHANNELS + 1u);
        const LaiueNetDelivery delivery = (choice >> 6u) & 3u;
        const uint32_t size = TakeLength(input, 0xFFFFu);
        (void)LaiueNetSend(port->endpoint, peer, channel, delivery, TakeBytes(input, size), size);
        return;
    }
    if ((choice & 0x80u) != 0u)
    {
        LaiueNetPeerInfo info;
        memset(&info, 0, sizeof(info));
        info.structSize = sizeof(info);
        if (LaiueNetPeerGetInfo(port->endpoint, peer, &info) == LAIUE_NET_OK &&
            info.state != LAIUE_NET_PEER_CONNECTING && info.state != LAIUE_NET_PEER_CONNECTED &&
            info.state != LAIUE_NET_PEER_DISCONNECTING)
            abort();
        return;
    }
    (void)LaiueNetDisconnect(port->endpoint, peer, choice & 0x3Fu,
                             (choice & 0x40u) != 0u ? LAIUE_NET_DISCONNECT_FLUSH : 0u);
}

/* ---- Сеть без ключей ------------------------------------------------------------------- */

static const uint8_t fuzzIdentity[LAIUE_NET_KEY_BYTES] = {7u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
static const uint8_t fuzzSecret[LAIUE_NET_KEY_BYTES] = {9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u};
/* Открытый ключ сервера считается один раз: умножение на кривой — самая
 * дорогая часть прогона. */
static uint8_t fuzzIdentityPublic[LAIUE_NET_KEY_BYTES];
static bool fuzzIdentityReady;

/* Датаграмма извне. Сервер получает её от одного из четырёх адресов;
 * флаг подставляет метку, выданную на последний запрос, вместе с теми
 * полями запроса, к которым она привязана. Клиенту флаг подставляет его CID
 * и адрес сервера: ответ проходит первую проверку. */
static void Inject(FuzzInput *input, FuzzPort *target, const FuzzPort *server, uint8_t action)
{
    const uint32_t size = TakeLength(input, NET_DATAGRAM_MAXIMUM + 1u);
    const uint8_t *bytes = TakeBytes(input, size);
    if (size == 0u)
        return;
    FuzzDatagram *datagram = QueuePush(&target->inbox);
    datagram->size = size;
    memcpy(datagram->bytes, bytes, size);
    datagram->address = MakeAddress((uint8_t)(100u + ((action >> 4u) & 3u)), 5000u);
    const bool patch = (action & 0x40u) != 0u;
    if (target == server)
    {
        if (patch && target->helloSeen && size >= NET_HELLO_FIXED_SIZE + 1u + NET_COOKIE_SIZE)
        {
            for (uint32_t index = target->outbox.count; index != 0u; --index)
            {
                const FuzzDatagram *reply =
                    &target->outbox.items[(target->outbox.head + index - 1u) % FUZZ_QUEUE];
                if (reply->bytes[0] != NET_PACKET_RETRY || reply->size != NET_RETRY_SIZE)
                    continue;
                memcpy(datagram->bytes + 4, target->hello + 4, NET_HELLO_FIXED_SIZE - 20u);
                datagram->bytes[NET_HELLO_FIXED_SIZE] = NET_COOKIE_SIZE;
                memcpy(datagram->bytes + NET_HELLO_FIXED_SIZE + 1u, reply->bytes + 11,
                       NET_COOKIE_SIZE);
                datagram->address = target->helloFrom;
                break;
            }
        }
        else if (datagram->bytes[0] == NET_PACKET_HELLO && size >= NET_HELLO_FIXED_SIZE)
        {
            memcpy(target->hello, datagram->bytes, NET_HELLO_FIXED_SIZE);
            target->helloFrom = datagram->address;
            target->helloSeen = true;
        }
    }
    else if (patch)
    {
        if (size >= 11u)
            NetStore64(datagram->bytes + 3, target->helloCid);
        datagram->address = server->address;
    }
}

/* Пересылает старейшую датаграмму одной стороны другой, если она адресована
 * ей; флаг портит в ней один бит. */
static void Relay(FuzzInput *input, FuzzPort *from, FuzzPort *to, uint8_t action)
{
    const FuzzDatagram *sent = QueuePop(&from->outbox);
    const uint8_t position = TakeByte(input);
    const uint8_t bit = TakeByte(input);
    if (sent == NULL || !LaiueNetAddressEqual(&sent->address, &to->address))
        return;
    FuzzDatagram *datagram = QueuePush(&to->inbox);
    *datagram = *sent;
    datagram->address = from->address;
    if ((action & 0x40u) != 0u)
        datagram->bytes[position % datagram->size] ^= (uint8_t)(1u << (bit & 7u));
}

static void RunWire(FuzzInput *input)
{
    FuzzPort *server = &ports[0];
    FuzzPort *client = &ports[1];
    const uint8_t config = TakeByte(input);
    PortBegin(server, 1u, UINT64_C(0x9E3779B97F4A7C15));
    PortBegin(client, 2u, UINT64_C(0xD1B54A32D192ED03));
    /* Биты настройки: 0–1 — предел соединений сервера, 2–3 — его каналы,
     * 4 — клиент соединяется сразу, 5 — у сервера есть ключ, 6 — общий
     * секрет у обоих, 7 — клиент закрепляет ключ сервера. */
    LaiueNetEndpointDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.flags = LAIUE_NET_ENDPOINT_ACCEPT;
    desc.maxPeers = 1u + (config & 3u);
    desc.channelCount = 1u + ((config >> 2u) & 3u);
    desc.identityPrivateKey = (config & 0x20u) != 0u ? fuzzIdentity : NULL;
    desc.presharedKey = (config & 0x40u) != 0u ? fuzzSecret : NULL;
    const bool serverOpen = PortOpen(server, &desc);
    desc.flags = 0u;
    desc.maxPeers = 4u;
    desc.channelCount = 2u;
    desc.identityPrivateKey = NULL;
    if (!serverOpen || !PortOpen(client, &desc))
    {
        PortEnd(server);
        PortEnd(client);
        return;
    }
    if ((config & 0x80u) != 0u && !fuzzIdentityReady)
        fuzzIdentityReady = LaiueNetPublicKey(fuzzIdentity, fuzzIdentityPublic) == LAIUE_NET_OK;
    LaiueNetConnectDesc connect;
    memset(&connect, 0, sizeof(connect));
    connect.structSize = sizeof(connect);
    connect.address = server->address;
    connect.serverPublicKey = (config & 0x80u) != 0u ? fuzzIdentityPublic : NULL;
    LaiueNetPeer peer = 0u;
    if ((config & 0x10u) != 0u &&
        LaiueNetConnect(client->endpoint, &connect, &peer) == LAIUE_NET_OK)
        client->handles[client->handleNext++ % FUZZ_HANDLES] = peer;

    uint64_t now = FUZZ_START;
    PortStep(server, now);
    PortStep(client, now);
    while (InputLeft(input))
    {
        const uint8_t action = TakeByte(input);
        switch (action & 7u)
        {
        case 0u:
            Inject(input, server, server, action);
            break;
        case 1u:
            Inject(input, client, server, action);
            break;
        case 2u:
            Relay(input, server, client, action);
            break;
        case 3u:
            Relay(input, client, server, action);
            break;
        case 4u:
            now = AdvanceTime(input, action, now);
            break;
        case 5u:
            ApplicationCall(input, (action & 0x20u) != 0u ? client : server, action);
            break;
        case 6u:
            /* Ещё одно соединение: к серверу или в пустоту. */
            connect.address = (action & 0x08u) != 0u ? MakeAddress(99u, 1u) : server->address;
            if (LaiueNetConnect(client->endpoint, &connect, &peer) == LAIUE_NET_OK)
                client->handles[client->handleNext++ % FUZZ_HANDLES] = peer;
            break;
        default:
            /* Ответы, которые никто не перешлёт, уходят в никуда. */
            (void)QueuePop((action & 0x08u) != 0u ? &client->outbox : &server->outbox);
            break;
        }
        PortStep(server, now);
        PortStep(client, now);
    }
    PortEnd(client);
    PortEnd(server);
}

/* ---- Собеседник с ключами -------------------------------------------------------------- */

static void RunFrames(FuzzInput *input)
{
    FuzzPort *server = &ports[0];
    const uint8_t config = TakeByte(input);
    PortBegin(server, 1u, UINT64_C(0x9E3779B97F4A7C15));
    LaiueNetEndpointDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.flags = LAIUE_NET_ENDPOINT_ACCEPT;
    desc.maxPeers = 2u;
    desc.channelCount = 1u + (config & 7u);
    /* Малые пределы делают достижимыми переполнение окна, очереди и
     * памяти событий. */
    if ((config & 0x08u) != 0u)
    {
        desc.maxMessageBytes = 3000u;
        desc.receiveWindowBytes = 4096u;
        desc.sendBufferBytes = 8192u;
        desc.eventBacklogBytes = 4096u;
    }
    desc.maxDatagramBytes = (config & 0x10u) != 0u ? NET_DATAGRAM_MINIMUM : 0u;
    /* Подтверждения и потери ведут модель пути BBR, а не окно CUBIC. */
    desc.congestionControl = (config & 0x20u) != 0u ? LAIUE_NET_CONGESTION_BBR : 0u;
    if (!PortOpen(server, &desc))
        return;
    uint64_t now = FUZZ_START;
    PortStep(server, now);

    /* Участник в том виде, в каком его оставляет принятый запрос: ключи
     * выведены, приложение о нём ещё не знает. Параметры собеседника — из
     * входа, если они допустимы. */
    LaiueNetEndpoint *endpoint = server->endpoint;
    NetParameters remote = endpoint->local;
    if (input->size - input->at >= NET_PARAMETERS_SIZE)
    {
        NetParameters parsed;
        if (NetParametersRead(TakeBytes(input, NET_PARAMETERS_SIZE), &parsed))
            remote = parsed;
    }
    const LaiueNetAddress home = MakeAddress(100u, 5000u);
    const LaiueNetAddress away = MakeAddress(101u, 5001u);
    const uint64_t cid = FUZZ_SERVER_CID;
    LaiueNetStatus status = LAIUE_NET_OK;
    NetPeer *created = NetPeerCreate(endpoint, &home, &cid, &status);
    if (created == NULL)
    {
        PortEnd(server);
        return;
    }
    uint8_t key[NET_KEY_SIZE];
    memset(key, 0x5A, sizeof(key));
    created->remoteCid = FUZZ_CLIENT_CID;
    created->remote = remote;
    memcpy(created->receiveKey, key, sizeof(key));
    memset(created->sendKey, 0xA5, NET_KEY_SIZE);
    created->handshakeDeadline = now + endpoint->handshakeTimeout;
    created->helloSentAt = now;
    NetPeerSetState(endpoint, created, NET_STATE_HALF_OPEN);

    uint64_t next = 0u;
    while (InputLeft(input))
    {
        const uint8_t action = TakeByte(input);
        switch (action & 3u)
        {
        case 0u:
        {
            /* Номер: следующий, давний повтор либо скачок вперёд. */
            uint64_t number = next;
            const uint8_t step = TakeByte(input);
            if ((action & 0x0Cu) == 0x04u)
                number = next > step ? next - 1u - step : 0u;
            else if ((action & 0x0Cu) == 0x08u)
                number = next + step;
            else if ((action & 0x0Cu) == 0x0Cu)
                number = next + ((uint64_t)step << 24u);
            if (number >= next)
                next = number + 1u;
            const uint32_t size = TakeLength(input, NET_DATAGRAM_MAXIMUM + 1u - NET_SHORT_OVERHEAD);
            const uint8_t *frames = TakeBytes(input, size);
            FuzzDatagram *datagram = QueuePush(&server->inbox);
            uint8_t *packet = datagram->bytes;
            uint8_t nonce[NET_NONCE_SIZE] = {0u};
            packet[0] = NET_PACKET_SHORT;
            NetStore64(packet + 1, cid);
            NetStore32(packet + 9, (uint32_t)number);
            NetStore64(nonce + 4, number);
            memcpy(packet + NET_SHORT_HEADER_SIZE, frames, size);
            NetAeadSeal(key, nonce, packet, NET_SHORT_HEADER_SIZE, packet + NET_SHORT_HEADER_SIZE,
                        size, packet + NET_SHORT_HEADER_SIZE,
                        packet + NET_SHORT_HEADER_SIZE + size);
            datagram->size = NET_SHORT_OVERHEAD + size;
            /* Пакет с другого адреса уводит соединение на новый путь. */
            datagram->address = (action & 0x10u) != 0u ? away : home;
            if ((action & 0x20u) != 0u)
                packet[step % datagram->size] ^= 0x01u;
            break;
        }
        case 1u:
            now = AdvanceTime(input, action, now);
            break;
        case 2u:
            ApplicationCall(input, server, action);
            break;
        default:
            /* Ответы стороны никому не нужны; очередь лишь чистится. */
            (void)QueuePop(&server->outbox);
            break;
        }
        PortStep(server, now);
    }
    PortEnd(server);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1u || size > FUZZ_INPUT_LIMIT)
        return 0;
    FuzzInput input = {.data = data, .size = size, .at = 1u};
    if ((data[0] & 1u) != 0u)
        RunFrames(&input);
    else
        RunWire(&input);
    return 0;
}
