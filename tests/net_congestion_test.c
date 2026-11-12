/* Окно перегрузки laiue.net отдельно от транспорта: CUBIC (RFC 9438) и
 * HyStart++ (RFC 9406) на заданных подтверждениях и замерах оборота.
 *
 * Полёт в этой модели — окно пакетов по датаграмме, ушедших разом; за оборот
 * они подтверждаются по ACK на два пакета, и каждый ACK несёт замер. */

#include "net/net_internal.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define DATAGRAM 1200u
#define MS UINT64_C(1000)
#define SECOND UINT64_C(1000000)

static NetCongestion congestion;
static uint64_t now;
static uint64_t nextNumber;

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
    LaiueTestRuntimeWrite("Net congestion failure: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ExpectBetween(uint64_t value, uint64_t low, uint64_t high, const char *message)
{
    if (value >= low && value <= high)
        return;
    LaiueTestRuntimeWrite("Net congestion failure: ");
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

static void Begin(uint64_t window)
{
    now = SECOND;
    nextNumber = 0u;
    NetCongestionReset(&congestion, DATAGRAM, window, nextNumber);
}

static void Ack(uint64_t largest, uint64_t sample, uint64_t smoothed, uint64_t bytes)
{
    const NetCongestionAck ack = {
        .now = now,
        .largest = largest,
        .nextNumber = nextNumber,
        .rttSample = sample,
        .smoothedRtt = smoothed,
        .limitedBytes = bytes,
        .limitedSentTime = bytes != 0u ? now - smoothed : 0u,
    };
    NetCongestionOnAck(&congestion, &ack);
}

static void Flight(uint64_t rtt, uint64_t sample)
{
    uint64_t packets = congestion.window / DATAGRAM;
    if (packets < 2u)
        packets = 2u;
    const uint64_t first = nextNumber;
    nextNumber += packets;
    const uint64_t acks = (packets + 1u) / 2u;
    const uint64_t start = now;
    for (uint64_t ack = 0u; ack < acks; ++ack)
    {
        /* Нечётный полёт кончается подтверждением одного пакета. Полёт длится
         * ровно оборот, даже если подтверждений больше, чем микросекунд. */
        const uint64_t count = packets - 2u * ack >= 2u ? 2u : 1u;
        now = start + rtt * (ack + 1u) / acks;
        Ack(first + 2u * ack + count - 1u, sample, rtt, count * DATAGRAM);
    }
}

/* Полёты, пока время кривой не дойдёт до elapsed. */
static void FlyUntil(uint64_t elapsed, uint64_t rtt)
{
    while (!congestion.cubic.started || congestion.cubic.elapsed < elapsed)
        Flight(rtt, rtt);
}

static uint64_t Segments(uint64_t bytes)
{
    return bytes / DATAGRAM;
}

/* Потеря свежего пакета: начинается восстановление. */
static void Lose(void)
{
    now += MS;
    Expect(NetCongestionOnLoss(&congestion, now - 1u, now), "a fresh loss reduces the window");
}

static void TestCubeRoot(void)
{
    /* Точные кубы и числа сразу под ними, вплоть до наибольшего корня. */
    for (uint64_t root = 0u; root <= UINT64_C(2642245); ++root)
    {
        const uint64_t cube = root * root * root;
        if (NetCubeRoot(cube) != root || (root != 0u && NetCubeRoot(cube - 1u) != root - 1u))
            Expect(false, "cube root is exact around cubes");
    }
    Expect(NetCubeRoot(UINT64_MAX) == UINT64_C(2642245), "cube root of the largest value");
    uint64_t state = UINT64_C(0x243F6A8885A308D3);
    for (uint32_t round = 0u; round < 100000u; ++round)
    {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        const uint64_t value = state >> (round % 64u);
        const uint64_t root = NetCubeRoot(value);
        const uint64_t next = root + 1u;
        if (root * root * root > value || (root < UINT64_C(2642245) && next * next * next <= value))
            Expect(false, "cube root rounds down");
    }
}

static void TestSlowStart(void)
{
    Begin(10u * DATAGRAM);
    Ack(1u, 50u * MS, 50u * MS, 2u * DATAGRAM);
    Expect(congestion.window == 12u * DATAGRAM, "slow start grows by the bytes acknowledged");
    Ack(3u, 50u * MS, 50u * MS, 0u);
    Expect(congestion.window == 12u * DATAGRAM, "an application-limited flight does not grow it");
    Lose();
    const uint64_t reduced = congestion.window;
    Expect(reduced == 12u * DATAGRAM * 7u / 10u, "a loss in slow start reduces to 0.7");
    Ack(5u, 50u * MS, 50u * MS, 2u * DATAGRAM);
    Expect(congestion.window == reduced, "packets sent before the loss do not grow the window");
}

static void TestReduction(void)
{
    Begin(100u * DATAGRAM);
    Lose();
    Expect(congestion.window == 70u * DATAGRAM && congestion.threshold == 70u * DATAGRAM,
           "the window and threshold drop to 0.7");
    Expect(congestion.cubic.maxWindow == 100u * DATAGRAM &&
               congestion.cubic.priorWindow == 100u * DATAGRAM,
           "the curve remembers the window before the loss");
    Expect(!NetCongestionOnLoss(&congestion, now - 1u, now + MS) &&
               congestion.window == 70u * DATAGRAM,
           "one reduction per flight");
    /* Окно не дотянуло до прежнего максимума: быстрая сходимость. */
    Lose();
    Expect(congestion.window == 49u * DATAGRAM, "a later loss reduces again");
    Expect(congestion.cubic.maxWindow == 70u * DATAGRAM * 17u / 20u,
           "fast convergence lowers the remembered maximum");
    Begin(2u * DATAGRAM);
    Lose();
    Expect(congestion.window == 2u * DATAGRAM, "the window never drops below two datagrams");
}

static void TestCubicCurve(void)
{
    const uint64_t rtt = 100u * MS;
    Begin(1000u * DATAGRAM);
    Lose();
    FlyUntil(0u, rtt);
    /* K = ∛(300 / 0,4) с = 9,0856 с. */
    Expect(congestion.cubic.plateau == 9085u, "the plateau time follows the cube root");
    FlyUntil(4543u * MS, rtt);
    ExpectBetween(Segments(congestion.window), 950u, 975u,
                  "halfway to the plateau most of the loss is recovered");
    FlyUntil(8085u * MS, rtt);
    const uint64_t approaching = congestion.window;
    ExpectBetween(Segments(approaching), 995u, 1000u, "the window approaches the old maximum");
    FlyUntil(10085u * MS, rtt);
    ExpectBetween(Segments(congestion.window - approaching), 0u, 4u,
                  "the curve is flat around the old maximum");
    FlyUntil(14085u * MS, rtt);
    ExpectBetween(Segments(congestion.window), 1045u, 1060u, "beyond it the curve probes");
    FlyUntil(19085u * MS, rtt);
    ExpectBetween(Segments(congestion.window), 1390u, 1440u, "and probing accelerates");
}

static void TestRenoRegion(void)
{
    /* Оборот 2 мс: кривая вернулась бы к 20 датаграммам за 2,5 с, Reno — за
     * дюжину оборотов. */
    Begin(20u * DATAGRAM);
    Lose();
    FlyUntil(0u, 2u * MS);
    for (uint32_t flight = 0u; flight < 12u; ++flight)
        Flight(2u * MS, 2u * MS);
    ExpectBetween(Segments(congestion.window), 20u, 22u,
                  "below the old window the Reno estimate grows by 9/17 per round trip");
    for (uint32_t flight = 0u; flight < 50u; ++flight)
        Flight(2u * MS, 2u * MS);
    ExpectBetween(Segments(congestion.window), 69u, 73u, "then by one datagram per round trip");

    /* Окно в семь тысяч датаграмм: доля одного подтверждения меньше байта,
     * но остатки копятся, и оценка растёт на те же 9/17 за оборот. */
    Begin(10000u * DATAGRAM);
    Lose();
    FlyUntil(0u, 2u * MS);
    const uint64_t reno = congestion.cubic.renoWindow;
    for (uint32_t flight = 0u; flight < 17u; ++flight)
        Flight(2u * MS, 2u * MS);
    ExpectBetween(Segments(congestion.cubic.renoWindow - reno), 8u, 10u,
                  "fractions of a byte per acknowledgement add up");
}

static void TestApplicationLimited(void)
{
    const uint64_t rtt = 100u * MS;
    Begin(100u * DATAGRAM);
    Lose();
    FlyUntil(2u * SECOND, rtt);
    const uint64_t window = congestion.window;
    const uint64_t elapsed = congestion.cubic.elapsed;
    /* Двадцать секунд приложению нечего слать: подтверждения идут, окно не
     * тесно. */
    for (uint32_t ack = 0u; ack < 2000u; ++ack)
    {
        now += 10u * MS;
        Ack(nextNumber, rtt, rtt, 0u);
    }
    Expect(congestion.window == window && congestion.cubic.elapsed == elapsed,
           "idle time neither grows the window nor runs the curve");
    /* И ещё пять секунд тишины без подтверждений. */
    now += 5u * SECOND;
    Flight(rtt, rtt);
    ExpectBetween(congestion.cubic.elapsed - elapsed, 90u * MS, 100u * MS,
                  "the curve resumes where it stopped");
    Expect(congestion.window < window + window / 20u, "and the window does not jump");

    /* Отправитель замолчал посреди тесного окна: подтверждений нет вовсе, и
     * пауза видна лишь по разрыву между ними. */
    const uint64_t before = congestion.cubic.elapsed;
    now += 20u * SECOND;
    Flight(rtt, rtt);
    Expect(congestion.cubic.elapsed - before < 2u * rtt,
           "a silent pause runs the curve by at most a round trip");
}

static void TestHyStart(void)
{
    const uint64_t rtt = 50u * MS;
    Begin(10u * DATAGRAM);
    for (uint32_t flight = 0u; flight < 4u; ++flight)
        Flight(rtt, rtt);
    Expect(congestion.window == 160u * DATAGRAM && !congestion.cautious,
           "slow start doubles the window every round trip");
    /* Оборот вырос на 8 мс, больше восьмой части: на узком месте очередь. */
    Flight(rtt, 58u * MS);
    Expect(congestion.cautious && congestion.baselineRtt == 58u * MS,
           "a longer round trip makes the growth cautious");
    uint64_t before = congestion.window;
    Flight(rtt, 58u * MS);
    ExpectBetween(congestion.window * 100u / before, 124u, 126u,
                  "cautious growth is a quarter of slow start");
    for (uint32_t flight = 0u; flight < 3u; ++flight)
        Flight(rtt, 58u * MS);
    Expect(congestion.cautious && congestion.threshold == UINT64_MAX,
           "cautious growth lasts five rounds");
    Flight(rtt, 58u * MS);
    Expect(!congestion.cautious && congestion.threshold != UINT64_MAX &&
               congestion.cubic.priorWindow == congestion.threshold,
           "then congestion avoidance begins without a loss");
    Expect(congestion.cubic.started && congestion.cubic.plateau == 0u &&
               congestion.cubic.maxWindow == congestion.threshold,
           "and the curve starts by probing beyond the window");

    /* Задержка растёт и дальше: очередь копится раунд за раундом. Порог —
     * восьмая часть прошлого оборота: 7,25 мс от 58 мс и 7,75 мс от 62 мс. */
    Begin(10u * DATAGRAM);
    for (uint32_t flight = 0u; flight < 4u; ++flight)
        Flight(rtt, rtt);
    Flight(rtt, 58u * MS);
    Flight(rtt, 62u * MS);
    Expect(congestion.cautious, "growth below the threshold keeps the growth cautious");
    before = congestion.window;
    Flight(rtt, 70u * MS);
    Expect(!congestion.cautious && congestion.threshold != UINT64_MAX &&
               congestion.threshold < before + before / 8u,
           "a delay growing again ends slow start without waiting five rounds");

    /* Задержка вернулась: осторожность была ложной. */
    Begin(10u * DATAGRAM);
    for (uint32_t flight = 0u; flight < 4u; ++flight)
        Flight(rtt, rtt);
    Flight(rtt, 58u * MS);
    Expect(congestion.cautious, "the round trip grows again");
    Flight(rtt, rtt);
    Expect(!congestion.cautious && congestion.baselineRtt == UINT64_MAX,
           "a shorter round trip resumes slow start");
    before = congestion.window;
    Flight(rtt, rtt);
    Expect(congestion.window == 2u * before, "and the window doubles again");

    /* Рост задержки меньше порога — шум: 4 мс от 50 мс не хватает 6,25 мс. */
    Begin(10u * DATAGRAM);
    for (uint32_t flight = 0u; flight < 4u; ++flight)
        Flight(rtt, rtt);
    Flight(rtt, 54u * MS);
    Expect(!congestion.cautious, "a small delay increase is noise");

    /* После потери медленный старт идёт до порога, и задержка его не
     * прерывает. */
    Begin(100u * DATAGRAM);
    Lose();
    NetCongestionCollapse(&congestion, now);
    while (congestion.window < 32u * DATAGRAM)
        Flight(rtt, rtt);
    Flight(rtt, 80u * MS);
    Expect(!congestion.cautious && congestion.window < congestion.threshold,
           "HyStart++ is only for the first slow start");
}

/* Четыре полёта медленного старта на обороте rtt, затем раунд с оборотом
 * sample: стал ли рост осторожным. */
static bool QueueSeen(uint64_t rtt, uint64_t sample)
{
    Begin(10u * DATAGRAM);
    for (uint32_t flight = 0u; flight < 4u; ++flight)
        Flight(rtt, rtt);
    Flight(rtt, sample);
    return congestion.cautious;
}

static void TestHyStartLimits(void)
{
    /* Порог — восьмая часть оборота, но не меньше 4 мс и не больше 16 мс. */
    Expect(!QueueSeen(10u * MS, 13u * MS) && QueueSeen(10u * MS, 14u * MS),
           "on a short path the threshold is 4 ms");
    Expect(!QueueSeen(200u * MS, 215u * MS) && QueueSeen(200u * MS, 216u * MS),
           "on a long path it is 16 ms");

    /* Раунд кончается подтверждением первого пакета, отправленного в нём, а
     * первому раунду сравнивать не с чем. */
    const uint64_t rtt = 50u * MS;
    Begin(20u * DATAGRAM);
    nextNumber = 20u;
    Ack(0u, rtt, rtt, DATAGRAM);
    Expect(congestion.roundEnd == 20u, "a round ends when its first packet is acknowledged");
    Begin(20u * DATAGRAM);
    Flight(rtt, rtt);
    Expect(!congestion.cautious && congestion.roundSamples >= 8u,
           "the first round has nothing to compare with");

    /* Решает раунд не меньше чем из восьми подтверждений. */
    Begin(6u * DATAGRAM);
    Flight(rtt, rtt);
    Flight(rtt, 58u * MS);
    Expect(!congestion.cautious, "a round of six samples decides nothing");
    Begin(8u * DATAGRAM);
    Flight(rtt, rtt);
    Flight(rtt, 58u * MS);
    Expect(congestion.cautious, "a round of eight samples decides");

    /* Потеря посреди осторожного роста кончает и его: дальше окно ведёт
     * CUBIC, и раунды HyStart++ его не трогают. */
    Expect(QueueSeen(rtt, 58u * MS), "the queue starts to grow");
    Lose();
    const uint64_t threshold = congestion.threshold;
    for (uint32_t flight = 0u; flight < 6u; ++flight)
        Flight(rtt, 58u * MS);
    Expect(!congestion.cautious && congestion.threshold == threshold,
           "a loss ends cautious growth for good");
}

static void TestSecondLoss(void)
{
    const uint64_t rtt = 100u * MS;
    Begin(100u * DATAGRAM);
    Lose();
    FlyUntil(3u * SECOND, rtt);
    Lose();
    const uint64_t window = congestion.window;
    const uint64_t maximum = congestion.cubic.maxWindow;
    Expect(maximum < congestion.cubic.priorWindow, "the window had not reached the old maximum");
    FlyUntil(0u, rtt);
    Expect(congestion.cubic.elapsed <= 2u * rtt &&
               congestion.cubic.plateau ==
                   NetCubeRoot((maximum - window) * UINT64_C(2500000000) / DATAGRAM),
           "a new loss restarts the curve towards the lowered maximum");
}

static void TestUndo(void)
{
    const uint64_t rtt = 100u * MS;
    Begin(100u * DATAGRAM);
    Lose();
    FlyUntil(3u * SECOND, rtt);
    const NetCongestion before = congestion;
    Lose();
    NetCongestionUndo(&congestion);
    Expect(congestion.window == before.window && congestion.threshold == before.threshold &&
               !congestion.inRecovery,
           "a spurious loss restores the window and threshold");
    Expect(congestion.cubic.maxWindow == before.cubic.maxWindow &&
               congestion.cubic.priorWindow == before.cubic.priorWindow &&
               congestion.cubic.renoWindow == before.cubic.renoWindow &&
               congestion.cubic.elapsed == before.cubic.elapsed &&
               congestion.cubic.plateau == before.cubic.plateau && congestion.cubic.started,
           "and the curve");

    /* Мнимая потеря первого медленного старта возвращает его. */
    Begin(10u * DATAGRAM);
    Flight(rtt, rtt);
    const uint64_t window = congestion.window;
    Lose();
    NetCongestionUndo(&congestion);
    Expect(congestion.window == window && congestion.threshold == UINT64_MAX,
           "an undone loss resumes the first slow start");
}

static void TestCollapse(void)
{
    const uint64_t rtt = 100u * MS;
    Begin(100u * DATAGRAM);
    Lose();
    FlyUntil(2u * SECOND, rtt);
    NetCongestionCollapse(&congestion, now);
    Expect(congestion.window == 2u * DATAGRAM && congestion.threshold == 70u * DATAGRAM,
           "persistent congestion collapses the window but keeps the threshold");
    FlyUntil(0u, rtt);
    Expect(congestion.cubic.plateau == 0u && congestion.cubic.maxWindow >= 70u * DATAGRAM,
           "after it the curve starts from the window, not the old maximum");

    /* Затяжная перегрузка посреди осторожного роста кончает и его. */
    Expect(QueueSeen(50u * MS, 58u * MS), "the queue starts to grow");
    NetCongestionCollapse(&congestion, now);
    Ack(nextNumber, 50u * MS, 50u * MS, 2u * DATAGRAM);
    Expect(congestion.window == 2u * DATAGRAM,
           "packets sent before the collapse do not grow the window");
    now += rtt;
    Flight(50u * MS, 50u * MS);
    Expect(!congestion.cautious && congestion.window == 4u * DATAGRAM,
           "after a collapse slow start runs at full speed");
}

static void TestLimits(void)
{
    Begin(NET_CONGESTION_WINDOW_LIMIT - DATAGRAM);
    Ack(1u, 50u * MS, 50u * MS, UINT64_C(1) << 20u);
    Expect(congestion.window == NET_CONGESTION_WINDOW_LIMIT, "slow start stops at the limit");
    Lose();
    /* Тысяча часовых оборотов: кривая уходит за предел своего расстояния. */
    for (uint32_t step = 0u; step < 1000u; ++step)
    {
        now += 3600u * SECOND;
        Ack(nextNumber, 50u * MS, 3600u * SECOND, congestion.window);
    }
    Expect(congestion.window == NET_CONGESTION_WINDOW_LIMIT &&
               congestion.cubic.renoWindow <= NET_CONGESTION_WINDOW_LIMIT,
           "hours of probing stop at the limit");
    /* Подтверждение нескольких окон разом доводит окно лишь до цели. Цель на
     * пятисекундном обороте — кривая через 25 с, около 1,7 окна, и она
     * упирается в полтора окна. */
    Begin(1000u * DATAGRAM);
    Lose();
    FlyUntil(20u * SECOND, 100u * MS);
    const uint64_t window = congestion.window;
    now += 100u * MS;
    Ack(nextNumber, 100u * MS, 5u * SECOND, 4u * window);
    Expect(congestion.window == window + window / 2u, "one acknowledgement grows at most to 1.5");
    /* Подтверждения по байту приносят по полбайта роста, и доли копятся. */
    const uint64_t grown = congestion.window;
    for (uint32_t ack = 0u; ack < 4u; ++ack)
    {
        now += MS;
        Ack(nextNumber, 100u * MS, 5u * SECOND, 1u);
    }
    Expect(congestion.window > grown, "fractions of a byte of growth add up");
}

LAIUE_TEST_ENTRY(NetCongestionTestEntryPoint)
{
    TestCubeRoot();
    TestSlowStart();
    TestReduction();
    TestCubicCurve();
    TestRenoRegion();
    TestApplicationLimited();
    TestHyStart();
    TestHyStartLimits();
    TestSecondLoss();
    TestUndo();
    TestCollapse();
    TestLimits();
    LaiueTestRuntimeWrite("net congestion: passed\n");
    LAIUE_TEST_SUCCESS();
}
