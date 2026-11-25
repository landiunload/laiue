/* Надёжный канал laiue.net без пакетов и шифрования: отправитель помнит,
 * какие отрезки потока подтверждены и какие потеряны, получатель собирает
 * поток из кусков. Как бы ни перемешались потери, повторы и подтверждения,
 * каждое сообщение доходит целым и по порядку, а каждый отправленный байт в
 * конце концов подтверждается. */

#include "net/net_internal.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define WINDOW_BYTES 65536u
#define FRAME_LIMIT 1200u
#define FLIGHT_CAPACITY 256u
#define NOTICE_CAPACITY 1024u
#define MESSAGE_LIMIT 20000u
#define MESSAGE_COUNT 24u

#define NOTICE_ACKED 0u
#define NOTICE_LOST 1u
#define NOTICE_SPURIOUS 2u

typedef struct Frame
{
    NetSentRange range;
    uint8_t bytes[FRAME_LIMIT];
} Frame;

/* Что отправитель узнаёт о кадре. */
typedef struct Notice
{
    NetSentRange range;
    uint32_t kind;
} Notice;

static LaiueNetEndpoint senderEndpoint;
static LaiueNetEndpoint receiverEndpoint;
static NetPeer sender;
static NetPeer receiver;
static NetChannel senderChannel;
static NetChannel receiverChannel;
static Frame flight[FLIGHT_CAPACITY];
static uint32_t flightCount;
static Notice notices[NOTICE_CAPACITY];
static uint32_t noticeCount;
static uint32_t messageSizes[MESSAGE_COUNT];
static uint32_t messageCount;
static uint32_t delivered;
static uint8_t message[MESSAGE_LIMIT];
static uint8_t packet[FRAME_LIMIT];
static uint64_t randomState;

static void Expect(bool condition, const char *text)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Net channel failure: ");
    LaiueTestRuntimeWrite(text);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint64_t NextRandom(void)
{
    randomState ^= randomState << 13u;
    randomState ^= randomState >> 7u;
    randomState ^= randomState << 17u;
    return randomState;
}

static uint8_t Pattern(uint32_t index, uint32_t position)
{
    return (uint8_t)(index * 37u + position * 11u + (position >> 7u));
}

/* Канал отдаёт собранные сообщения конечной точке; здесь они сверяются. */
bool NetEventPush(LaiueNetEndpoint *endpoint, const NetPeer *peer, LaiueNetEventType type,
                  uint32_t channel, LaiueNetDelivery delivery, const uint8_t *data, uint32_t size)
{
    Expect(endpoint == &receiverEndpoint && peer == &receiver && channel == 0u,
           "a message goes to the receiving side");
    Expect(type == LAIUE_NET_EVENT_MESSAGE && delivery == LAIUE_NET_RELIABLE,
           "the stream carries reliable messages");
    Expect(delivered < messageCount && size == messageSizes[delivered],
           "messages arrive whole and in order");
    for (uint32_t position = 0u; position < size; ++position)
    {
        if (data[position] != Pattern(delivered, position))
            Expect(false, "message bytes are intact");
    }
    ++delivered;
    return true;
}

/* Ненадёжные сообщения этим тестом не проверяются. */
uint32_t NetPeerMaxUnreliable(const NetPeer *peer)
{
    (void)peer;
    return FRAME_LIMIT;
}

static void Begin(uint64_t seed)
{
    memset(&senderEndpoint, 0, sizeof(senderEndpoint));
    memset(&receiverEndpoint, 0, sizeof(receiverEndpoint));
    memset(&sender, 0, sizeof(sender));
    memset(&receiver, 0, sizeof(receiver));
    memset(&senderChannel, 0, sizeof(senderChannel));
    memset(&receiverChannel, 0, sizeof(receiverChannel));
    senderEndpoint.sendBufferBytes = UINT32_C(1) << 20u;
    receiverEndpoint.local.windowBytes = WINDOW_BYTES;
    receiverEndpoint.local.maxMessageBytes = MESSAGE_LIMIT;
    sender.channels = &senderChannel;
    sender.channelCount = 1u;
    sender.remote.windowBytes = WINDOW_BYTES;
    sender.remote.maxMessageBytes = MESSAGE_LIMIT;
    receiver.channels = &receiverChannel;
    receiver.channelCount = 1u;
    flightCount = 0u;
    noticeCount = 0u;
    messageCount = 0u;
    delivered = 0u;
    randomState = seed * UINT64_C(0x9E3779B97F4A7C15) + 1u;
}

static void End(void)
{
    PlatformFree(senderChannel.sendData);
    PlatformFree(receiverChannel.receiveData);
    PlatformFree(receiverChannel.assembly);
}

static void Queue(uint32_t size)
{
    Expect(messageCount < MESSAGE_COUNT, "the message table has room");
    for (uint32_t position = 0u; position < size; ++position)
        message[position] = Pattern(messageCount, position);
    Expect(NetChannelQueueReliable(&senderEndpoint, &sender, 0u, message, size) == LAIUE_NET_OK,
           "a message is queued");
    messageSizes[messageCount] = size;
    ++messageCount;
}

/* Очередной кадр потока в сеть; false — отправлять нечего. */
static bool Send(uint32_t space)
{
    Expect(flightCount < FLIGHT_CAPACITY, "the network has room");
    Frame *frame = &flight[flightCount];
    const uint32_t written = NetChannelWriteStream(&sender, 0u, packet, space, &frame->range);
    if (written == 0u)
        return false;
    memcpy(frame->bytes, packet + written - frame->range.length, frame->range.length);
    ++flightCount;
    return true;
}

static uint32_t Deliver(const Frame *frame)
{
    const uint32_t status = NetChannelReceiveStream(
        &receiverEndpoint, &receiver, 0u, frame->range.offset, frame->bytes, frame->range.length);
    Expect(status == NET_FRAMES_OK || status == NET_FRAMES_DEFER,
           "an honest sender never violates the stream");
    return status;
}

static bool Finished(void)
{
    return delivered == messageCount && sender.queuedReliable == 0u &&
           senderChannel.sendAcked == senderChannel.sendEnd;
}

/* Подтверждений вразбивку больше, чем помнит отправитель. Забытое не
 * подтвердит уже никто, поэтому оно уходит ещё раз, и поток не встаёт перед
 * ним навсегда. */
static void TestOverflowingAcknowledgements(void)
{
    Begin(1u);
    Queue(MESSAGE_LIMIT);
    while (Send(200u))
    {
    }
    Expect(flightCount > 2u * NET_ACKED_RANGES + 8u, "the message spans many frames");
    /* Получатель принял всё по порядку, а подтверждения нечётных кадров
     * пришли раньше чётных. */
    for (uint32_t index = 0u; index < flightCount; ++index)
        Expect(Deliver(&flight[index]) == NET_FRAMES_OK, "frames in order are taken");
    Expect(delivered == messageCount, "the receiver has the whole message");
    for (uint32_t index = 1u; index < flightCount; index += 2u)
        NetChannelOnAcked(&sender, &flight[index].range);
    for (uint32_t index = 0u; index < flightCount; index += 2u)
        NetChannelOnAcked(&sender, &flight[index].range);
    uint32_t resent = 0u;
    while (NetPeerHasStreamData(&sender))
    {
        flightCount = 0u;
        Expect(Send(200u), "pending data is sent");
        Expect(Deliver(&flight[0]) == NET_FRAMES_OK, "a repeated frame is taken");
        NetChannelOnAcked(&sender, &flight[0].range);
        ++resent;
    }
    Expect(resent != 0u, "forgotten acknowledgements are resent");
    Expect(Finished(), "every byte is acknowledged in the end");
    End();
}

static void Notify(const NetSentRange *range, uint32_t kind)
{
    Expect(noticeCount < NOTICE_CAPACITY, "the return path has room");
    notices[noticeCount].range = *range;
    notices[noticeCount].kind = kind;
    ++noticeCount;
}

/* Случайная сеть. Кадр теряется либо приходит; получатель может отложить
 * кадр, которому нет места. Отправитель узнаёт о судьбе кадров в случайном
 * порядке: подтверждение, потеря, потеря подтверждения либо мнимая потеря,
 * за которой всё же приходит подтверждение. */
static void RunRandomNetwork(uint64_t seed)
{
    Begin(seed);
    const uint64_t lossPercent = seed % 3u == 0u ? 20u : seed % 3u;
    while (messageCount < MESSAGE_COUNT)
        Queue((uint32_t)(NextRandom() % 4u == 0u ? 0u : NextRandom() % MESSAGE_LIMIT));
    for (uint32_t step = 0u; step < 4000000u; ++step)
    {
        const uint64_t choice = NextRandom() % 100u;
        if (choice < 40u && flightCount < FLIGHT_CAPACITY && NetPeerHasStreamData(&sender))
        {
            (void)Send(16u + (uint32_t)(NextRandom() % (FRAME_LIMIT - 15u)));
        }
        else if (choice < 70u && flightCount != 0u)
        {
            const uint32_t index = (uint32_t)(NextRandom() % flightCount);
            const Frame *frame = &flight[index];
            const uint64_t fate = NextRandom() % 100u;
            if (fate < lossPercent || Deliver(frame) == NET_FRAMES_DEFER)
                Notify(&frame->range, NOTICE_LOST);
            else if (fate >= 95u)
                Notify(&frame->range, NOTICE_LOST);
            else if (fate >= 90u)
                Notify(&frame->range, NOTICE_SPURIOUS);
            else
                Notify(&frame->range, NOTICE_ACKED);
            flight[index] = flight[flightCount - 1u];
            --flightCount;
        }
        else if (noticeCount != 0u)
        {
            const uint32_t index = (uint32_t)(NextRandom() % noticeCount);
            Notice *notice = &notices[index];
            if (notice->kind == NOTICE_ACKED)
            {
                NetChannelOnAcked(&sender, &notice->range);
            }
            else
            {
                NetChannelOnLost(&sender, &notice->range);
                /* Мнимая потеря: подтверждение придёт позже. */
                if (notice->kind == NOTICE_SPURIOUS)
                {
                    notice->kind = NOTICE_ACKED;
                    continue;
                }
            }
            notices[index] = notices[noticeCount - 1u];
            --noticeCount;
        }
        else if (flightCount == 0u && !NetPeerHasStreamData(&sender))
        {
            break;
        }
    }
    Expect(delivered == messageCount, "every message arrives");
    Expect(Finished(), "the transfer never stalls");
    End();
}

static void TestRandomNetworks(void)
{
    for (uint64_t seed = 1u; seed <= 300u; ++seed)
        RunRandomNetwork(seed);
}

LAIUE_TEST_ENTRY(NetChannelTestEntryPoint)
{
    TestOverflowingAcknowledgements();
    TestRandomNetworks();
    LaiueTestRuntimeWrite("net channel: passed\n");
    LAIUE_TEST_SUCCESS();
}
