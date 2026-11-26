/* Поиск размера датаграммы пути laiue.net отдельно от транспорта: пробы
 * проходят по пути заданной ширины или теряются, а потери обычных пакетов и
 * пробный таймер выдают чёрные дыры. Пакеты и пробы нумеруются вместе, как в
 * соединении. */

#include "net/net_internal.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define BASE 1200u
#define LIMIT 1452u
#define RTT (UINT64_C(50) * NET_MILLISECOND)
#define MINUTE (UINT64_C(60) * NET_SECOND)

static NetPmtu pmtu;
static uint64_t now;
static uint64_t packet;
static uint32_t probesLost;
/* Размеры проб, отправленных Probe, по порядку. */
static uint32_t probed[64];
static uint32_t probedCount;

static void WriteNumber(uint64_t value)
{
    char text[21];
    uint32_t length = 0u;
    do
    {
        text[length] = (char)('0' + value % 10u);
        value /= 10u;
        ++length;
    } while (value != 0u);
    char ordered[21];
    for (uint32_t index = 0u; index < length; ++index)
        ordered[index] = text[length - 1u - index];
    ordered[length] = '\0';
    LaiueTestRuntimeWrite(ordered);
}

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Net path size failure: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ExpectBetween(uint64_t value, uint64_t low, uint64_t high, const char *message)
{
    if (value >= low && value <= high)
        return;
    LaiueTestRuntimeWrite("Net path size failure: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite(": got ");
    WriteNumber(value);
    LaiueTestRuntimeWrite(", expected ");
    WriteNumber(low);
    LaiueTestRuntimeWrite("..");
    WriteNumber(high);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void Begin(uint32_t base, uint32_t limit)
{
    now = NET_SECOND;
    packet = 0u;
    probesLost = 0u;
    probedCount = 0u;
    NetPmtuReset(&pmtu, base, limit, now);
}

/* Шлёт пробы, пока их пора слать, по пути, пропускающему датаграммы не длиннее
 * path байт. */
static void Probe(uint32_t path)
{
    for (uint32_t step = 0u; step < 1000u; ++step)
    {
        const uint32_t size = NetPmtuProbeSize(&pmtu, now);
        if (size == 0u)
            return;
        Expect(size > pmtu.current && size <= pmtu.limit,
               "a probe is larger than the confirmed size and within the limit");
        NetPmtuProbeSent(&pmtu, size);
        Expect(NetPmtuProbeSize(&pmtu, now) == 0u, "one probe at a time");
        if (probedCount < sizeof(probed) / sizeof(probed[0]))
            probed[probedCount++] = size;
        const uint64_t number = packet++;
        const uint64_t sent = now;
        now += RTT;
        if (size <= path)
        {
            NetPmtuProbeAcked(&pmtu, number, size, sent, now);
        }
        else
        {
            NetPmtuProbeLost(&pmtu, size, sent, now);
            ++probesLost;
        }
    }
    Expect(false, "the search ends");
}

/* Теряет count проб размера size, отправленных сейчас. */
static void LoseProbes(uint32_t size, uint32_t count)
{
    for (uint32_t loss = 0u; loss < count; ++loss)
    {
        NetPmtuProbeSent(&pmtu, size);
        ++packet;
        NetPmtuProbeLost(&pmtu, size, now, now);
    }
}

/* Найденный размер — не шире пути и не уже его больше чем на 15 байт. */
static void ExpectFound(uint32_t path, const char *message)
{
    const uint32_t width = path < LIMIT ? path : LIMIT;
    ExpectBetween(pmtu.current, width - 15u, width, message);
}

static void TestEveryPath(void)
{
    for (uint32_t path = BASE; path <= LIMIT + 48u; ++path)
    {
        Begin(BASE, LIMIT);
        Probe(path);
        ExpectFound(path, "the search finds the path within 16 bytes");
        /* Наибольший размер пробуется первым, затем промежуток делится
         * пополам: не больше пяти закрытых размеров по три пробы. */
        ExpectBetween(probesLost, path >= LIMIT ? 0u : 3u, path >= LIMIT ? 0u : 15u, "lost probes");
        Expect(probesLost % 3u == 0u, "every closed size costs three probes");
        Expect(NetPmtuProbeSize(&pmtu, now + 9u * MINUTE) == 0u,
               "a finished search waits ten minutes");
    }
}

/* Путь в 1350 байт: предел, затем середины промежутков. */
static void TestProbeSequence(void)
{
    static const uint32_t expected[] = {1452u, 1452u, 1452u, 1326u, 1389u, 1389u,
                                        1389u, 1357u, 1357u, 1357u, 1341u};
    Begin(BASE, LIMIT);
    Probe(1350u);
    Expect(probedCount == sizeof(expected) / sizeof(expected[0]), "the search takes eleven probes");
    for (uint32_t index = 0u; index < probedCount; ++index)
        Expect(probed[index] == expected[index], "each probe halves the range");
    Expect(pmtu.current == 1341u, "and the last one is within 16 bytes of the path");
}

static void TestRaise(void)
{
    Begin(BASE, LIMIT);
    Probe(1300u);
    const uint32_t found = pmtu.current;
    now += 10u * MINUTE - NET_MILLISECOND;
    Expect(NetPmtuProbeSize(&pmtu, now) == 0u, "the search waits ten minutes");
    now += NET_MILLISECOND;
    Expect(NetPmtuProbeSize(&pmtu, now) == LIMIT, "then it probes the limit again");
    /* Путь не изменился: поиск не теряет найденного. */
    Probe(1300u);
    Expect(pmtu.current >= found, "an unchanged path keeps its size");
    ExpectFound(1300u, "and the search still fits the path");
    /* Путь расширился. */
    now += 10u * MINUTE;
    Probe(LIMIT);
    Expect(pmtu.current == LIMIT, "a wider path is found");
    now += 1000u * MINUTE;
    Expect(NetPmtuProbeSize(&pmtu, now) == 0u, "at the limit there is nothing left to search");
}

static void TestNarrowLimits(void)
{
    Begin(BASE, BASE);
    Expect(pmtu.current == BASE && NetPmtuProbeSize(&pmtu, now) == 0u,
           "no search without room above the base");
    Begin(BASE, 576u);
    Expect(pmtu.current == 576u && NetPmtuProbeSize(&pmtu, now) == 0u,
           "a limit below the base is the size");
    Begin(BASE, BASE + 10u);
    Probe(BASE + 5u);
    Expect(pmtu.current == BASE && probesLost == 3u,
           "a range narrower than 16 bytes ends after its top fails");
    Begin(BASE, BASE + 17u);
    Probe(BASE + 16u);
    Expect(pmtu.current == BASE + 8u, "a range of exactly 16 bytes is split once more");
}

static void TestLateAcknowledgement(void)
{
    /* Две потери подряд размер ещё не закрывают. */
    Begin(BASE, LIMIT);
    LoseProbes(LIMIT, 2u);
    Expect(NetPmtuProbeSize(&pmtu, now) == LIMIT, "two lost probes retry the same size");
    NetPmtuProbeSent(&pmtu, LIMIT);
    Expect(NetPmtuProbeAcked(&pmtu, packet++, LIMIT, now, now) && pmtu.current == LIMIT,
           "and the third one may still pass");

    /* Проба, сочтённая потерянной, всё же подтверждена. */
    Begin(BASE, LIMIT);
    LoseProbes(LIMIT, 3u);
    Expect(pmtu.ceiling == LIMIT - 1u, "three lost probes close their size");
    Expect(NetPmtuProbeAcked(&pmtu, packet++, LIMIT, now, now) && pmtu.current == LIMIT,
           "a late acknowledgement still proves it");
    Expect(!NetPmtuProbeAcked(&pmtu, packet++, 1300u, now, now) && pmtu.current == LIMIT,
           "a smaller acknowledged probe changes nothing");
    Expect(!NetPmtuProbeAcked(&pmtu, packet++, LIMIT, now, now),
           "nor does one of the confirmed size");
}

/* Потерянные пробы считаются для одного размера одного поиска. */
static void TestFailureCount(void)
{
    Begin(BASE, LIMIT);
    LoseProbes(LIMIT, 2u);
    now += RTT;
    NetPmtuReset(&pmtu, BASE, LIMIT, now);
    LoseProbes(LIMIT, 1u);
    Expect(pmtu.ceiling == LIMIT && pmtu.failures == 1u, "a new search counts afresh");

    Begin(BASE, LIMIT);
    LoseProbes(LIMIT, 3u);
    LoseProbes(1326u, 2u);
    NetPmtuProbeSent(&pmtu, 1326u);
    Expect(NetPmtuProbeAcked(&pmtu, packet++, 1326u, now, now) && pmtu.next == 1389u,
           "the third probe of a size may pass");
    LoseProbes(1389u, 1u);
    Expect(pmtu.ceiling == LIMIT - 1u && pmtu.failures == 1u, "and the next size counts afresh");

    /* Проба уходит повторно, а подтверждение приходит на первую. */
    Begin(BASE, LIMIT);
    const uint64_t firstNumber = packet++;
    const uint64_t firstSent = now;
    NetPmtuProbeSent(&pmtu, LIMIT);
    now += RTT;
    NetPmtuProbeLost(&pmtu, LIMIT, firstSent, now);
    const uint64_t retrySent = now;
    NetPmtuProbeSent(&pmtu, LIMIT);
    ++packet;
    now += RTT;
    Expect(NetPmtuProbeAcked(&pmtu, firstNumber, LIMIT, firstSent, now) && pmtu.current == LIMIT,
           "a late acknowledgement of the first probe proves the size");
    NetPmtuProbeLost(&pmtu, LIMIT, retrySent, now);
    Expect(pmtu.failures == 0u, "and the lost retry counts nothing");

    /* Поиск ушёл вниз, а потом подтвердился предел: потеря пробы ниже него
     * ничего не значит. */
    Begin(BASE, LIMIT);
    uint64_t lastNumber = 0u;
    uint64_t lastSent = 0u;
    for (uint32_t loss = 0u; loss < 3u; ++loss)
    {
        lastNumber = packet++;
        lastSent = now;
        NetPmtuProbeSent(&pmtu, LIMIT);
        now += RTT;
        NetPmtuProbeLost(&pmtu, LIMIT, lastSent, now);
    }
    Expect(pmtu.next == 1326u, "three losses move the search down");
    const uint64_t middleSent = now;
    NetPmtuProbeSent(&pmtu, 1326u);
    ++packet;
    now += RTT;
    Expect(NetPmtuProbeAcked(&pmtu, lastNumber, LIMIT, lastSent, now) && pmtu.current == LIMIT,
           "a late acknowledgement of the limit ends the search");
    NetPmtuProbeLost(&pmtu, 1326u, middleSent, now);
    Expect(pmtu.failures == 0u && NetPmtuProbeSize(&pmtu, now) == 0u,
           "and the loss of a probe below it counts nothing");
}

static void TestBlackHoleLosses(void)
{
    Begin(BASE, LIMIT);
    Probe(LIMIT);
    /* Пачка потерь, за которой дошёл пакет полного размера, — перегрузка,
     * сколько бы пакетов в ней ни было. */
    const uint64_t burst = packet;
    packet += 40u;
    NetPmtuAcked(&pmtu, packet++, LIMIT);
    for (uint64_t lost = burst; lost < burst + 40u; ++lost)
        Expect(!NetPmtuLost(&pmtu, lost, LIMIT, now),
               "losses before a delivered full-size packet are congestion");
    /* Без такого пакета большие потери подозрительны, но семи мало. */
    for (uint32_t loss = 0u; loss < 7u; ++loss)
        Expect(!NetPmtuLost(&pmtu, packet++, LIMIT, now), "seven suspects are not a black hole");
    NetPmtuAcked(&pmtu, packet++, LIMIT);
    for (uint32_t loss = 0u; loss < 7u; ++loss)
        Expect(!NetPmtuLost(&pmtu, packet++, LIMIT, now),
               "a delivered full-size packet clears the suspicion");
    /* Дошедшие пакеты меньше подозрительных подозрения не снимают, а малые
     * потери не подозрительны. */
    NetPmtuAcked(&pmtu, packet++, BASE);
    NetPmtuAcked(&pmtu, packet++, LIMIT - 1u);
    Expect(!NetPmtuLost(&pmtu, packet++, BASE, now), "small losses do not count");
    Expect(NetPmtuLost(&pmtu, packet++, LIMIT, now) && pmtu.current == BASE,
           "the eighth suspect falls to the base size");
    Expect(NetPmtuProbeSize(&pmtu, now + NET_SECOND - 1u) == 0u, "the search waits a second");
    now += NET_SECOND;
    Expect(NetPmtuProbeSize(&pmtu, now) == LIMIT, "then it retries the failed size");
    Probe(1350u);
    ExpectFound(1350u, "and finds the narrower path");
    const uint32_t found = pmtu.current;
    for (uint32_t loss = 0u; loss < 20u; ++loss)
        NetPmtuLost(&pmtu, packet++, BASE, now);
    Expect(pmtu.current == found, "losses at the base size are not a black hole");
    for (uint32_t loss = 0u; loss < 20u; ++loss)
        NetPmtuLost(&pmtu, packet++, LIMIT, now);
    Expect(pmtu.current == found, "nor are packets of the old size, sent before it");
    /* Подозрение снимает и меньший пакет, если он не меньше подозрительных. */
    for (uint32_t loss = 0u; loss < 7u; ++loss)
        NetPmtuLost(&pmtu, packet++, 1300u, now);
    NetPmtuAcked(&pmtu, packet++, 1299u);
    Expect(pmtu.suspects == 7u, "a smaller delivered packet keeps the suspects");
    NetPmtuAcked(&pmtu, packet++, 1300u);
    Expect(pmtu.suspects == 0u, "one as large as them clears them");
    for (uint32_t loss = 0u; loss < 7u; ++loss)
        NetPmtuLost(&pmtu, packet++, found, now);
    Expect(NetPmtuLost(&pmtu, packet++, BASE + 1u, now) && pmtu.current == BASE,
           "packets above the base and up to the new size count");
}

static void TestSuspicion(void)
{
    /* Меньшие пакеты, дошедшие после больших, их потерь не объясняют. */
    Begin(BASE, LIMIT);
    Probe(LIMIT);
    const uint64_t large = packet;
    packet += 8u;
    NetPmtuAcked(&pmtu, packet++, BASE);
    NetPmtuAcked(&pmtu, packet++, 1300u);
    for (uint64_t lost = large; lost < large + 7u; ++lost)
        NetPmtuLost(&pmtu, lost, LIMIT, now);
    Expect(NetPmtuLost(&pmtu, large + 7u, LIMIT, now) && pmtu.current == BASE,
           "smaller packets delivered after large losses do not explain them");

    /* Снятое подозрение забывает свой размер. */
    Begin(BASE, LIMIT);
    Probe(LIMIT);
    for (uint32_t loss = 0u; loss < 7u; ++loss)
        NetPmtuLost(&pmtu, packet++, LIMIT, now);
    NetPmtuAcked(&pmtu, packet++, LIMIT);
    for (uint32_t loss = 0u; loss < 7u; ++loss)
        NetPmtuLost(&pmtu, packet++, 1300u, now);
    NetPmtuAcked(&pmtu, packet++, 1300u);
    Expect(pmtu.suspects == 0u, "new suspects are cleared by their own size");

    /* Новый путь не помнит ни номеров, ни подозрений прежнего. */
    Begin(BASE, LIMIT);
    Probe(LIMIT);
    NetPmtuAcked(&pmtu, 1000u, LIMIT);
    for (uint32_t loss = 0u; loss < 7u; ++loss)
        NetPmtuLost(&pmtu, 1001u + loss, LIMIT, now);
    Begin(BASE, LIMIT);
    Probe(1300u);
    const uint32_t found = pmtu.current;
    for (uint32_t loss = 0u; loss < 7u; ++loss)
        Expect(!NetPmtuLost(&pmtu, packet++, found, now), "old suspects do not carry over");
    Expect(NetPmtuLost(&pmtu, packet++, found, now) && pmtu.current == BASE,
           "nor do old numbers explain new losses");

    /* Подтверждения приходят и не по порядку: более раннее доказательства
     * не отодвигает. */
    Begin(BASE, LIMIT);
    Probe(LIMIT);
    const uint64_t first = packet;
    packet += 10u;
    NetPmtuAcked(&pmtu, packet++, LIMIT);
    NetPmtuAcked(&pmtu, first, LIMIT);
    for (uint64_t lost = first + 1u; lost < first + 10u; ++lost)
        Expect(!NetPmtuLost(&pmtu, lost, LIMIT, now),
               "an earlier packet acknowledged later does not move the proof back");
    Expect(pmtu.suspects == 0u, "and the losses before the proof stay explained");
}

/* Подтверждённая проба объясняет потери пакетов, ушедших до неё. */
static void TestProvenByProbe(void)
{
    Begin(BASE, LIMIT);
    Probe(1300u);
    const uint32_t found = pmtu.current;
    const uint64_t early = packet;
    packet += 20u;
    now += 10u * MINUTE;
    Probe(LIMIT);
    Expect(pmtu.current == LIMIT, "the path widened");
    for (uint64_t lost = early; lost < early + 20u; ++lost)
        Expect(!NetPmtuLost(&pmtu, lost, found, now),
               "losses before the delivered probe are congestion");
    Expect(pmtu.suspects == 0u, "and leave no suspicion");
}

static void TestBlackHoleTimeouts(void)
{
    Begin(BASE, LIMIT);
    Probe(LIMIT);
    Expect(!NetPmtuTimeout(&pmtu, 1u, LIMIT, now), "one probe timeout is not a black hole");
    Expect(!NetPmtuTimeout(&pmtu, 2u, BASE, now) && pmtu.current == LIMIT,
           "nor are two while only small packets are in flight");
    Expect(NetPmtuTimeout(&pmtu, 2u, LIMIT, now) && pmtu.current == BASE,
           "two with large packets in flight fall to the base size");
    Expect(!NetPmtuTimeout(&pmtu, 3u, LIMIT, now), "below the base there is nowhere to fall");
}

/* Чёрная дыра по таймеру и поиск после неё на пути шириной path. Итог —
 * пауза перед поиском. */
static uint64_t FallAndSearch(uint32_t path)
{
    Expect(NetPmtuTimeout(&pmtu, 2u, pmtu.current, now), "a black hole");
    const uint64_t fall = now;
    while (NetPmtuProbeSize(&pmtu, now) == 0u)
        now += NET_MILLISECOND;
    const uint64_t holdOff = now - fall;
    Probe(path);
    return holdOff;
}

static void TestHoldOff(void)
{
    Begin(BASE, LIMIT);
    Probe(LIMIT);
    Expect(FallAndSearch(LIMIT) == NET_SECOND, "the first black hole holds off a second");
    now += 10u * NET_SECOND;
    Expect(FallAndSearch(LIMIT) == 2u * NET_SECOND, "a second one soon after holds off two");
    now += 10u * NET_SECOND;
    Expect(FallAndSearch(LIMIT) == 4u * NET_SECOND, "a third one four");
    for (uint32_t fall = 0u; fall < 12u; ++fall)
        FallAndSearch(LIMIT);
    Expect(FallAndSearch(LIMIT) == 10u * MINUTE, "the hold-off stops at ten minutes");
    now += 10u * MINUTE;
    Expect(FallAndSearch(LIMIT) == NET_SECOND, "ten quiet minutes reset it");
    /* Десять минут считаются от конца паузы. */
    now = pmtu.fallAt + pmtu.holdOff + 10u * MINUTE - 1u;
    Expect(FallAndSearch(LIMIT) == 2u * NET_SECOND, "a moment less still doubles it");
    now = pmtu.fallAt + pmtu.holdOff + 10u * MINUTE;
    Expect(FallAndSearch(LIMIT) == NET_SECOND, "the full ten minutes do not");

    /* Проба, ушедшая до чёрной дыры, ничего не решает: ни потерей, ни
     * подтверждением. */
    Begin(BASE, LIMIT);
    Probe(1300u);
    const uint32_t found = pmtu.current;
    now += 10u * MINUTE;
    NetPmtuProbeSent(&pmtu, NetPmtuProbeSize(&pmtu, now));
    const uint64_t sent = now;
    now += RTT;
    Expect(NetPmtuTimeout(&pmtu, 2u, found, now), "a black hole during the search");
    NetPmtuProbeLost(&pmtu, LIMIT, sent, now);
    Expect(pmtu.failures == 0u && pmtu.next == found, "the probe from before it is ignored");
    Expect(!NetPmtuProbeAcked(&pmtu, packet++, LIMIT, sent, now) && pmtu.current == BASE,
           "even when it is acknowledged");
    Expect(NetPmtuProbeSize(&pmtu, now + NET_SECOND) == found,
           "and the search resumes after the hold-off");
    /* Новый путь — новый поиск. */
    NetPmtuProbeSent(&pmtu, LIMIT);
    NetPmtuReset(&pmtu, BASE, LIMIT, now + RTT);
    Expect(!NetPmtuProbeAcked(&pmtu, packet++, LIMIT, now, now + RTT) && pmtu.current == BASE,
           "a probe of the old path proves nothing about a new one");
    Expect(NetPmtuProbeSize(&pmtu, now + RTT) == LIMIT, "and the new search starts at once");
    NetPmtuProbeSent(&pmtu, LIMIT);
    NetPmtuProbeLost(&pmtu, LIMIT, now, now + 2u * RTT);
    Expect(pmtu.failures == 0u && NetPmtuProbeSize(&pmtu, now + 2u * RTT) == 0u,
           "nor does its loss count against the new probe in flight");
}

LAIUE_TEST_ENTRY(NetPmtuTestEntryPoint)
{
    TestEveryPath();
    TestProbeSequence();
    TestRaise();
    TestNarrowLimits();
    TestLateAcknowledgement();
    TestFailureCount();
    TestBlackHoleLosses();
    TestSuspicion();
    TestProvenByProbe();
    TestBlackHoleTimeouts();
    TestHoldOff();
    LaiueTestRuntimeWrite("net path size: passed\n");
    LAIUE_TEST_SUCCESS();
}