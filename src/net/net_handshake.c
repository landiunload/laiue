#include "net/net_internal.h"

#include "platform/system.h"

#include <string.h>

/* Рукопожатие.
 *
 *   клиент                                   сервер
 *   HELLO без метки, дополнен до датаграммы ->
 *                                <- RETRY с меткой; состояния у сервера нет
 *   HELLO с меткой                          ->
 *                                <- ACCEPT; участник создан, ждёт пакета
 *   первый пакет данных                     ->  соединение установлено
 *
 * Метка привязана к адресу и запросу клиента, поэтому вернуть её может
 * только тот, кто получает пакеты по этому адресу. До неё сервер ничего не
 * хранит и не считает дорогих функций, а любой его ответ короче запроса —
 * отражать и усиливать трафик через него бессмысленно.
 *
 * Ключи: обе стороны дают эфемерный ключ X25519. В общий секрет входят
 * произведение эфемерных ключей, при закреплённом ключе сервера —
 * произведение эфемерного ключа клиента и постоянного ключа сервера, и,
 * если задан, общий секрет приложения. Соль — хеш открытой части
 * рукопожатия, так что подмена любого её байта даёт сторонам разные ключи.
 * ACCEPT несёт параметры сервера, зашифрованные выведенным ключом: клиент
 * открывает их только у сервера, владеющего теми же секретами. */

/* Смещения в HELLO. */
#define HELLO_FLAGS 3u
#define HELLO_PROTOCOL 4u
#define HELLO_CLIENT_CID 12u
#define HELLO_EPHEMERAL 20u
#define HELLO_PARAMETERS 52u
#define HELLO_COOKIE_SIZE 68u
#define HELLO_COOKIE 69u

/* Адрес, протокол, CID клиента и его эфемерный ключ: то, к чему привязана
 * метка и из чего выводится CID сервера. */
#define BINDING_SIZE (23u + 8u + 8u + NET_KEY_SIZE)

static const uint8_t zeroNonce[NET_NONCE_SIZE] = {0u};

static void Binding(const LaiueNetAddress *from, const uint8_t *hello, uint8_t output[BINDING_SIZE])
{
    memset(output, 0, 23u);
    /* У IPv4 значимы четыре байта: остальное драйвер заполнять не обязан. */
    memcpy(output, from->ip, from->family == LAIUE_NET_FAMILY_IPV4 ? 4u : 16u);
    NetStore32(output + 16, from->scope);
    NetStore16(output + 20, from->port);
    output[22] = from->family;
    memcpy(output + 23, hello + HELLO_PROTOCOL, 8u + 8u + NET_KEY_SIZE);
}

/* ---- Метки обратного пути ---------------------------------------------------------- */

static bool ServerKeysReady(LaiueNetEndpoint *endpoint)
{
    if (!endpoint->cookieReady)
    {
        if (!NetRandom(endpoint, endpoint->cookieKeys[0], NET_KEY_SIZE) ||
            !NetRandom(endpoint, endpoint->cookieKeys[1], NET_KEY_SIZE) ||
            !NetRandom(endpoint, endpoint->cidKey, NET_KEY_SIZE))
            return false;
        endpoint->cookieReady = true;
        endpoint->cookieRotateAt = endpoint->now + NET_COOKIE_LIFETIME;
    }
    else if (endpoint->now >= endpoint->cookieRotateAt)
    {
        /* Сменяется старший из двух ключей: метка живёт от одного до двух
         * сроков. Ключи меняются по запросу, а не по таймеру, поэтому после
         * долгой тишины пропущено больше одной смены и устарели оба. При
         * отказе генератора прежние ключи служат дальше. */
        const uint8_t next = (uint8_t)(endpoint->cookieEpoch + 1u);
        const bool both = endpoint->now - endpoint->cookieRotateAt >= NET_COOKIE_LIFETIME;
        if (NetRandom(endpoint, endpoint->cookieKeys[next & 1u], NET_KEY_SIZE) &&
            (!both ||
             NetRandom(endpoint, endpoint->cookieKeys[endpoint->cookieEpoch & 1u], NET_KEY_SIZE)))
        {
            endpoint->cookieEpoch = next;
            endpoint->cookieRotateAt = endpoint->now + NET_COOKIE_LIFETIME;
        }
    }
    return true;
}

static bool CookieCompute(const LaiueNetEndpoint *endpoint, uint8_t epoch,
                          const LaiueNetAddress *from, const uint8_t *hello,
                          uint8_t cookie[NET_COOKIE_SIZE])
{
    uint8_t binding[BINDING_SIZE];
    uint8_t mac[NET_HASH_SIZE];
    Binding(from, hello, binding);
    if (!NetHmacSha256(endpoint->cookieKeys[epoch & 1u], NET_KEY_SIZE, binding, sizeof(binding),
                       mac))
        return false;
    cookie[0] = epoch;
    memcpy(cookie + 1, mac, NET_COOKIE_SIZE - 1u);
    return true;
}

static bool CookieValid(const LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                        const uint8_t *hello)
{
    uint8_t expected[NET_COOKIE_SIZE];
    const uint8_t *cookie = hello + HELLO_COOKIE;
    if (hello[HELLO_COOKIE_SIZE] != NET_COOKIE_SIZE ||
        (cookie[0] != endpoint->cookieEpoch && cookie[0] != (uint8_t)(endpoint->cookieEpoch - 1u)))
        return false;
    return CookieCompute(endpoint, cookie[0], from, hello, expected) &&
           PlatformConstantTimeEqual(expected, cookie, NET_COOKIE_SIZE);
}

/* CID сервера — псевдослучайная функция запроса клиента. Повторный HELLO
 * даёт тот же CID и находит участника в таблице без перебора. */
static bool ServerCid(const LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                      const uint8_t *hello, uint64_t *outCid)
{
    uint8_t binding[BINDING_SIZE];
    uint8_t mac[NET_HASH_SIZE];
    Binding(from, hello, binding);
    if (!NetHmacSha256(endpoint->cidKey, NET_KEY_SIZE, binding, sizeof(binding), mac))
        return false;
    *outCid = NetLoad64(mac);
    return true;
}

/* ---- Ключи --------------------------------------------------------------------------- */

#define SECRETS_CAPACITY (3u * NET_KEY_SIZE)

/* Собирает секреты подряд: эфемерный обмен, обмен с постоянным ключом
 * сервера, общий секрет приложения. false — точка малого порядка. */
static bool GatherSecrets(const uint8_t *ephemeralScalar, const uint8_t *ephemeralPoint,
                          const uint8_t *staticScalar, const uint8_t *staticPoint,
                          const uint8_t *preshared, uint8_t secrets[SECRETS_CAPACITY],
                          uint32_t *outSize)
{
    uint32_t size = NET_KEY_SIZE;
    if (!NetX25519(ephemeralScalar, ephemeralPoint, secrets))
        return false;
    if (staticScalar != NULL)
    {
        if (!NetX25519(staticScalar, staticPoint, secrets + size))
            return false;
        size += NET_KEY_SIZE;
    }
    if (preshared != NULL)
    {
        memcpy(secrets + size, preshared, NET_KEY_SIZE);
        size += NET_KEY_SIZE;
    }
    *outSize = size;
    return true;
}

static bool DeriveKeys(const uint8_t *hello, uint64_t serverCid, const uint8_t *serverEphemeral,
                       const uint8_t *secrets, uint32_t secretsSize,
                       uint8_t acceptKey[NET_KEY_SIZE], uint8_t clientKey[NET_KEY_SIZE],
                       uint8_t serverKey[NET_KEY_SIZE])
{
    static const char label[] = "laiue.net v1 handshake";
    uint8_t opened[sizeof(label) - 1u + NET_HELLO_FIXED_SIZE + 8u + NET_KEY_SIZE];
    uint8_t transcript[NET_HASH_SIZE];
    uint8_t prk[NET_HASH_SIZE];
    uint8_t *cursor = opened;
    memcpy(cursor, label, sizeof(label) - 1u);
    cursor += sizeof(label) - 1u;
    memcpy(cursor, hello, NET_HELLO_FIXED_SIZE);
    cursor += NET_HELLO_FIXED_SIZE;
    NetStore64(cursor, serverCid);
    memcpy(cursor + 8, serverEphemeral, NET_KEY_SIZE);
    const bool derived =
        PlatformSha256(opened, sizeof(opened), transcript) &&
        NetHkdfExtract(transcript, sizeof(transcript), secrets, secretsSize, prk) &&
        NetHkdfExpand(prk, "laiue.net v1 accept", acceptKey) &&
        NetHkdfExpand(prk, "laiue.net v1 client", clientKey) &&
        NetHkdfExpand(prk, "laiue.net v1 server", serverKey);
    NetWipe(prk, sizeof(prk));
    return derived;
}

/* ---- Клиент -------------------------------------------------------------------------- */

static void WriteHelloFixed(const LaiueNetEndpoint *endpoint, const NetPeer *peer,
                            uint8_t output[NET_HELLO_FIXED_SIZE])
{
    output[0] = NET_PACKET_HELLO;
    NetStore16(output + 1, NET_PROTOCOL_VERSION);
    output[HELLO_FLAGS] = (uint8_t)((peer->pinned ? NET_HELLO_PINNED : 0u) |
                                    (endpoint->hasPreshared ? NET_HELLO_PRESHARED : 0u));
    NetStore64(output + HELLO_PROTOCOL, endpoint->protocolId);
    NetStore64(output + HELLO_CLIENT_CID, peer->localCid);
    memcpy(output + HELLO_EPHEMERAL, peer->ephemeralPublic, NET_KEY_SIZE);
    NetParametersWrite(&endpoint->local, output + HELLO_PARAMETERS);
}

/* Длина запроса: базовый размер датаграммы либо меньший предел клиента. */
static uint32_t HelloBytes(const NetParameters *client)
{
    return client->datagramBytes < NET_DATAGRAM_BASE ? client->datagramBytes : NET_DATAGRAM_BASE;
}

static void SendHello(LaiueNetEndpoint *endpoint, const NetPeer *peer)
{
    /* Запрос дополнен до базового размера: ответ сервера заведомо короче, а
     * дошедший запрос показывает, что путь пропускает такой размер. Больший
     * найдёт поиск уже установленного соединения. */
    uint8_t *packet = endpoint->packetBuffer;
    const uint32_t size = HelloBytes(&endpoint->local);
    memset(packet, 0, size);
    WriteHelloFixed(endpoint, peer, packet);
    packet[HELLO_COOKIE_SIZE] = peer->cookieSize;
    memcpy(packet + HELLO_COOKIE, peer->cookie, peer->cookieSize);
    NetTransmit(endpoint, &peer->address, packet, size);
}

bool NetHandshakeBegin(LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t *serverPublicKey)
{
    if (!NetRandom(endpoint, peer->ephemeralPrivate, NET_KEY_SIZE))
        return false;
    NetX25519Public(peer->ephemeralPrivate, peer->ephemeralPublic);
    if (serverPublicKey != NULL)
    {
        peer->pinned = true;
        memcpy(peer->remoteKey, serverPublicKey, NET_KEY_SIZE);
    }
    return true;
}

/* Ответ сервера принимается только от адреса, куда ушёл запрос, и только
 * пока соединение ждёт ответа. */
static NetPeer *FindConnecting(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                               const uint8_t *bytes, uint32_t size, uint32_t expectedSize)
{
    if (size != expectedSize)
        return NULL;
    NetPeer *peer = NetPeerFindByCid(endpoint, NetLoad64(bytes + 3));
    if (peer == NULL || peer->state != NET_STATE_CONNECTING || !peer->handshakeStarted ||
        !LaiueNetAddressEqual(&peer->address, from))
        return NULL;
    return peer;
}

static void ReceiveRetry(LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t *bytes)
{
    const uint8_t *cookie = bytes + 11;
    const bool repeated = peer->cookieSize != 0u;
    /* Та же метка ещё раз — ответ на повтор запроса: запрос с ней уже в пути. */
    if (repeated && memcmp(peer->cookie, cookie, NET_COOKIE_SIZE) == 0)
        return;
    memcpy(peer->cookie, cookie, NET_COOKIE_SIZE);
    peer->cookieSize = NET_COOKIE_SIZE;
    /* Немедленных повторов немного: дальше запрос уходит по таймеру, и
     * поток поддельных RETRY не превращает клиента в источник трафика. */
    if (peer->retryResends >= NET_RETRY_RESENDS)
        return;
    ++peer->retryResends;
    peer->handshakeInterval = NET_HELLO_INTERVAL_FIRST;
    peer->handshakeResendAt = endpoint->now + peer->handshakeInterval;
    peer->helloSentAt = endpoint->now;
    /* Сервер мог принять и запрос с прежней меткой: тогда его ответ не
     * относится к этой отправке. */
    peer->helloResent = repeated;
    SendHello(endpoint, peer);
}

static bool OpenAccept(const LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t *bytes)
{
    uint8_t hello[NET_HELLO_FIXED_SIZE];
    uint8_t secrets[SECRETS_CAPACITY];
    uint8_t acceptKey[NET_KEY_SIZE];
    uint8_t clientKey[NET_KEY_SIZE];
    uint8_t serverKey[NET_KEY_SIZE];
    uint8_t parameters[NET_PARAMETERS_SIZE];
    uint32_t secretsSize = 0u;
    const uint64_t serverCid = NetLoad64(bytes + 11);
    const uint8_t *serverEphemeral = bytes + 19;
    WriteHelloFixed(endpoint, peer, hello);
    const bool opened =
        GatherSecrets(peer->ephemeralPrivate, serverEphemeral,
                      peer->pinned ? peer->ephemeralPrivate : NULL, peer->remoteKey,
                      endpoint->hasPreshared ? endpoint->preshared : NULL, secrets, &secretsSize) &&
        DeriveKeys(hello, serverCid, serverEphemeral, secrets, secretsSize, acceptKey, clientKey,
                   serverKey) &&
        NetAeadOpen(acceptKey, zeroNonce, bytes, NET_ACCEPT_HEADER_SIZE,
                    bytes + NET_ACCEPT_HEADER_SIZE, NET_PARAMETERS_SIZE,
                    bytes + NET_ACCEPT_HEADER_SIZE + NET_PARAMETERS_SIZE, parameters) &&
        NetParametersRead(parameters, &peer->remote);
    if (opened)
    {
        peer->remoteCid = serverCid;
        memcpy(peer->sendKey, clientKey, NET_KEY_SIZE);
        memcpy(peer->receiveKey, serverKey, NET_KEY_SIZE);
    }
    NetWipe(secrets, sizeof(secrets));
    NetWipe(acceptKey, sizeof(acceptKey));
    NetWipe(clientKey, sizeof(clientKey));
    NetWipe(serverKey, sizeof(serverKey));
    return opened;
}

static void ReceiveAccept(LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t *bytes)
{
    if (!OpenAccept(endpoint, peer, bytes))
    {
        /* Подделка либо не тот ключ. Соединение ждёт дальше: настоящий ответ
         * ещё может прийти, а причина станет известна по тайм-ауту. */
        peer->acceptFailed = true;
        return;
    }
    NetWipe(peer->ephemeralPrivate, sizeof(peer->ephemeralPrivate));
    if (!NetPeerEstablish(endpoint, peer))
    {
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_RESOURCE, 0u, NET_CLOSE_NONE);
        return;
    }
    NetPeerSetState(endpoint, peer, NET_STATE_CONNECTED);
    /* Сервер держит полуоткрытое соединение не дольше рукопожатия: столько
     * же клиент ждёт от него первого пакета. */
    peer->handshakeDeadline = endpoint->now + endpoint->handshakeTimeout;
    if (!NetEventPush(endpoint, peer, LAIUE_NET_EVENT_CONNECTED, 0u, 0u, NULL, 0u))
    {
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_RESOURCE, 0u, NET_CLOSE_RESOURCE);
        return;
    }
    /* Запрос не повторялся, значит, ответ пришёл именно на него: первая
     * оценка пути готова ещё до первого пакета данных. */
    if (!peer->helloResent)
        NetPeerSeedRtt(peer, endpoint->now - peer->helloSentAt, endpoint->now);
    /* Первый пакет подтверждает серверу владение ключами. */
    peer->pingPending = true;
}

/* ---- Сервер -------------------------------------------------------------------------- */

static void SendReject(LaiueNetEndpoint *endpoint, const LaiueNetAddress *to, uint64_t clientCid,
                       uint32_t reason)
{
    uint8_t packet[NET_REJECT_SIZE] = {0u};
    packet[0] = NET_PACKET_REJECT;
    NetStore16(packet + 1, NET_PROTOCOL_VERSION);
    NetStore64(packet + 3, clientCid);
    packet[11] = (uint8_t)reason;
    /* Подробность: версия, которую понимает сервер. */
    NetStore32(packet + 12, NET_PROTOCOL_VERSION);
    NetTransmit(endpoint, to, packet, sizeof(packet));
}

static void SendRetry(LaiueNetEndpoint *endpoint, const LaiueNetAddress *to, const uint8_t *hello)
{
    uint8_t packet[NET_RETRY_SIZE];
    packet[0] = NET_PACKET_RETRY;
    NetStore16(packet + 1, NET_PROTOCOL_VERSION);
    memcpy(packet + 3, hello + HELLO_CLIENT_CID, 8u);
    if (CookieCompute(endpoint, endpoint->cookieEpoch, to, hello, packet + 11))
        NetTransmit(endpoint, to, packet, sizeof(packet));
}

/* Создаёт участника по проверенному запросу и отвечает ACCEPT. */
static void AcceptHello(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                        const uint8_t *hello, const NetParameters *remote, uint64_t serverCid)
{
    uint8_t secrets[SECRETS_CAPACITY];
    uint8_t acceptKey[NET_KEY_SIZE];
    uint8_t parameters[NET_PARAMETERS_SIZE];
    uint32_t secretsSize = 0u;
    LaiueNetStatus status = LAIUE_NET_OK;
    const bool pinned = (hello[HELLO_FLAGS] & NET_HELLO_PINNED) != 0u;
    const uint8_t *clientEphemeral = hello + HELLO_EPHEMERAL;
    NetPeer *peer = NetPeerCreate(endpoint, from, &serverCid, &status);
    if (peer == NULL)
        return;
    peer->remoteCid = NetLoad64(hello + HELLO_CLIENT_CID);
    peer->remote = *remote;
    memcpy(peer->remoteKey, clientEphemeral, NET_KEY_SIZE);
    bool accepted = NetRandom(endpoint, peer->ephemeralPrivate, NET_KEY_SIZE);
    if (accepted)
    {
        NetX25519Public(peer->ephemeralPrivate, peer->ephemeralPublic);
        /* Ключ клиента — серверу на приём, ключ сервера — на отправку. */
        accepted = GatherSecrets(peer->ephemeralPrivate, clientEphemeral,
                                 pinned ? endpoint->identityPrivate : NULL, clientEphemeral,
                                 endpoint->hasPreshared ? endpoint->preshared : NULL, secrets,
                                 &secretsSize) &&
                   DeriveKeys(hello, serverCid, peer->ephemeralPublic, secrets, secretsSize,
                              acceptKey, peer->receiveKey, peer->sendKey);
    }
    if (accepted)
    {
        uint8_t *accept = peer->accept;
        accept[0] = NET_PACKET_ACCEPT;
        NetStore16(accept + 1, NET_PROTOCOL_VERSION);
        NetStore64(accept + 3, peer->remoteCid);
        NetStore64(accept + 11, serverCid);
        memcpy(accept + 19, peer->ephemeralPublic, NET_KEY_SIZE);
        NetParametersWrite(&endpoint->local, parameters);
        NetAeadSeal(acceptKey, zeroNonce, accept, NET_ACCEPT_HEADER_SIZE, parameters,
                    NET_PARAMETERS_SIZE, accept + NET_ACCEPT_HEADER_SIZE,
                    accept + NET_ACCEPT_HEADER_SIZE + NET_PARAMETERS_SIZE);
    }
    NetWipe(secrets, sizeof(secrets));
    NetWipe(acceptKey, sizeof(acceptKey));
    NetWipe(peer->ephemeralPrivate, sizeof(peer->ephemeralPrivate));
    if (!accepted)
    {
        NetPeerRelease(endpoint, peer);
        return;
    }
    peer->handshakeDeadline = endpoint->now + endpoint->handshakeTimeout;
    peer->helloSentAt = endpoint->now;
    NetPeerSetState(endpoint, peer, NET_STATE_HALF_OPEN);
    NetTransmit(endpoint, from, peer->accept, NET_ACCEPT_SIZE);
}

/* Запрос того, чей участник уже есть. true — запрос этим и исчерпан. */
static bool AnswerKnown(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                        const uint8_t *hello, uint64_t serverCid)
{
    NetPeer *known = NetPeerFindByCid(endpoint, serverCid);
    if (known == NULL)
        return false;
    /* Повтор запроса: ответ потерялся в пути. Тот же ACCEPT уходит снова;
     * уже установленному соединению запрос ни о чём не говорит. */
    if (known->state == NET_STATE_HALF_OPEN &&
        known->remoteCid == NetLoad64(hello + HELLO_CLIENT_CID) &&
        memcmp(known->remoteKey, hello + HELLO_EPHEMERAL, NET_KEY_SIZE) == 0)
    {
        known->helloResent = true;
        NetTransmit(endpoint, from, known->accept, NET_ACCEPT_SIZE);
    }
    return true;
}

/* Тратит единицу бюджета и создаёт участника, освобождая при нужде место. */
static void Admit(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from, const uint8_t *hello,
                  const NetParameters *remote, uint64_t serverCid)
{
    --endpoint->acceptBudget;
    if (NetPeerCount(endpoint) >= endpoint->maxPeers && !NetPeerEvictHalfOpen(endpoint))
    {
        SendReject(endpoint, from, NetLoad64(hello + HELLO_CLIENT_CID), LAIUE_NET_REJECT_FULL);
        return;
    }
    AcceptHello(endpoint, from, hello, remote, serverCid);
}

/* Откладывает проверенный запрос. Полная очередь — предел и для потока
 * запросов: лишние теряются, их клиенты повторят. */
static void DeferHello(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                       const uint8_t *hello, const NetParameters *remote, uint64_t serverCid)
{
    if (endpoint->deferredCount == NET_DEFERRED_HELLOS)
        return;
    if (endpoint->deferred == NULL)
    {
        endpoint->deferred =
            PlatformAllocate((size_t)NET_DEFERRED_HELLOS * sizeof(NetDeferredHello), false);
        if (endpoint->deferred == NULL)
            return;
    }
    NetDeferredHello *entry =
        &endpoint->deferred[(endpoint->deferredHead + endpoint->deferredCount) &
                            (NET_DEFERRED_HELLOS - 1u)];
    ++endpoint->deferredCount;
    entry->from = *from;
    entry->remote = *remote;
    entry->serverCid = serverCid;
    entry->receivedAt = endpoint->now;
    memcpy(entry->hello, hello, NET_HELLO_FIXED_SIZE);
}

static void ReceiveHello(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                         const uint8_t *hello, uint32_t size)
{
    NetParameters remote;
    uint64_t serverCid = 0u;
    /* Запрос короче наименьшей датаграммы не получает ответа вовсе: иначе
     * ответ мог бы оказаться длиннее запроса. */
    if ((endpoint->flags & LAIUE_NET_ENDPOINT_ACCEPT) == 0u || size < NET_DATAGRAM_MINIMUM)
    {
        ++endpoint->stats.datagramsDropped;
        return;
    }
    const uint64_t clientCid = NetLoad64(hello + HELLO_CLIENT_CID);
    const uint8_t flags = hello[HELLO_FLAGS];
    uint32_t reject = 0u;
    if (NetLoad16(hello + 1) != NET_PROTOCOL_VERSION)
        reject = LAIUE_NET_REJECT_VERSION;
    else if (NetLoad64(hello + HELLO_PROTOCOL) != endpoint->protocolId)
        reject = LAIUE_NET_REJECT_PROTOCOL;
    else if ((flags & NET_HELLO_PINNED) != 0u && !endpoint->hasIdentity)
        reject = LAIUE_NET_REJECT_IDENTITY;
    else if (((flags & NET_HELLO_PRESHARED) != 0u) != endpoint->hasPreshared)
        reject = LAIUE_NET_REJECT_PRESHARED_KEY;
    if (reject != 0u)
    {
        SendReject(endpoint, from, clientCid, reject);
        return;
    }
    /* Клиент дополняет запрос до базового размера датаграммы. Запрос короче
     * не доказал, что путь такой размер пропускает. */
    if ((flags & ~(NET_HELLO_PINNED | NET_HELLO_PRESHARED)) != 0u ||
        !NetParametersRead(hello + HELLO_PARAMETERS, &remote) || size < HelloBytes(&remote) ||
        !ServerKeysReady(endpoint))
    {
        ++endpoint->stats.datagramsDropped;
        return;
    }
    if (!CookieValid(endpoint, from, hello))
    {
        SendRetry(endpoint, from, hello);
        return;
    }
    if (!ServerCid(endpoint, from, hello, &serverCid) ||
        AnswerKnown(endpoint, from, hello, serverCid))
        return;
    /* Обмен ключами дорог: за одно обновление принимается ограниченное число
     * новых соединений. Остальные проверенные запросы ждут следующих
     * обновлений, а не повторов клиентов: те, начав разом, и повторяли бы
     * разом, снова упираясь в тот же предел. */
    if (endpoint->acceptBudget == 0u)
    {
        DeferHello(endpoint, from, hello, &remote, serverCid);
        return;
    }
    Admit(endpoint, from, hello, &remote, serverCid);
}

void NetHandshakeAdmitDeferred(LaiueNetEndpoint *endpoint)
{
    while (endpoint->deferredCount != 0u && endpoint->acceptBudget != 0u)
    {
        const NetDeferredHello *entry = &endpoint->deferred[endpoint->deferredHead];
        endpoint->deferredHead = (endpoint->deferredHead + 1u) & (NET_DEFERRED_HELLOS - 1u);
        --endpoint->deferredCount;
        /* Ждавший дольше повтора клиент уже прислал запрос заново, а тот, что
         * сдался, ответа не ждёт: обмен ключами ради него не нужен. */
        if (endpoint->now - entry->receivedAt > NET_HELLO_INTERVAL_LIMIT ||
            AnswerKnown(endpoint, &entry->from, entry->hello, entry->serverCid))
            continue;
        Admit(endpoint, &entry->from, entry->hello, &entry->remote, entry->serverCid);
    }
}

/* ---- Общее --------------------------------------------------------------------------- */

void NetHandshakeReceive(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                         const uint8_t *bytes, uint32_t size)
{
    NetPeer *peer = NULL;
    switch (bytes[0])
    {
    case NET_PACKET_HELLO:
        ReceiveHello(endpoint, from, bytes, size);
        return;
    case NET_PACKET_RETRY:
        peer = FindConnecting(endpoint, from, bytes, size, NET_RETRY_SIZE);
        if (peer != NULL && NetLoad16(bytes + 1) == NET_PROTOCOL_VERSION)
        {
            ReceiveRetry(endpoint, peer, bytes);
            return;
        }
        break;
    case NET_PACKET_ACCEPT:
        peer = FindConnecting(endpoint, from, bytes, size, NET_ACCEPT_SIZE);
        if (peer != NULL && NetLoad16(bytes + 1) == NET_PROTOCOL_VERSION)
        {
            ReceiveAccept(endpoint, peer, bytes);
            return;
        }
        break;
    case NET_PACKET_REJECT:
        peer = FindConnecting(endpoint, from, bytes, size, NET_REJECT_SIZE);
        if (peer != NULL)
        {
            NetPeerClose(endpoint, peer, LAIUE_NET_REASON_REJECTED, bytes[11], NET_CLOSE_NONE);
            return;
        }
        break;
    default:
        break;
    }
    ++endpoint->stats.datagramsDropped;
}

void NetHandshakeUpdate(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    const uint64_t now = endpoint->now;
    if (peer->state == NET_STATE_HALF_OPEN)
    {
        /* Клиент так и не прислал пакета: приложение о нём не знало. */
        if (now >= peer->handshakeDeadline)
            NetPeerClose(endpoint, peer, LAIUE_NET_REASON_TIMEOUT, 0u, NET_CLOSE_NONE);
        return;
    }
    if (!peer->handshakeStarted)
    {
        /* Отсчёт идёт от первого обновления после LaiueNetConnect: до него
         * конечная точка не знает текущего времени. */
        peer->handshakeStarted = true;
        peer->handshakeDeadline = now + endpoint->handshakeTimeout;
        peer->handshakeInterval = NET_HELLO_INTERVAL_FIRST;
        peer->handshakeResendAt = now + peer->handshakeInterval;
        peer->helloSentAt = now;
        SendHello(endpoint, peer);
        return;
    }
    if (now >= peer->handshakeDeadline)
    {
        NetPeerClose(endpoint, peer,
                     peer->acceptFailed ? LAIUE_NET_REASON_AUTHENTICATION
                                        : LAIUE_NET_REASON_TIMEOUT,
                     0u, NET_CLOSE_NONE);
        return;
    }
    if (now < peer->handshakeResendAt)
        return;
    peer->handshakeInterval *= 2u;
    if (peer->handshakeInterval > NET_HELLO_INTERVAL_LIMIT)
        peer->handshakeInterval = NET_HELLO_INTERVAL_LIMIT;
    peer->handshakeResendAt = now + peer->handshakeInterval;
    peer->helloResent = true;
    SendHello(endpoint, peer);
}
