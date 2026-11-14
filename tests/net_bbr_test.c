/* Управление перегрузкой BBR (draft-ietf-ccwg-bbr-07) и замер скорости
 * доставки laiue.net отдельно от транспорта.
 *
 * Сначала замер на заданных отправках и подтверждениях, затем правила BBR на
 * заданных подтверждениях и потерях, затем BBR на модели пути: отправитель,
 * узкое место с очередью и задержка. Модель ведёт BBR теми же вызовами, что
 * и транспорт, но без пакетов, ключей и каналов, поэтому каждое состояние
 * видно. Арифметика целая, и на любой платформе модель проходит один и тот
 * же путь. */

#include "net/net_internal.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DATAGRAM 1200u
#define MS UINT64_C(1000)
#define SECOND UINT64_C(1000000)
#define UNLIMITED UINT64_MAX
#define WINDOW (UINT64_C(10) * DATAGRAM)

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
    LaiueTestRuntimeWrite("Net BBR failure: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ExpectEqual(uint64_t value, uint64_t expected, const char *message)
{
    if (value == expected)
        return;
    LaiueTestRuntimeWrite("Net BBR failure: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite(": got ");
    WriteNumber(value);
    LaiueTestRuntimeWrite(", expected ");
    WriteNumber(expected);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void ExpectBetween(uint64_t value, uint64_t low, uint64_t high, const char *message)
{
    if (value >= low && value <= high)
        return;
    LaiueTestRuntimeWrite("Net BBR failure: ");
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

/* ---- Замер скорости доставки -------------------------------------------------------- */

#define METER_PACKETS 512u

/* Отправитель с одним замером: пакеты по тысяче байт. */
static struct
{
    NetRate rate;
    NetRateStamp stamps[METER_PACKETS];
    uint64_t sentTimes[METER_PACKETS];
    uint64_t inFlight;
    uint32_t sent;
    /* Что получатель говорит в кадрах о своих часах: их погрешность и то, что
     * сеть он не дочитал. */
    uint64_t age;
    bool open;
} meter;

static void MeterBegin(void)
{
    memset(&meter, 0, sizeof(meter));
}

/* Отправляет пакет в now; при ровном темпе он ушёл бы в paced. */
static uint32_t MeterSend(uint64_t paced, uint64_t now)
{
    const uint32_t index = meter.sent;
    Expect(index < METER_PACKETS, "the meter has room for the packet");
    ++meter.sent;
    meter.inFlight += 1000u;
    meter.sentTimes[index] = now;
    NetRateSent(&meter.rate, &meter.stamps[index], meter.inFlight, 1000u, paced, now);
    return index;
}

/* Кадр подтверждения с часами получателя clock. */
static NetRateSample MeterFrame(uint64_t clock)
{
    const NetRateSample sample = {.ackTime = clock, .ackAge = meter.age, .ackOpen = meter.open};
    return sample;
}

static void MeterAck(NetRateSample *sample, uint32_t index, uint64_t now)
{
    meter.inFlight -= 1000u;
    NetRateAcked(&meter.rate, sample, &meter.stamps[index], index, meter.sentTimes[index], 1000u,
                 now);
}

/* Подтверждает пакеты first..last одним кадром и считает замер. */
static NetRateSample MeterAckRange(uint32_t first, uint32_t last, uint64_t clock, uint64_t now,
                                   uint64_t minRtt)
{
    NetRateSample sample = MeterFrame(clock);
    for (uint32_t index = first; index <= last; ++index)
        MeterAck(&sample, index, now);
    NetRateFinish(&meter.rate, &sample, minRtt);
    return sample;
}

static void TestRateSteady(void)
{
    /* Пакет в миллисекунду, оборот 50 мс, подтверждение на каждый пакет:
     * мегабайт в секунду, какой бы пакет ни дал замер. */
    MeterBegin();
    NetRateSample sample = {0};
    for (uint32_t tick = 0u; tick < 200u; ++tick)
    {
        const uint64_t now = SECOND + tick * MS;
        if (tick >= 50u)
        {
            sample = MeterAckRange(tick - 50u, tick - 50u, now, now, 50u * MS);
            if (tick > 100u)
            {
                ExpectEqual(sample.rate, 1000000u, "a steady stream measures its own rate");
                ExpectEqual(sample.proven, 1000000u, "and exact receiver clocks prove all of it");
                ExpectEqual(sample.interval, 50u * MS, "over one round trip");
                ExpectEqual(sample.delivered, 50000u, "in which a flight is delivered");
                ExpectEqual(sample.flight, 50000u, "and the flight is the delivered volume");
                ExpectEqual(sample.stamp.inFlight, 50000u, "the packet remembers the flight");
            }
        }
        MeterSend(now, now);
    }
    ExpectEqual(sample.acked, 1000u, "one acknowledgement delivers one packet");
    Expect(sample.filled && sample.number == 149u && sample.sentTime == SECOND + 149u * MS &&
               !sample.stamp.appLimited && sample.lost == 0u,
           "the sample describes the newest packet");
}

static void TestRateIntervals(void)
{
    /* Десять пакетов уходят разом, подтверждения приходят по одному в
     * миллисекунду: скорость — скорость подтверждений. */
    MeterBegin();
    uint64_t now = SECOND;
    for (uint32_t index = 0u; index < 20u; ++index)
        MeterSend(now, now);
    NetRateSample sample = {0};
    for (uint32_t index = 0u; index < 10u; ++index)
        sample = MeterAckRange(index, index, now + 40u * MS + index * MS,
                               now + 40u * MS + index * MS, 0u);
    ExpectEqual(sample.interval, 49u * MS, "a burst is measured by its acknowledgements");
    ExpectEqual(sample.rate, 10000u * SECOND / (49u * MS), "at their rate");

    /* Двадцать пакетов уходят по одному в миллисекунду, а подтверждения
     * приходят одним кадром. До первого подтверждения оба отрезка считаются
     * от первой отправки: отрезок подтверждений длиннее, и скорость — его. */
    MeterBegin();
    now = 10u * SECOND;
    for (uint32_t index = 0u; index < 20u; ++index)
        MeterSend(now + index * MS, now + index * MS);
    sample = MeterAckRange(0u, 0u, now + 40u * MS, now + 40u * MS, 0u);
    ExpectEqual(sample.interval, 40u * MS, "the first packet is timed from its own departure");
    const uint32_t later = MeterSend(now + 40u * MS, now + 40u * MS);
    sample = MeterAckRange(1u, 19u, now + 41u * MS, now + 41u * MS, 0u);
    ExpectEqual(sample.interval, 41u * MS, "the first flight is timed from the first departure");
    ExpectEqual(sample.rate, 20000u * SECOND / (41u * MS), "by the longer of the two intervals");
    /* Пакет, отправленный после первого подтверждения, меряет от него. */
    sample = MeterAckRange(later, later, now + 80u * MS, now + 80u * MS, 0u);
    ExpectEqual(sample.interval, 40u * MS, "a later packet is timed from the acknowledgement");
    ExpectEqual(sample.delivered, 20000u, "and counts what was delivered since it left");

    /* Очередь на пути тает: пакеты уходят раз в две миллисекунды, а
     * подтверждения приходят раз в одну. Отрезок подтверждений вдвое короче
     * отрезка отправки, и по нему скорость вышла бы вдвое выше той, с какой
     * соединение слало. */
    MeterBegin();
    uint32_t acked = 0u;
    for (uint64_t offset = 0u; offset <= 180u * MS; offset += 500u)
    {
        if (offset % (2u * MS) == 0u && meter.sent < 80u)
            MeterSend(now + offset, now + offset);
        if (offset < 100u * MS || offset % MS == 0u || acked == meter.sent)
            continue;
        sample = MeterAckRange(acked, acked, now + offset, now + offset, 0u);
        ++acked;
        if (acked != 61u)
            continue;
        /* Пакет 60 ушёл на 120-й миллисекунде, сразу после подтверждения
         * пакета 19, отправленного на 38-й; сам подтверждён на 160,5-й. */
        ExpectEqual(sample.delivered, 41000u, "forty-one packets are delivered during the flight");
        ExpectEqual(sample.interval, 82u * MS,
                    "compressed acknowledgements are timed by the departures");
        ExpectEqual(sample.rate, 500000u, "and cannot raise the rate above the sending rate");
        ExpectEqual(sample.proven, 500000u, "nor what the path has proven");
    }
    ExpectEqual(acked, 80u, "every packet of the draining queue is acknowledged");

    /* Отрезок короче наименьшего оборота скорости не даёт, но доставленное
     * считает. */
    MeterBegin();
    MeterSend(now, now);
    sample = MeterAckRange(0u, 0u, now + 10u * MS, now + 10u * MS, 11u * MS);
    Expect(sample.filled && sample.rate == 0u && sample.proven == 0u && sample.delivered == 1000u &&
               sample.interval == 10u * MS,
           "an interval shorter than the round trip gives no rate");
    MeterSend(now + 20u * MS, now + 20u * MS);
    sample = MeterAckRange(1u, 1u, now + 31u * MS, now + 31u * MS, 11u * MS);
    ExpectEqual(sample.rate, 1000u * SECOND / (11u * MS), "an interval of one round trip does");
    /* Кадр без новых пакетов замера не даёт. */
    NetRateSample empty = {0};
    NetRateFinish(&meter.rate, &empty, 0u);
    Expect(!empty.filled && empty.rate == 0u && empty.proven == 0u && empty.flight == 0u,
           "an empty frame is no sample");
}

/* Обмен с обновлением раз в 16 мс: пачка в шестнадцать пакетов уходит разом,
 * и каждое обновление одним кадром подтверждает остаток прошлой пачки и
 * начало следующей, пять либо одиннадцать пакетов: путь делит пачки как
 * придётся. paced — отправка помечена временем ровного темпа, по пакету в
 * миллисекунду, иначе временем обновления. Возвращает наибольший и
 * наименьший замеры установившегося обмена. */
static void RunBursts(bool paced, uint64_t *highest, uint64_t *lowest)
{
    MeterBegin();
    *highest = 0u;
    *lowest = UINT64_MAX;
    uint32_t next = 0u;
    for (uint32_t update = 0u; update < 16u; ++update)
    {
        const uint64_t now = SECOND + 16u * MS * update;
        if (update >= 3u)
        {
            const uint32_t last = (update - 3u) * 16u + (update % 2u == 0u ? 5u : 11u) - 1u;
            const NetRateSample sample = MeterAckRange(next, last, now, now, 30u * MS);
            next = last + 1u;
            if (update >= 9u)
            {
                *highest = sample.rate > *highest ? sample.rate : *highest;
                *lowest = sample.rate < *lowest ? sample.rate : *lowest;
            }
        }
        for (uint32_t index = 0u; index < 16u; ++index)
            MeterSend(paced ? now - 16u * MS + (index + 1u) * MS : now, now);
    }
}

static void TestRatePacedTime(void)
{
    /* Кадр, подтвердивший на шесть пакетов больше прошлого, меряет ровно
     * темп: отрезок отправки длиннее на те же шесть миллисекунд. Кадр,
     * подтвердивший на шесть меньше, меряет ниже: 42 пакета за 48 мс. */
    uint64_t highest = 0u;
    uint64_t lowest = 0u;
    RunBursts(true, &highest, &lowest);
    ExpectEqual(highest, 1000000u, "bursts stamped at an even pace never measure above it");
    ExpectEqual(lowest, 875000u, "and fewer acknowledgements measure below it");
    /* По времени обновления вся пачка ушла бы в один миг, и тот же обмен дал
     * бы больше, чем отправлено: 54 пакета за 48 мс. */
    RunBursts(false, &highest, &lowest);
    ExpectEqual(highest, 1125000u, "real burst times would overstate the rate by an eighth");
}

/* Отправитель шлёт на четверть быстрее пути, как при пробе полосы: пакет в
 * 0,8 мс при пути в пакет за миллисекунду. Получатель подтверждает раз в
 * 10 мс, а отправитель обновляется с опозданием до 9 мс и разбирает
 * подтверждения когда придётся. receiver — отрезок подтверждений считается
 * по часам получателя, иначе по своим. Возвращает наибольший и наименьший
 * замеры установившегося обмена. */
static void RunLateUpdates(bool receiver, uint64_t *highest, uint64_t *lowest)
{
    static const uint32_t lateness[] = {0u, 7u, 3u, 9u, 1u, 8u, 0u, 5u};
    MeterBegin();
    *highest = 0u;
    *lowest = UINT64_MAX;
    for (uint32_t update = 0u; update < 34u; ++update)
    {
        const uint64_t now = SECOND + 10u * MS * update + MS * lateness[update % 8u];
        if (update >= 4u)
        {
            /* Кадр ушёл от получателя ровно через 40 мс после отправки
             * первого пакета и дальше каждые 10 мс; в нём десять пакетов. */
            const uint32_t frame = update - 4u;
            const uint64_t emitted = SECOND + 40u * MS + 10u * MS * frame;
            const uint32_t first = frame == 0u ? 0u : (frame - 1u) * 10u + 1u;
            const NetRateSample sample =
                MeterAckRange(first, frame * 10u, receiver ? emitted : now, now, 0u);
            if (frame >= 8u)
            {
                *highest = sample.rate > *highest ? sample.rate : *highest;
                *lowest = sample.rate < *lowest ? sample.rate : *lowest;
            }
        }
        while (SECOND + meter.sent * 800u <= now)
            MeterSend(SECOND + meter.sent * 800u, now);
    }
}

static void TestRateReceiverClock(void)
{
    /* По своим часам отрезок между подтверждениями короче настоящего на
     * разницу опозданий, и замер выше скорости пути: фильтр наибольшего его и
     * запомнит. */
    uint64_t highest = 0u;
    uint64_t lowest = 0u;
    RunLateUpdates(false, &highest, &lowest);
    Expect(highest > 1100000u, "the sender's own clock would overstate the path by a tenth");
    Expect(lowest < 1000000u, "or understate it");
    /* По часам получателя замер — ровно скорость пути, когда бы
     * подтверждение ни разобрали. */
    RunLateUpdates(true, &highest, &lowest);
    ExpectEqual(highest, 1000000u, "the receiver clock measures the path itself");
    ExpectEqual(lowest, 1000000u, "whenever the acknowledgement is processed");
    /* Подтверждение, обогнанное более поздним, времени назад не ведёт и
     * погрешности не меняет. */
    const uint64_t before = meter.rate.deliveredTime;
    meter.age = 3u * MS;
    meter.open = true;
    (void)MeterAckRange(291u, 291u, before - 30u * MS, before + SECOND, 0u);
    ExpectEqual(meter.rate.deliveredTime, before, "an overtaken acknowledgement keeps the clock");
    Expect(meter.rate.deliveredAge == 0u && !meter.rate.deliveredOpen, "and what is known of it");
    ExpectEqual(meter.rate.ackedTime, before + SECOND, "though it is processed now");
}

/* Узкое место отдаёт пакет раз в gap микросекунд, а отправитель шлёт на
 * четверть чаще: очередь перед узким местом не пустеет. Получатель читает
 * сеть раз в period и называет в кадре всё пришедшее; первый пакет приходит
 * к нему через 40 мс и offset. Отправитель разбирает кадр в тот же миг.
 * Возвращает наибольшие скорость доставки и скорость, которую путь заведомо
 * выдал, в установившемся обмене. */
static void RunReads(uint64_t gap, uint64_t period, uint64_t offset, uint64_t *rate,
                     uint64_t *proven)
{
    MeterBegin();
    meter.age = period;
    *rate = 0u;
    *proven = 0u;
    const uint64_t pace = gap * 4u / 5u;
    uint32_t named = 0u;
    for (uint64_t elapsed = 0u; elapsed <= 400u * MS; elapsed += 100u)
    {
        const uint64_t now = SECOND + elapsed;
        while (meter.sent < METER_PACKETS && meter.sent * pace <= elapsed)
            MeterSend(SECOND + meter.sent * pace, now);
        if (elapsed == 0u || elapsed % period != 0u)
            continue;
        uint32_t arrived = named;
        while (arrived < meter.sent && 40u * MS + offset + arrived * gap <= elapsed)
            ++arrived;
        if (arrived == named)
            continue;
        const NetRateSample sample = MeterAckRange(named, arrived - 1u, now, now, 0u);
        named = arrived;
        if (elapsed < 200u * MS)
            continue;
        *rate = sample.rate > *rate ? sample.rate : *rate;
        *proven = sample.proven > *proven ? sample.proven : *proven;
    }
}

static void TestRateProven(void)
{
    /* Путь — мегабайт в секунду, получатель читает сеть раз в 10,3 мс. В
     * отрезок между двумя чтениями попадает то на пакет больше, то на пакет
     * меньше, чем путь отдаёт за это время, и наибольшая скорость доставки
     * выше пути: 83 пакета за 82,4 мс. Заведомая скорость считает без одного
     * пакета и пути не превышает: 113 из 114 пакетов за 113,3 мс. */
    uint64_t rate = 0u;
    uint64_t proven = 0u;
    RunReads(MS, 10300u, 500u, &rate, &proven);
    ExpectEqual(rate, 83000u * SECOND / 82400u,
                "between two reads the delivery rate overshoots the path");
    ExpectEqual(proven, 113000u * SECOND / 113300u, "and the proven rate stays within it");
    /* Чтение раз в 10 мс попадает в такт с путём: скорость доставки точна, а
     * заведомая ниже на пакет за отрезок. */
    RunReads(MS, 10u * MS, 500u, &rate, &proven);
    ExpectEqual(rate, 1000000u, "reads in step with the path measure it exactly");
    ExpectEqual(proven, 119000u * SECOND / (120u * MS), "and prove it less one packet");
    /* Путь — пакет в 10 мс, а получатель читает сеть каждую миллисекунду:
     * приход каждого пакета известен с точностью до неё. Без одного пакета
     * из десятка замер терял бы десятую часть; по погрешности часов — сотую. */
    RunReads(10u * MS, MS, 300u, &rate, &proven);
    ExpectEqual(rate, 100000u, "a receiver reading faster than packets arrive measures the path");
    ExpectEqual(proven, 12000u * SECOND / (121u * MS), "and proves it but for the age of a read");

    /* Погрешность часов получателя: 16 мс — он читает сеть реже, чем приходят
     * пакеты; миллисекунда — чаще; и такая, что в снимок не помещается, — её
     * он не знает и сам. */
    static const uint64_t ages[] = {16u * MS, MS, UINT64_C(1) << 40u};
    static const uint64_t loneProven[] = {875u * SECOND / (40u * MS), 1000u * SECOND / (41u * MS),
                                          875u * SECOND / (40u * MS)};
    static const uint64_t tenProven[] = {9000u * SECOND / (40u * MS), 10000u * SECOND / (41u * MS),
                                         9000u * SECOND / (40u * MS)};
    uint64_t now = 20u * SECOND;
    NetRateSample sample = {0};
    for (uint32_t pass = 0u; pass < 3u; ++pass)
    {
        /* Один пакет за отрезок. Без него от доставленного не осталось бы
         * ничего, и проба полосы не подняла бы оценку: заведомая скорость
         * считается без восьмой части — либо по погрешности часов, если так
         * выходит больше. */
        MeterBegin();
        meter.age = ages[pass];
        MeterSend(now, now);
        MeterSend(now, now);
        (void)MeterAckRange(0u, 0u, now + 40u * MS, now + 40u * MS, 0u);
        const uint32_t lone = MeterSend(now + 40u * MS, now + 40u * MS);
        if (pass == 2u)
            ExpectEqual(meter.stamps[lone].deliveredAge, UINT32_MAX,
                        "an age beyond the stamp saturates");
        sample = MeterAckRange(lone, lone, now + 80u * MS, now + 80u * MS, 0u);
        ExpectEqual(sample.rate, 25000u, "a lone packet is delivered at its own rate");
        ExpectEqual(sample.proven, loneProven[pass],
                    "and proves seven eighths of it, or what the clock allows");

        /* Десять пакетов за отрезок: без одного — либо по погрешности часов. */
        MeterBegin();
        meter.age = ages[pass];
        for (uint32_t index = 0u; index < 12u; ++index)
            MeterSend(now + index * MS, now + index * MS);
        (void)MeterAckRange(0u, 0u, now + 40u * MS, now + 40u * MS, 0u);
        const uint32_t probe = MeterSend(now + 40u * MS, now + 40u * MS);
        NetRateSample frame = MeterFrame(now + 80u * MS);
        for (uint32_t index = 1u; index <= 9u; ++index)
            MeterAck(&frame, index, now + 80u * MS);
        MeterAck(&frame, probe, now + 80u * MS);
        NetRateFinish(&meter.rate, &frame, 0u);
        ExpectEqual(frame.delivered, 10000u, "ten packets are delivered in the interval");
        ExpectEqual(frame.rate, 250000u, "at the rate of the path");
        ExpectEqual(frame.proven, tenProven[pass],
                    "and all but one are proven, or what the clock allows");
    }

    /* Получатель прочёл двадцать пакетов разом, а подтвердил двумя кадрами:
     * десять посреди чтения, десять после. Пакет, ушедший между кадрами,
     * считает вторые десять доставленными за свой полёт, хотя пришли они до
     * его отправки. */
    for (uint32_t pass = 0u; pass < 2u; ++pass)
    {
        MeterBegin();
        meter.age = 30u * MS;
        now = 40u * SECOND;
        for (uint32_t index = 0u; index < 20u; ++index)
            MeterSend(now + 40u * MS + index * MS, now + 59u * MS);
        /* Второй проход: первый кадр отправлен после чтения, а вторые десять
         * пакетов прочитаны позже — они в самом деле пришли за полёт. */
        meter.open = pass == 0u;
        (void)MeterAckRange(0u, 9u, now + 60u * MS, now + 60u * MS + 500u, 0u);
        const uint32_t between = MeterSend(now + 60u * MS + 500u, now + 60u * MS + 500u);
        Expect(meter.stamps[between].deliveredOpen == (pass == 0u) &&
                   meter.stamps[between].deliveredAge == 30u * MS,
               "a packet remembers what the last frame said of its clock");
        meter.open = false;
        (void)MeterAckRange(10u, 19u, now + 60u * MS + 10u * MS * pass, now + 61u * MS, 0u);
        Expect(!meter.rate.deliveredOpen, "the frame after the read closes it");
        meter.age = 10u * MS;
        sample = MeterAckRange(between, between, now + 100u * MS, now + 101u * MS, 0u);
        ExpectEqual(sample.delivered, 11000u,
                    "eleven packets count as delivered in forty milliseconds");
        ExpectEqual(sample.rate, 275000u, "and the delivery rate takes them all");
        if (pass == 0u)
            ExpectEqual(sample.proven, 11000u * SECOND / (70u * MS),
                        "a frame sent amid a read proves only from the read before it");
        else
            ExpectEqual(sample.proven, 250000u, "a frame sent after the read proves all but one");
    }

    /* Подтверждение пришло в тот же миг, в какой пакеты ушли: о приходе оно
     * не говорит, и остаётся скорость отправки. */
    MeterBegin();
    meter.age = 5u * MS;
    now = 50u * SECOND;
    const uint32_t early = MeterSend(now - 500u, now);
    const uint32_t late = MeterSend(now, now);
    sample = MeterAckRange(early, late, now, now, 0u);
    ExpectEqual(sample.rate, 4000000u, "an instant acknowledgement is timed by the departures");
    ExpectEqual(sample.proven, 4000000u, "and proves what was sent");
}

static void TestRateApplicationLimited(void)
{
    MeterBegin();
    uint64_t now = SECOND;
    for (uint32_t index = 0u; index < 4u; ++index)
        MeterSend(now, now);
    /* Слать больше нечего: отметка — всё, что сейчас в пути. */
    NetRateLimited(&meter.rate, meter.inFlight);
    ExpectEqual(meter.rate.appLimited, 4000u, "the mark covers the flight");
    now += 10u * MS;
    const uint32_t bubble = MeterSend(now, now);
    Expect(meter.stamps[bubble].appLimited, "a packet sent behind the mark is limited");
    now += 30u * MS;
    NetRateSample sample = MeterAckRange(0u, 3u, now, now, 0u);
    Expect(!sample.stamp.appLimited && meter.rate.appLimited == 4000u,
           "delivering the flight itself keeps the mark");
    const uint32_t still = MeterSend(now, now);
    Expect(meter.stamps[still].appLimited, "and packets are limited until it is passed");
    now += 10u * MS;
    sample = MeterAckRange(bubble, bubble, now, now, 0u);
    Expect(sample.stamp.appLimited && meter.rate.appLimited == 0u,
           "the bubble's own sample is limited, and passing the mark clears it");
    const uint32_t clear = MeterSend(now, now);
    Expect(!meter.stamps[clear].appLimited, "later packets measure the path again");
    /* Пустое соединение тоже получает отметку. */
    MeterBegin();
    NetRateLimited(&meter.rate, 0u);
    ExpectEqual(meter.rate.appLimited, 1u, "an idle connection is marked too");
}

static void TestRateIdleRestart(void)
{
    MeterBegin();
    meter.age = 7u * MS;
    meter.open = true;
    uint64_t now = SECOND;
    MeterSend(now, now);
    MeterSend(now, now);
    NetRateSample sample = MeterAckRange(0u, 1u, now + 40u * MS, now + 40u * MS, 0u);
    ExpectEqual(sample.interval, 40u * MS, "a flight is timed by its round trip");
    Expect(meter.rate.deliveredAge == 7u * MS && meter.rate.deliveredOpen,
           "the frame's word on its clock is kept");
    /* Десять секунд тишины. Первый пакет после неё меряет от себя, а не от
     * давнего подтверждения, и раньше него на путь ничего не выйдет: начало
     * отрезка известно точно. */
    now += 10u * SECOND;
    const uint32_t first = MeterSend(now - 500u, now);
    ExpectEqual(meter.stamps[first].firstSentTime, now - 500u, "departures restart");
    ExpectEqual(meter.stamps[first].deliveredTime, now, "and so does the delivery clock");
    Expect(meter.stamps[first].deliveredAge == 0u && !meter.stamps[first].deliveredOpen,
           "which is exact again");
    const uint32_t second = MeterSend(now, now);
    ExpectEqual(meter.stamps[second].firstSentTime, now - 500u, "the flight shares its start");
    sample = MeterAckRange(first, second, now + 40u * MS, now + 40u * MS, 0u);
    ExpectEqual(sample.interval, 40u * MS, "the restart is timed by the round trip");
    ExpectEqual(sample.rate, 2000u * SECOND / (40u * MS), "not by the silence");
    ExpectEqual(sample.proven, sample.rate, "and proves all of it");

    /* Подтверждение разобрано на 7 мс позже, чем ушло бы по часам
     * получателя: через тишину это отставание сохраняется. */
    MeterBegin();
    now = SECOND;
    MeterSend(now, now);
    (void)MeterAckRange(0u, 0u, now + 33u * MS, now + 40u * MS, 0u);
    now += 10u * SECOND;
    const uint32_t late = MeterSend(now, now);
    ExpectEqual(meter.stamps[late].deliveredTime, now - 7u * MS,
                "the receiver clock keeps its lag through the silence");

    /* Первый пакет после тишины потерян, и следующий снова уходит в пустой
     * путь: тишина до первого второй раз не считается. */
    MeterBegin();
    now = SECOND;
    MeterSend(now, now);
    (void)MeterAckRange(0u, 0u, now + 40u * MS, now + 40u * MS, 0u);
    now += 10u * SECOND;
    const uint32_t lost = MeterSend(now, now);
    ExpectEqual(meter.stamps[lost].deliveredTime, now, "the delivery clock restarts");
    meter.inFlight -= 1000u;
    now += 5u * SECOND;
    const uint32_t again = MeterSend(now, now);
    ExpectEqual(meter.stamps[again].deliveredTime, now, "and restarts again from there");
}

static void TestRateLossAndFlight(void)
{
    /* Потери за полёт пакета. */
    MeterBegin();
    uint64_t now = SECOND;
    for (uint32_t index = 0u; index < 10u; ++index)
        MeterSend(now, now);
    meter.rate.lost += 2000u;
    meter.inFlight -= 2000u;
    const uint32_t after = MeterSend(now + MS, now + MS);
    ExpectEqual(meter.stamps[after].lost, 2000u, "a packet remembers the losses before it");
    meter.rate.lost += 1000u;
    meter.inFlight -= 1000u;
    NetRateSample sample = MeterAckRange(after, after, now + 40u * MS, now + 40u * MS, 0u);
    ExpectEqual(sample.lost, 1000u, "and the sample counts those during its flight");
    ExpectEqual(sample.stamp.inFlight, 9000u, "against the flight it was sent into");

    /* Один кадр подтверждает пакеты двух пачек. Самый новый ушёл, когда в
     * пути было мало, и по нему одному объём был бы занижен. */
    MeterBegin();
    now = SECOND;
    for (uint32_t index = 0u; index < 30u; ++index)
        MeterSend(now, now);
    sample = MeterAckRange(0u, 19u, now + 40u * MS, now + 40u * MS, 0u);
    ExpectEqual(sample.flight, 20000u, "the first frame delivers twenty packets");
    for (uint32_t index = 0u; index < 5u; ++index)
        MeterSend(now + 40u * MS, now + 40u * MS);
    sample = MeterAckRange(20u, 34u, now + 80u * MS, now + 80u * MS, 0u);
    ExpectEqual(sample.delivered, 15000u, "the newest packet saw fifteen delivered");
    ExpectEqual(sample.flight, 30000u, "but the old burst flew through thirty");

    /* Отрезки кадра разбираются от старших к младшим: замер всё равно даёт
     * самый новый пакет. */
    MeterBegin();
    for (uint32_t index = 0u; index < 6u; ++index)
        MeterSend(now + index * MS, now + index * MS);
    NetRateSample ranges = MeterFrame(now + 50u * MS);
    MeterAck(&ranges, 4u, now + 50u * MS);
    MeterAck(&ranges, 5u, now + 50u * MS);
    MeterAck(&ranges, 0u, now + 50u * MS);
    MeterAck(&ranges, 1u, now + 50u * MS);
    NetRateFinish(&meter.rate, &ranges, 0u);
    Expect(ranges.number == 5u && ranges.sentTime == now + 5u * MS && ranges.acked == 4000u,
           "the newest packet gives the sample whatever the order");
    ExpectEqual(meter.rate.firstSentTime, now + 5u * MS, "and the next departures start at it");
}

/* ---- Правила BBR на заданных подтверждениях ----------------------------------------- */

/* Соединение, которому подтверждения и потери диктует тест. Оборот 40 мс. */
static struct
{
    NetBbr bbr;
    uint64_t now;
    uint64_t delivered;
    /* На столько скорость, которую путь заведомо выдал, ниже скорости
     * доставки в подтверждениях скрипта. */
    uint64_t withheld;
    /* Когда отправлены подтверждаемые пакеты; 0 — оборот назад. */
    uint64_t sentAt;
} script;

/* Зерно случайных пауз между пробами полосы. */
static uint64_t scriptSeed = 7u;

#define SCRIPT_RTT (UINT64_C(40) * MS)

/* Темп при скорости rate и множителе gain процентов: на процент ниже. */
static uint64_t Pace(uint64_t rate, uint64_t gain)
{
    return rate * gain * 99u / 10000u;
}

/* Скорость доставки в счёт короткой модели: темп ниже оценки на процент, и
 * путь, доставивший всё в этом темпе, выдал то, что от него просили. */
static uint64_t Credit(uint64_t rate)
{
    return rate * 100u / 99u;
}

/* Соединение началось оборот назад: первые подтверждения скрипта относятся к
 * пакетам, отправленным уже по этому пути. */
static void ScriptBegin(void)
{
    memset(&script, 0, sizeof(script));
    script.now = SECOND;
    NetBbrReset(&script.bbr, DATAGRAM, WINDOW, scriptSeed, script.now - SCRIPT_RTT);
}

/* Подтверждение acked байт с замером rate. roundStart — подтверждён пакет,
 * отправленный только что, и оборот пакетов кончается; иначе пакет отправлен
 * до начала нынешнего оборота. */
static void ScriptAckFull(uint64_t rate, uint64_t acked, uint64_t inFlight, bool roundStart,
                          bool appLimited, uint64_t lost, uint64_t txInFlight)
{
    uint64_t prior = script.delivered;
    if (!roundStart)
    {
        Expect(script.bbr.nextRoundDelivered != 0u, "the script is past its first acknowledgement");
        prior = script.bbr.nextRoundDelivered - 1u;
    }
    script.delivered += acked;
    const NetBbrAck ack = {
        .now = script.now,
        .delivered = script.delivered,
        .inFlight = inFlight,
        .priorDelivered = prior,
        .sampleDelivered = txInFlight,
        .lost = lost,
        .rate = rate,
        .proven = rate > script.withheld ? rate - script.withheld : 0u,
        .txInFlight = txInFlight,
        .acked = acked,
        .sentTime = script.sentAt != 0u ? script.sentAt : script.now - SCRIPT_RTT,
        .rttSample = SCRIPT_RTT,
        .smoothedRtt = SCRIPT_RTT,
        .appLimited = appLimited,
    };
    NetBbrOnAck(&script.bbr, &ack);
}

static void ScriptAck(uint64_t rate, uint64_t acked, uint64_t inFlight, bool roundStart)
{
    ScriptAckFull(rate, acked, inFlight, roundStart, false, 0u, inFlight);
}

/* Оборот: восемь подтверждений через равные промежутки, первое начинает
 * оборот пакетов. Доставлено за оборот ровно столько, сколько даёт rate. */
static void ScriptRound(uint64_t rate, uint64_t inFlight)
{
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        script.now += SCRIPT_RTT / 8u;
        ScriptAck(rate, rate / 200u, inFlight, index == 0u);
    }
}

static bool ScriptLose(uint64_t txInFlight, uint64_t lost, bool contiguous)
{
    const NetBbrLoss loss = {
        .now = script.now,
        .sentTime = script.now - SCRIPT_RTT / 2u,
        .txInFlight = txInFlight,
        .lost = lost,
        .size = DATAGRAM,
        .contiguous = contiguous,
    };
    return NetBbrOnLoss(&script.bbr, &loss);
}

/* Доводит соединение до плато: поиск полосы кончен, в пути два объёма пути. */
static void ScriptToDrain(uint64_t rate)
{
    ScriptBegin();
    NetBbrSeed(&script.bbr, SCRIPT_RTT);
    for (uint32_t round = 0u; round < 4u; ++round)
        ScriptRound(rate, 2u * (rate / 25u));
    Expect(script.bbr.state == NET_BBR_DRAIN, "the script leaves startup on a plateau");
}

/* Доводит соединение до ровного хода с полосой rate: очередь ушла. */
static void ScriptToProbeBw(uint64_t rate)
{
    ScriptToDrain(rate);
    script.now += SCRIPT_RTT / 8u;
    ScriptAck(rate, rate / 200u, rate / 25u, false);
    Expect(script.bbr.state == NET_BBR_PROBE_CRUISE, "and drains into the probing cycle");
}

/* То же, но очередь не ушла и за три оборота: цикл проб начинается со
 * спуска. */
static void ScriptToProbeDown(uint64_t rate)
{
    ScriptToDrain(rate);
    for (uint32_t round = 0u; round < 3u; ++round)
        ScriptRound(rate, 2u * (rate / 25u));
    Expect(script.bbr.state == NET_BBR_DRAIN, "three rounds of draining are allowed");
    script.now += SCRIPT_RTT / 8u;
    ScriptAck(rate, rate / 200u, 2u * (rate / 25u), true);
    Expect(script.bbr.state == NET_BBR_PROBE_DOWN, "the fourth gives up on the queue");
}

/* Срок следующей пробы вышел: путь наполняется оборот, затем проба. */
static void ScriptNextProbe(uint64_t rate, uint64_t inFlight)
{
    script.now += 3u * SECOND;
    ScriptRound(rate, inFlight);
    Expect(script.bbr.state == NET_BBR_PROBE_REFILL, "the script refills the path");
    ScriptRound(rate, inFlight);
    Expect(script.bbr.state == NET_BBR_PROBE_UP, "and probes");
}

/* Доводит соединение до пробы полосы вверх. */
static void ScriptToProbeUp(uint64_t rate)
{
    ScriptToProbeBw(rate);
    ScriptNextProbe(rate, rate / 25u);
}

/* Проба кончается тремя оборотами плато; в пути остаётся её очередь. */
static void ScriptEndProbe(uint64_t rate, uint64_t inFlight)
{
    for (uint32_t round = 0u; round < 3u && script.bbr.state == NET_BBR_PROBE_UP; ++round)
        ScriptRound(rate, inFlight);
    Expect(script.bbr.state == NET_BBR_PROBE_DOWN, "three plateau rounds end the probe");
}

/* Проба с известным пределом объёма: прошлая встретила потери выше порога.
 * Возвращает предел. */
static uint64_t ScriptToLimitedProbe(void)
{
    ScriptToProbeUp(2400000u);
    script.now += MS;
    /* Сто пакетов в пути, потерян третий из них. */
    (void)ScriptLose(120000u, 3600u, false);
    Expect(script.bbr.state == NET_BBR_PROBE_DOWN && script.bbr.probeTooHigh,
           "three percent of the flight end the probe");
    const uint64_t limit = script.bbr.inflightLong;
    ScriptRound(2400000u, 80000u);
    Expect(script.bbr.state == NET_BBR_PROBE_CRUISE, "the flow cruises under the limit");
    ScriptNextProbe(2400000u, 80000u);
    return limit;
}

static void TestStartupRules(void)
{
    ScriptBegin();
    NetBbr *bbr = &script.bbr;
    Expect(bbr->state == NET_BBR_STARTUP && bbr->window == WINDOW && bbr->pacingRate == 0u &&
               bbr->pacingGain == 277u && bbr->windowGain == 200u,
           "a connection starts in startup, unpaced until the round trip is known");
    NetBbrSeed(bbr, 0u);
    ExpectEqual(bbr->pacingRate, 0u, "no round trip seeds no pace");
    NetBbrSeed(bbr, SCRIPT_RTT);
    /* 2,77 начального окна за оборот: 12000 * 25 * 2,77. */
    ExpectEqual(bbr->pacingRate, 831000u, "the first pace spreads the initial window");
    NetBbrSeed(bbr, SCRIPT_RTT / 2u);
    ExpectEqual(bbr->pacingRate, 831000u, "and is seeded once");
    ExpectEqual(NetBbrQuantum(bbr), 2u * DATAGRAM, "a slow pace sends two datagrams at once");

    /* Первый замер ниже начального темпа его не снижает: полоса ещё не
     * найдена. Замер выше — поднимает до 2,77 скорости без процента. По
     * первому замеру модели хватило бы окна меньше начального, но, пока
     * начальное окно не доставлено, окно растёт с каждым подтверждением. */
    script.now += SCRIPT_RTT;
    ScriptAck(50000u, 2400u, 12000u, true);
    ExpectEqual(bbr->maxBw, 50000u, "the first sample is the bandwidth");
    ExpectEqual(bbr->pacingRate, 831000u, "but the pace only rises in startup");
    ExpectEqual(bbr->minRtt, SCRIPT_RTT, "the round trip is measured");
    ExpectEqual(bbr->window, WINDOW + 2400u, "the window grows by what is acknowledged");
    script.now += MS;
    ScriptAck(400000u, 1200u, 12000u, false);
    ExpectEqual(bbr->pacingRate, Pace(400000u, 277u), "a higher sample raises the pace");

    /* Рост на четверть за оборот продолжает поиск; три оборота без него
     * кончают его. Замер, ограниченный приложением, и подтверждение посреди
     * оборота не считаются. */
    ScriptRound(800000u, 60000u);
    ScriptRound(1600000u, 120000u);
    Expect(bbr->fullBw == 1600000u && bbr->fullBwCount == 0u, "growth resets the plateau count");
    ScriptRound(1999999u, 120000u);
    ExpectEqual(bbr->fullBwCount, 1u, "growth under a quarter is a plateau round");
    script.now += MS;
    ScriptAck(1900000u, 1200u, 120000u, false);
    ExpectEqual(bbr->fullBwCount, 1u, "acknowledgements inside a round do not count");
    script.now += MS;
    ScriptAckFull(1900000u, 1200u, 120000u, true, true, 0u, 120000u);
    ExpectEqual(bbr->fullBwCount, 1u, "an application-limited round does not count");
    ScriptRound(2000000u, 120000u);
    Expect(bbr->fullBw == 2000000u && bbr->fullBwCount == 0u,
           "a quarter above the baseline restarts the search");
    ScriptRound(2400000u, 240000u);
    ScriptRound(2400000u, 240000u);
    Expect(bbr->state == NET_BBR_STARTUP && !bbr->fullBwReached,
           "two plateau rounds are not enough");
    script.now += SCRIPT_RTT;
    ScriptAck(2400000u, 1200u, 240000u, true);
    Expect(bbr->state == NET_BBR_DRAIN && bbr->fullBwReached && bbr->pacingGain == 50u,
           "the third ends startup and drains at half the rate");
    ExpectEqual(bbr->pacingRate, Pace(2400000u, 50u), "the pace drops at once");

    /* Очередь ушла, когда в пути не больше объёма пути: 2,4 МБ/с на 40 мс —
     * 96000 байт. Спускать больше нечего, и соединение сразу идёт ровно. */
    script.now += MS;
    ScriptAck(2400000u, 1200u, 96001u, false);
    Expect(bbr->state == NET_BBR_DRAIN, "a queue still stands above the path volume");
    script.now += MS;
    ScriptAck(2400000u, 1200u, 96000u, false);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE && bbr->pacingGain == 100u &&
               bbr->cycleStamp == script.now,
           "at the path volume the probing cycle starts with nothing left to drain");
    ExpectEqual(bbr->pacingRate, Pace(2400000u, 100u), "a percent under the bandwidth");

    /* Очередь, которая не уходит три оборота, значит завышенную оценку:
     * исправит её цикл проб, и начинается он со спуска. */
    ScriptToProbeDown(2400000u);
    Expect(bbr->pacingGain == 90u && bbr->roundCount == 8u,
           "the probing cycle starts on the way down");
}

static void TestStartupLossRules(void)
{
    /* Серий потерь меньше шести, либо потери не выше двух процентов, либо
     * соединение не в восстановлении: поиск полосы продолжается. */
    for (uint32_t variant = 0u; variant < 5u; ++variant)
    {
        ScriptBegin();
        NetBbr *bbr = &script.bbr;
        NetBbrSeed(bbr, SCRIPT_RTT);
        ScriptRound(1000000u, 60000u);
        ScriptRound(2000000u, 120000u);
        script.now += MS;
        const uint32_t losses = variant == 1u ? 5u : 6u;
        for (uint32_t index = 0u; index < losses; ++index)
        {
            const bool episode =
                ScriptLose(120000u, (index + 1u) * DATAGRAM, variant == 2u && index != 0u);
            Expect(episode == (index == 0u), "the first loss of a flight starts the recovery");
        }
        ExpectEqual(bbr->lossEvents,
                    variant == 1u   ? 5u
                    : variant == 2u ? 1u
                                    : 6u,
                    "loss runs are counted, not packets");
        if (variant == 4u)
            NetBbrUndo(bbr);
        /* Оборот потерь — не меньше восьми замеров после первой потери.
         * Скорость всё это время удваивается: плато поиск не кончает. */
        for (uint32_t index = 0u; index < 7u; ++index)
        {
            script.now += MS;
            ScriptAckFull(UINT64_C(4000000) << index, 1200u, 120000u, true, false, 7200u, 240000u);
            Expect(!bbr->fullBwReached, "a loss round is not over before eight samples");
        }
        script.now += MS;
        /* 7200 из 240000 — три процента; из 360000 — ровно два. */
        ScriptAckFull(UINT64_C(4000000) << 6u, 1200u, 120000u, true, false, 7200u,
                      variant == 3u ? 360000u : 240000u);
        if (variant == 0u)
        {
            Expect(bbr->fullBwReached && bbr->state != NET_BBR_STARTUP,
                   "six loss runs above two percent end startup");
            /* Предел — объём пути по найденной скорости: 256 МБ/с на 40 мс. */
            ExpectEqual(bbr->inflightLong, 10240000u, "the path volume becomes the flight limit");
        }
        else
        {
            Expect(!bbr->fullBwReached && bbr->state == NET_BBR_STARTUP &&
                       bbr->inflightLong == UNLIMITED,
                   "fewer runs, lower loss or no recovery keep startup");
        }
    }

    /* Шесть серий за два оборота потерь — не шесть за один: каждый оборот
     * потерь считает свои. */
    ScriptBegin();
    NetBbr *bbr = &script.bbr;
    NetBbrSeed(bbr, SCRIPT_RTT);
    ScriptRound(1000000u, 60000u);
    ScriptRound(2000000u, 120000u);
    for (uint32_t half = 0u; half < 2u; ++half)
    {
        script.now += MS;
        for (uint32_t index = 0u; index < 3u; ++index)
            (void)ScriptLose(120000u, (index + 1u) * DATAGRAM, false);
        for (uint32_t index = 0u; index < 8u; ++index)
        {
            script.now += MS;
            ScriptAckFull(UINT64_C(4000000) << (half * 8u + index), 1200u, 120000u, true, false,
                          7200u, 240000u);
        }
        ExpectEqual(bbr->lossEvents, 0u, "a loss round ends with its runs counted");
    }
    Expect(bbr->state == NET_BBR_STARTUP && !bbr->fullBwReached && bbr->inRecovery,
           "three runs in each of two loss rounds keep startup");

    /* Шесть серий выше порога, но восстановление кончилось раньше оборота
     * потерь: первым подтверждён пакет, отправленный после них. */
    ScriptBegin();
    NetBbrSeed(bbr, SCRIPT_RTT);
    ScriptRound(1000000u, 60000u);
    ScriptRound(2000000u, 120000u);
    script.now += MS;
    for (uint32_t index = 0u; index < 6u; ++index)
        (void)ScriptLose(120000u, (index + 1u) * DATAGRAM, false);
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        script.now += MS;
        script.sentAt = index == 0u ? script.now - MS / 2u : 0u;
        ScriptAckFull(UINT64_C(4000000) << index, 1200u, 120000u, true, false, 7200u, 240000u);
    }
    Expect(!bbr->inRecovery && bbr->state == NET_BBR_STARTUP && !bbr->fullBwReached,
           "losses whose recovery has ended keep startup");
}

static void TestProbeCycleRules(void)
{
    /* 2,4 МБ/с и 40 мс: объём пути — 96000 байт. */
    ScriptToProbeDown(2400000u);
    NetBbr *bbr = &script.bbr;
    const uint64_t entered = script.now;
    Expect(bbr->probeWait >= 2u * SECOND && bbr->probeWait < 3u * SECOND &&
               bbr->cycleStamp == entered,
           "the next probe waits two to three seconds");
    Expect(bbr->roundsSinceProbe <= 1u, "and its round count starts at zero or one");
    ExpectEqual(bbr->pacingRate, Pace(2400000u, 90u), "the way down is paced at 0.9");
    ExpectEqual(bbr->window, 192000u + bbr->extraAcked,
                "the window is twice the path volume and the aggregation");
    /* Дальше срок не случаен: от розыгрыша тест не зависит. */
    bbr->probeWait = 2u * SECOND;

    /* В пути больше объёма пути: вниз. Столько же: ровный ход. */
    script.now += MS;
    ScriptAck(2400000u, 1200u, 96001u, false);
    Expect(bbr->state == NET_BBR_PROBE_DOWN, "the way down lasts while a queue stands");
    script.now += MS;
    ScriptAck(2400000u, 1200u, 96000u, false);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE && bbr->pacingGain == 100u, "then the flow cruises");
    ExpectEqual(bbr->pacingRate, Pace(2400000u, 100u), "a percent under the bandwidth");

    /* До срока проба не начинается, после — путь наполняется, и через
     * оборот идёт проба: темп 1,25, окно 2,25. */
    while (script.now + SCRIPT_RTT <= entered + bbr->probeWait)
    {
        ScriptRound(2400000u, 96000u);
        Expect(bbr->state == NET_BBR_PROBE_CRUISE, "cruising lasts until the wait is over");
    }
    ScriptRound(2400000u, 96000u);
    Expect(bbr->state == NET_BBR_PROBE_REFILL && bbr->pacingGain == 100u,
           "after the wait the path is refilled");
    ScriptRound(2400000u, 96000u);
    Expect(bbr->state == NET_BBR_PROBE_UP && bbr->pacingGain == 125u && bbr->windowGain == 225u,
           "for a round, and then probed");
    ExpectEqual(bbr->pacingRate, Pace(2400000u, 125u), "at a quarter above");
    Expect(bbr->fullBw == 2400000u && bbr->fullBwCount == 0u && bbr->checkProbeLoss,
           "the probe looks for growth from its own start and for its own losses");
    /* Три оборота без роста на четверть кончают пробу. В пути её очередь, и
     * соединение идёт вниз. */
    ScriptRound(2500000u, 120000u);
    ScriptRound(2500000u, 120000u);
    Expect(bbr->state == NET_BBR_PROBE_UP, "two rounds without growth keep probing");
    ScriptRound(2500000u, 120000u);
    Expect(bbr->state == NET_BBR_PROBE_DOWN && bbr->maxBw == 2500000u,
           "the third ends the probe and keeps what it found");
    Expect(bbr->checkProbeLoss && bbr->probeStopping, "the losses of the probe may still arrive");
    ScriptRound(2500000u, 120000u);
    Expect(!bbr->checkProbeLoss && !bbr->probeStopping, "a round later they cannot");
}

static void TestProbeWaitSpread(void)
{
    /* Срок до пробы — две секунды и случайная доля третьей, счёт оборотов
     * начинается с нуля или единицы: пробы потоков одного узкого места
     * расходятся. */
    const uint64_t seed = scriptSeed;
    uint64_t shortest = UNLIMITED;
    uint64_t longest = 0u;
    uint32_t odd = 0u;
    for (uint32_t draw = 0u; draw < 64u; ++draw)
    {
        scriptSeed = seed + draw;
        ScriptToProbeDown(2400000u);
        const NetBbr *bbr = &script.bbr;
        ExpectBetween(bbr->probeWait, 2u * SECOND, 3u * SECOND - 1u,
                      "the wait is two to three seconds");
        Expect(bbr->roundsSinceProbe <= 1u, "the round count starts at zero or one");
        shortest = bbr->probeWait < shortest ? bbr->probeWait : shortest;
        longest = bbr->probeWait > longest ? bbr->probeWait : longest;
        odd += bbr->roundsSinceProbe;
    }
    scriptSeed = seed;
    Expect(shortest < 2u * SECOND + 100u * MS && longest > 3u * SECOND - 100u * MS,
           "and spreads over the whole third second");
    ExpectBetween(odd, 16u, 48u, "both round counts are drawn");
}

static void TestRenoCoexistence(void)
{
    /* Объём пути — восемь датаграмм: проба идёт через столько оборотов,
     * сколько понадобилось бы Reno, а не через две секунды. */
    ScriptToProbeBw(240000u);
    NetBbr *bbr = &script.bbr;
    const uint64_t started = script.now;
    uint32_t rounds = 0u;
    while (bbr->state == NET_BBR_PROBE_CRUISE && rounds < 100u)
    {
        ScriptRound(240000u, 9600u);
        ++rounds;
    }
    Expect(bbr->state == NET_BBR_PROBE_REFILL, "the small flow probes");
    ExpectBetween(rounds, 7u, 8u, "after as many rounds as its window has datagrams");
    Expect(script.now - started < SECOND, "long before the two seconds");

    /* У широкого потока оборотов не больше 63. Срок по часам отодвинут:
     * виден один счёт оборотов. */
    ScriptToProbeBw(24000000u);
    bbr->probeWait = 3600u * SECOND;
    rounds = 0u;
    while (bbr->state == NET_BBR_PROBE_CRUISE && rounds < 100u)
    {
        ScriptRound(24000000u, 960000u);
        ++rounds;
    }
    Expect(bbr->state == NET_BBR_PROBE_REFILL, "a wide flow probes by rounds too");
    ExpectBetween(rounds, 62u, 63u, "after 63 of them at the latest");
}

static void TestProbeLossRules(void)
{
    /* Потери пробы не выше двух процентов её не прерывают. */
    ScriptToProbeUp(2400000u);
    NetBbr *bbr = &script.bbr;
    script.now += MS;
    Expect(ScriptLose(120000u, 2400u, false), "a loss in the probe starts a recovery");
    Expect(bbr->state == NET_BBR_PROBE_UP && bbr->inflightLong == UNLIMITED && !bbr->probeTooHigh,
           "two percent of the flight is tolerated");
    /* Выше — прерывают. Предел — объём, на котором доля перешла порог:
     * 99 пакетов в пути, потерян второй из них. До него в пути 117600 байт
     * и потеряно 1200; ещё 1175 байт этого пакета доводят долю до двух
     * процентов. */
    ScriptToProbeUp(2400000u);
    script.now += MS;
    (void)ScriptLose(118800u, 1200u, false);
    Expect(bbr->state == NET_BBR_PROBE_UP, "one loss in 99 packets is under the threshold");
    (void)ScriptLose(118800u, 2400u, false);
    Expect(bbr->state == NET_BBR_PROBE_DOWN && bbr->probeTooHigh && !bbr->checkProbeLoss,
           "the second ends the probe");
    ExpectEqual(bbr->nextRoundDelivered, script.delivered, "and the way down starts a round");
    ExpectEqual(bbr->inflightLong, 118775u, "the limit is where the loss rate crossed");
    /* На одну пробу — один ответ. */
    (void)ScriptLose(60000u, 6000u, false);
    ExpectEqual(bbr->inflightLong, 118775u, "later losses of the same probe change nothing");

    /* Предел не падает ниже 0,7 того, что соединение держало. */
    ScriptToProbeUp(2400000u);
    script.now += MS;
    (void)ScriptLose(24000u, 2400u, false);
    ExpectEqual(bbr->inflightLong, 96000u * 7u / 10u, "the limit keeps 0.7 of the target");

    /* Пакет, ограниченный приложением, предела не ставит, но пробу кончает. */
    ScriptToProbeUp(2400000u);
    script.now += MS;
    const NetBbrLoss limited = {
        .now = script.now,
        .sentTime = script.now - MS,
        .txInFlight = 24000u,
        .lost = 2400u,
        .size = DATAGRAM,
        .appLimited = true,
    };
    (void)NetBbrOnLoss(bbr, &limited);
    Expect(bbr->state == NET_BBR_PROBE_DOWN && bbr->inflightLong == UNLIMITED && bbr->probeTooHigh,
           "an application-limited flight says nothing about the path volume");

    /* Вне пробы потери предела не трогают. */
    ScriptToProbeBw(2400000u);
    script.now += MS;
    (void)ScriptLose(24000u, 6000u, false);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE && bbr->inflightLong == UNLIMITED &&
               !bbr->probeTooHigh,
           "losses outside a probe do not set the limit");
}

static void TestShortTermModel(void)
{
    /* Путь стал держать меньше. Оборот с потерями вне пробы опускает
     * короткую модель к тому, что путь за этот оборот доставил, но не ниже
     * 0,7 прежнего: скорость — к замеру с поправкой на запас темпа, объём — к
     * 0,7 окна. */
    ScriptToProbeBw(2400000u);
    NetBbr *bbr = &script.bbr;
    const uint64_t window = bbr->window;
    ScriptRound(2000000u, 90000u);
    Expect(bbr->bwShort == UNLIMITED && bbr->inflightShort == UNLIMITED && bbr->window == window,
           "a slower round without losses bounds nothing");
    script.now += MS;
    (void)ScriptLose(90000u, 1200u, false);
    ScriptRound(2000000u, 90000u);
    ExpectEqual(bbr->bwShort, Credit(2000000u),
                "the delivery rate of the loss round bounds the bandwidth");
    ExpectEqual(bbr->inflightShort, window * 7u / 10u, "and 0.7 of the window bounds the flight");
    ExpectEqual(bbr->bw, Credit(2000000u), "the model takes the lower bandwidth");
    ExpectEqual(bbr->pacingRate, Pace(Credit(2000000u), 100u), "and paces by it");
    ExpectEqual(bbr->window, window * 7u / 10u, "the window follows the flight bound");
    ExpectEqual(bbr->maxBw, 2400000u, "the long-term bandwidth is untouched");
    /* Замеры намного ниже: не больше чем на 0,7 за оборот. */
    ScriptRound(500000u, 20000u);
    ExpectEqual(bbr->bwShort, Credit(2000000u), "a round without losses keeps the bound");
    script.now += MS;
    (void)ScriptLose(20000u, 1200u, false);
    ScriptRound(500000u, 20000u);
    ExpectEqual(bbr->bwShort, Credit(2000000u) * 7u / 10u, "one loss round takes at most 0.7");
    ExpectEqual(bbr->inflightShort, window * 7u / 10u * 7u / 10u, "from the flight as well");
    /* Следующая проба короткую модель забывает. */
    script.now += 3u * SECOND;
    ScriptRound(500000u, 20000u);
    Expect(bbr->state == NET_BBR_PROBE_REFILL && bbr->bwShort == UNLIMITED &&
               bbr->inflightShort == UNLIMITED && bbr->bw == 2400000u,
           "refilling forgets the short-term model");

    /* Оборот потерь при редких подтверждениях длится восемь замеров: один
     * шумный замер модель не роняет. */
    ScriptToProbeBw(2400000u);
    ScriptRound(1500000u, 96000u);
    script.now += MS;
    (void)ScriptLose(96000u, 1200u, false);
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        script.now += SCRIPT_RTT;
        ScriptAck(index == 3u ? 2300000u : 1500000u, 12000u, 96000u, true);
        Expect(bbr->bwShort == UNLIMITED, "seven samples do not end a loss round");
    }
    script.now += SCRIPT_RTT;
    ScriptAck(1500000u, 12000u, 96000u, true);
    ExpectEqual(bbr->bwShort, Credit(2300000u), "the best of eight samples bounds the bandwidth");

    /* Поток, доставляющий всё в своём темпе, полосы не теряет, сколько бы
     * оборотов с потерями ни шло подряд. Без поправки на запас темпа каждый
     * из них опускал бы модель ещё на процент. */
    ScriptToProbeBw(2400000u);
    for (uint32_t round = 0u; round < 20u; ++round)
    {
        script.now += MS;
        (void)ScriptLose(96000u, 1200u, false);
        ScriptRound(bbr->pacingRate, 96000u);
    }
    Expect(bbr->state == NET_BBR_PROBE_CRUISE && bbr->bwShort != UNLIMITED,
           "twenty loss rounds bound the bandwidth");
    ExpectEqual(bbr->bw, 2400000u, "but a flow delivering at its own pace keeps all of it");

    /* Оборот потерь кончается подтверждением пакета, отправленного после его
     * начала, и не раньше восьми замеров. Объём доставки за него — наибольший
     * из его замеров, считая замер, его начавший. */
    ScriptToProbeBw(2400000u);
    for (uint32_t index = 0u; index < 16u && !bbr->lossRoundStart; ++index)
    {
        script.now += SCRIPT_RTT / 8u;
        ScriptAck(2400000u, 12000u, 60000u, true);
    }
    Expect(bbr->lossRoundStart, "a loss round starts");
    ExpectEqual(bbr->lossRoundDelivered, script.delivered, "and waits for packets sent after it");
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        script.now += SCRIPT_RTT / 8u;
        ScriptAck(2400000u, 12000u, 60000u, true);
        Expect(!bbr->lossRoundStart, "seven samples do not end it");
    }
    script.now += SCRIPT_RTT / 8u;
    ScriptAck(2400000u, 12000u, 150000u, true);
    Expect(bbr->lossRoundStart, "the eighth starts the next");
    script.now += MS;
    (void)ScriptLose(150000u, 1200u, false);
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        script.now += SCRIPT_RTT / 8u;
        ScriptAck(2400000u, 12000u, 60000u, true);
    }
    ExpectEqual(bbr->inflightShort, 150000u,
                "the largest flight of the loss round, its first included, bounds the flight");

    /* Наполняя путь перед пробой, соединение шлёт быстрее нарочно: оборот с
     * потерями, кончившийся в это время, короткой модели не трогает. */
    ScriptToProbeBw(2400000u);
    script.now += 3u * SECOND;
    ScriptAck(2400000u, 12000u, 96000u, true);
    Expect(bbr->state == NET_BBR_PROBE_REFILL, "the probe is due");
    script.now += MS;
    (void)ScriptLose(96000u, 1200u, false);
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        script.now += MS;
        ScriptAck(2400000u, 1200u, 96000u, false);
    }
    script.now += MS;
    ScriptAck(2400000u, 1200u, 96000u, true);
    Expect(bbr->state == NET_BBR_PROBE_UP && bbr->bwShort == UNLIMITED &&
               bbr->inflightShort == UNLIMITED,
           "a loss round of the refill bounds nothing");
}

static void TestProvenRate(void)
{
    /* Скорость доставки бывает выше пути на датаграмму за оборот. О полосе
     * говорит скорость, которую путь заведомо выдал; за тем, что он доставил
     * в последний оборот, следит скорость доставки. */
    ScriptBegin();
    NetBbr *bbr = &script.bbr;
    NetBbrSeed(bbr, SCRIPT_RTT);
    script.withheld = 30000u;
    script.now += SCRIPT_RTT;
    ScriptAck(1000000u, 2400u, 12000u, true);
    ExpectEqual(bbr->maxBw, 970000u, "the estimate takes what the path has proven");
    ExpectEqual(bbr->bwLatest, Credit(1000000u), "the latest delivery rate is kept as measured");
    ExpectEqual(bbr->pacingRate, Pace(970000u, 277u), "and the pace follows the estimate");

    /* Замер, ограниченный приложением, говорит о пути, только если заведомая
     * скорость выше оценки. Цикл проб свежий: своих замеров у него ещё нет. */
    ScriptToProbeBw(2400000u);
    script.now += SCRIPT_RTT;
    ScriptAck(2400000u, 1200u, 96000u, true);
    ExpectEqual(bbr->maxBwSlots[bbr->cycle], 0u, "a new cycle has no samples yet");
    script.withheld = 30000u;
    script.now += MS;
    ScriptAckFull(2410000u, 1200u, 96000u, false, true, 0u, 96000u);
    ExpectEqual(bbr->maxBwSlots[bbr->cycle], 0u,
                "a limited sample that proves less than the estimate says nothing");
    script.now += MS;
    ScriptAckFull(2440000u, 1200u, 96000u, false, true, 0u, 96000u);
    ExpectEqual(bbr->maxBw, 2410000u, "one that proves more raises it");

    /* Оборот с потерями: короткая модель идёт к скорости доставки, и
     * следующий оборот начинает с неё же. */
    ScriptToProbeBw(2400000u);
    script.withheld = 30000u;
    ScriptRound(1500000u, 96000u);
    script.now += MS;
    (void)ScriptLose(96000u, 1200u, false);
    for (uint32_t index = 0u; index < 7u; ++index)
    {
        script.now += SCRIPT_RTT;
        ScriptAck(2000000u, 12000u, 96000u, true);
    }
    script.now += SCRIPT_RTT;
    ScriptAck(1500000u, 12000u, 96000u, true);
    ExpectEqual(bbr->bwShort, Credit(2000000u),
                "the short-term bound follows the delivery rate itself");
    ExpectEqual(bbr->bwLatest, Credit(1500000u), "and the next round starts from the latest one");
    ExpectEqual(bbr->maxBw, 2400000u, "while the estimate keeps what was proven");
}

static void TestMaxBwFilter(void)
{
    /* Оценка — наибольший замер двух последних циклов проб. */
    ScriptToProbeUp(2400000u);
    NetBbr *bbr = &script.bbr;
    ScriptRound(3000000u, 120000u);
    ExpectEqual(bbr->maxBw, 3000000u, "a higher sample raises the estimate at once");
    script.now += MS;
    ScriptAckFull(2000000u, 1200u, 120000u, false, true, 0u, 120000u);
    ExpectEqual(bbr->maxBw, 3000000u, "a lower one does not lower it");
    script.now += MS;
    ScriptAckFull(3100000u, 1200u, 120000u, false, true, 0u, 120000u);
    ExpectEqual(bbr->maxBw, 3100000u, "an application-limited sample counts only when higher");
    /* Полоса упала до двух мегабайт. Цикл закрывает первый оборот после
     * пробы: замеры позапрошлого цикла забыты, прошлого — ещё нет. */
    ScriptEndProbe(2000000u, 200000u);
    ScriptRound(2000000u, 80000u);
    ExpectEqual(bbr->maxBw, 3100000u, "a sample outlives the cycle after its own");
    ScriptNextProbe(2000000u, 80000u);
    ScriptEndProbe(2000000u, 200000u);
    script.now += SCRIPT_RTT;
    ScriptAck(2000000u, 1200u, 80000u, true);
    ExpectEqual(bbr->maxBw, 2000000u, "the next cycle forgets it");

    /* Оборот, ограниченный приложением, цикла не закрывает: о пути его
     * замеры не говорят, и прежние живут на цикл дольше. */
    ScriptToProbeUp(2400000u);
    ScriptRound(3000000u, 120000u);
    ScriptEndProbe(2000000u, 200000u);
    script.now += SCRIPT_RTT;
    ScriptAckFull(500000u, 1200u, 80000u, true, true, 0u, 80000u);
    ScriptNextProbe(2000000u, 80000u);
    ScriptEndProbe(2000000u, 200000u);
    script.now += SCRIPT_RTT;
    ScriptAck(2000000u, 1200u, 80000u, true);
    ExpectEqual(bbr->maxBw, 3000000u, "an application-limited round does not close the cycle");

    /* Цикл без единого замера прежних не вытесняет, иначе оценка упала бы
     * до нуля. Все подтверждения цикла ограничены приложением, пробу кончает
     * потеря, а оборот после неё замера скорости не дал. Первый такой цикл
     * закрывает цикл поиска полосы, второй пуст. */
    ScriptToProbeBw(2400000u);
    for (uint32_t cycle = 0u; cycle < 2u; ++cycle)
    {
        script.now += 3u * SECOND;
        for (uint32_t round = 0u; round < 3u; ++round)
        {
            script.now += SCRIPT_RTT;
            ScriptAckFull(500000u, 1200u, 40000u, true, true, 0u, 40000u);
        }
        Expect(bbr->state == NET_BBR_PROBE_UP, "an application-limited flow still probes");
        script.now += MS;
        (void)ScriptLose(80000u, 4800u, false);
        Expect(bbr->state == NET_BBR_PROBE_DOWN, "a loss ends its probe");
        script.now += SCRIPT_RTT;
        ScriptAck(0u, 1200u, 40000u, true);
        ExpectEqual(bbr->cycle, 1u, "the cycle of the bandwidth search is closed once");
        ExpectEqual(bbr->maxBw, 2400000u, "a cycle without a single sample evicts nothing");
    }
    ExpectEqual(bbr->pacingRate, Pace(2400000u, 100u), "and the pace stands");
}

static void TestPrecautionaryProbe(void)
{
    /* Проба встретила потери выше порога. Следующая останавливается у
     * прежнего предела, не дожидаясь потерь, и, если их не было, сразу
     * повторяется — уже без оглядки на предел. */
    const uint64_t limit = ScriptToLimitedProbe();
    NetBbr *bbr = &script.bbr;
    ExpectEqual(limit, 118800u, "the limit is the flight before the lost packets");
    Expect(bbr->probeTooHigh, "the next probe remembers the loss");
    script.now += MS;
    ScriptAck(2400000u, 1200u, limit - 1u, false);
    Expect(bbr->state == NET_BBR_PROBE_UP, "below the old limit the probe goes on");
    script.now += MS;
    ScriptAck(2400000u, 1200u, limit, false);
    Expect(bbr->state == NET_BBR_PROBE_DOWN && bbr->probePrecautionary && !bbr->probeTooHigh,
           "at the old limit it stops by itself");
    /* Подтверждения пробы кончились без потерь: проба повторяется сразу. */
    ScriptRound(2400000u, 80000u);
    Expect(bbr->state == NET_BBR_PROBE_REFILL && !bbr->probePrecautionary,
           "a clean precautionary probe is repeated at once");
    ScriptRound(2400000u, 80000u);
    script.now += MS;
    ScriptAck(2400000u, 1200u, limit + 12000u, false);
    Expect(bbr->state == NET_BBR_PROBE_UP, "and this one passes the old limit");
    ExpectEqual(bbr->inflightLong, limit + 12000u, "which a delivered flight raises");
}

static void TestInflightProbing(void)
{
    /* Проба, которой предел объёма уже не страшен: прошлая дошла до него
     * без потерь. */
    const uint64_t limit = ScriptToLimitedProbe();
    NetBbr *bbr = &script.bbr;
    script.now += MS;
    ScriptAck(2400000u, 1200u, limit, false);
    ScriptRound(2400000u, 80000u);
    ScriptRound(2400000u, 80000u);
    Expect(bbr->state == NET_BBR_PROBE_UP && !bbr->probeTooHigh && bbr->inflightLong == limit,
           "the probe is repeated under the same limit");
    ExpectEqual(bbr->window, limit, "which holds the window");
    ExpectEqual(bbr->upAckedPerStep, limit, "the first round raises it by a datagram per window");

    /* Предел растёт, только пока окно в него упирается. */
    script.now += MS;
    ScriptAck(2400000u, 60000u, limit, false);
    ExpectEqual(bbr->inflightLong, limit, "the limit does not grow while the window has room");
    NetBbrOnSend(bbr, limit - 2u * DATAGRAM, DATAGRAM, false, script.now);
    Expect(!bbr->windowFilled, "a window with room for another datagram is not full");
    NetBbrOnSend(bbr, limit - DATAGRAM, DATAGRAM, false, script.now);
    Expect(bbr->windowFilled, "one without it is");
    script.now += MS;
    ScriptAck(2400000u, 60000u, limit, false);
    ExpectEqual(bbr->inflightLong, limit, "half a window acknowledged is not yet a step");
    script.now += MS;
    ScriptAck(2400000u, 60000u, limit, false);
    ExpectEqual(bbr->inflightLong, limit + DATAGRAM, "a window acknowledged is one datagram");
    ExpectEqual(bbr->window, limit + DATAGRAM, "and the window follows the limit");

    /* Окно в предел упирается, а скорость не растёт: проба не кончается,
     * плато тут — не предел пути. Шаг предела вдвое короче с каждым
     * оборотом: окно, половина, четверть, восьмая подтверждённых байт на
     * датаграмму — за три оборота 1, 4 и 6 датаграмм. */
    for (uint32_t round = 0u; round < 3u; ++round)
    {
        NetBbrOnSend(bbr, bbr->window - DATAGRAM, DATAGRAM, false, script.now);
        ScriptRound(2400000u, bbr->window);
    }
    Expect(bbr->state == NET_BBR_PROBE_UP && bbr->fullBwCount == 0u,
           "a plateau under the flight limit is not the bandwidth");
    ExpectEqual(bbr->upRounds, 4u, "every round of the probe doubles the growth");
    const uint64_t raised = bbr->inflightLong;
    ExpectEqual(raised, limit + 12u * DATAGRAM,
                "three rounds raise the limit by 11 datagrams more");

    /* Окно перестало упираться: три оборота плато кончают пробу, и предел
     * остаётся там, куда она его довела. */
    ScriptRound(2400000u, 100000u);
    ScriptRound(2400000u, 100000u);
    Expect(bbr->state == NET_BBR_PROBE_UP, "two plateau rounds keep probing");
    ScriptRound(2400000u, 100000u);
    Expect(bbr->state == NET_BBR_PROBE_DOWN, "without the limit a plateau ends the probe");
    ExpectEqual(bbr->inflightLong, raised, "and the limit stays where the probe left it");

    /* На ровном ходу окно держится под пределом с запасом в 15 процентов. */
    script.now += MS;
    ScriptAck(2400000u, 1200u, 96000u, false);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE, "the flow cruises again");
    ExpectEqual(bbr->window, raised - raised * 15u / 100u,
                "cruising leaves headroom under the limit");

    /* Запас держит и спуск: выше него соединение на ровный ход не выходит.
     * Предел после потерь пробы — 0,7 объёма пути, 67200 байт; с запасом —
     * 57120. */
    ScriptToProbeUp(2400000u);
    script.now += MS;
    (void)ScriptLose(24000u, 2400u, false);
    script.now += MS;
    ScriptAck(2400000u, 1200u, 57121u, false);
    Expect(bbr->state == NET_BBR_PROBE_DOWN, "above the headroom the flow keeps going down");
    script.now += MS;
    ScriptAck(2400000u, 1200u, 57120u, false);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE, "at the headroom it cruises");

    /* Предел ниже объёма пути держит окно, и следующая проба с потерями
     * оставляет 0,7 того, что окно держало, а не объёма пути. */
    ScriptNextProbe(2400000u, 57120u);
    ExpectEqual(bbr->window, 67200u, "the probe fills the window up to the limit");
    script.now += MS;
    (void)ScriptLose(24000u, 2400u, false);
    ExpectEqual(bbr->inflightLong, 67200u * 7u / 10u, "a lossy probe keeps 0.7 of the window");

    /* Окно заполнено, но ниже предела: предел не растёт. */
    ScriptToProbeUp(2400000u);
    bbr->inflightLong = bbr->window + 10u * DATAGRAM;
    const uint64_t roomy = bbr->inflightLong;
    NetBbrOnSend(bbr, bbr->window - DATAGRAM, DATAGRAM, false, script.now);
    script.now += MS;
    ScriptAck(2400000u, 2u * bbr->window, 96000u, false);
    ExpectEqual(bbr->inflightLong, roomy, "a full window under the limit does not raise it");

    /* Путь в пять датаграмм: запас под пределом — целая датаграмма. */
    ScriptToProbeDown(2400000u);
    bbr->inflightLong = 5u * DATAGRAM;
    script.now += MS;
    ScriptAck(2400000u, 1200u, 4u * DATAGRAM + 1u, false);
    Expect(bbr->state == NET_BBR_PROBE_DOWN, "a byte above four datagrams keeps going down");
    script.now += MS;
    ScriptAck(2400000u, 1200u, 4u * DATAGRAM, false);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE && bbr->window == 4u * DATAGRAM,
           "four cruise, a datagram under the limit");
    /* Предел в четыре датаграммы: запас не опускает объём ниже наименьшего
     * окна. */
    ScriptToProbeDown(2400000u);
    bbr->inflightLong = 4u * DATAGRAM;
    script.now += MS;
    ScriptAck(2400000u, 1200u, 4u * DATAGRAM, false);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE, "under a limit of four datagrams four still cruise");
}

static void TestProbeRttRules(void)
{
    ScriptToProbeBw(2400000u);
    NetBbr *bbr = &script.bbr;
    const uint64_t stamp = bbr->minRttStamp;
    ExpectEqual(stamp, SECOND + SCRIPT_RTT / 8u, "the first sample dates the round trip");
    /* Десять секунд без нового наименьшего оборота; пробы полосы идут своим
     * чередом. */
    uint64_t window = 0u;
    for (uint32_t index = 0u; bbr->state != NET_BBR_PROBE_RTT && index < 4000u; ++index)
    {
        Expect(script.now <= stamp + 10u * SECOND, "the round trip is trusted for ten seconds");
        window = bbr->window;
        script.now += SCRIPT_RTT / 8u;
        ScriptAck(2400000u, 12000u, 96000u, index % 8u == 0u);
    }
    Expect(bbr->state == NET_BBR_PROBE_RTT && script.now > stamp + 10u * SECOND &&
               script.now <= stamp + 10u * SECOND + SCRIPT_RTT / 8u,
           "and measured again right after");
    Expect(bbr->pacingGain == 100u && NetBbrProbesRtt(bbr),
           "at the pace of the path, with the rate samples marked as limited");
    ExpectEqual(bbr->window, 48000u, "with half the path volume in flight");
    ExpectEqual(bbr->priorWindow, window, "the window is remembered");
    ExpectEqual(bbr->probeRttDone, 0u, "the measurement waits for the flight to fall");
    script.now += MS;
    ScriptAck(2400000u, 1200u, 48001u, false);
    ExpectEqual(bbr->probeRttDone, 0u, "a byte above half the path volume is not yet it");
    /* Объём упал: замер длится 200 мс и не меньше оборота. Оборот прошёл
     * раньше. */
    script.now += MS;
    ScriptAck(2400000u, 1200u, 48000u, false);
    const uint64_t done = script.now + 200u * MS;
    ExpectEqual(bbr->probeRttDone, done, "it lasts 200 ms from then");
    script.now += 100u * MS;
    ScriptAck(2400000u, 1200u, 48000u, true);
    Expect(bbr->state == NET_BBR_PROBE_RTT, "a round trip alone does not end it");
    script.now = done;
    ScriptAck(2400000u, 1200u, 48000u, false);
    Expect(bbr->state == NET_BBR_PROBE_RTT, "nor does a microsecond less than 200 ms");
    /* Оборот с потерями урезал короткую модель. */
    bbr->bwShort = 2000000u;
    bbr->inflightShort = 150000u;
    script.now += 1u;
    ScriptAck(2400000u, 1200u, 48000u, false);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE, "then the flow cruises");
    ExpectEqual(bbr->minRttStamp, script.now, "with a fresh round trip");
    Expect(bbr->window >= 192000u, "and its old window");
    Expect(bbr->cycleStamp == script.now && bbr->bwShort == UNLIMITED &&
               bbr->inflightShort == UNLIMITED,
           "the probing cycle and the short-term model start over");

    /* Меньший оборот освежает оценку сам, и замер не нужен. */
    ScriptToProbeBw(2400000u);
    script.now += 9u * SECOND;
    const NetBbrAck lower = {
        .now = script.now,
        .delivered = script.delivered + 1200u,
        .inFlight = 96000u,
        .priorDelivered = 0u,
        .sampleDelivered = 96000u,
        .rate = 2400000u,
        .proven = 2400000u,
        .txInFlight = 96000u,
        .acked = 1200u,
        .sentTime = script.now - 30u * MS,
        .rttSample = 30u * MS,
        .smoothedRtt = SCRIPT_RTT,
    };
    script.delivered += 1200u;
    NetBbrOnAck(bbr, &lower);
    Expect(bbr->minRtt == 30u * MS && bbr->minRttStamp == script.now,
           "a lower round trip replaces the estimate");
    script.now += 9u * SECOND;
    ScriptAck(2400000u, 1200u, 72000u, false);
    Expect(bbr->state != NET_BBR_PROBE_RTT && bbr->minRtt == 30u * MS,
           "and is trusted for its own ten seconds");
    /* Подтверждение без замера оборота оценку не трогает, а срок её вышел. */
    script.now += 2u * SECOND;
    NetBbrAck blind = lower;
    blind.now = script.now;
    blind.delivered = script.delivered + 1200u;
    blind.sentTime = script.now - SCRIPT_RTT;
    blind.rttSample = 0u;
    script.delivered += 1200u;
    NetBbrOnAck(bbr, &blind);
    Expect(bbr->state == NET_BBR_PROBE_RTT && bbr->minRtt == 30u * MS,
           "an expired estimate without a sample still asks for the measurement");
    /* Окно замера считается по прежнему обороту: 2,4 МБ/с на 30 мс — 72000
     * байт, половина — 36000. 200 мс прошли раньше оборота. */
    script.now += MS;
    ScriptAck(2400000u, 1200u, 36001u, false);
    ExpectEqual(bbr->minRtt, SCRIPT_RTT, "the expired estimate takes the first sample");
    ExpectEqual(bbr->window, 36000u, "the window is half the old path volume");
    ExpectEqual(bbr->probeRttDone, 0u, "and a byte above it does not start the clock");
    script.now += MS;
    ScriptAck(2400000u, 1200u, 36000u, false);
    ExpectEqual(bbr->probeRttDone, script.now + 200u * MS,
                "half the old path volume starts the clock");
    script.now += 201u * MS;
    ScriptAck(2400000u, 1200u, 36000u, false);
    Expect(bbr->state == NET_BBR_PROBE_RTT, "200 ms alone do not end the measurement");
    script.now += MS;
    ScriptAck(2400000u, 1200u, 36000u, true);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE, "a round trip in it does");

    /* Тишина посреди замера: первая же отправка после него кончает замер. */
    ScriptToProbeBw(2400000u);
    script.now += 11u * SECOND;
    ScriptAck(2400000u, 1200u, 48000u, false);
    Expect(bbr->state == NET_BBR_PROBE_RTT && bbr->probeRttDone == script.now + 200u * MS,
           "the measurement starts");
    NetBbrOnSend(bbr, 0u, DATAGRAM, true, script.now + 200u * MS);
    Expect(bbr->state == NET_BBR_PROBE_RTT, "a send within it changes nothing");
    script.now += SECOND;
    NetBbrOnSend(bbr, 0u, DATAGRAM, true, script.now);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE && bbr->minRttStamp == script.now,
           "a send after it ends the measurement");

    /* Отправка после тишины: путь и так был пуст, замер не нужен, а темп —
     * ровно скорость пути, какой бы ни была фаза. */
    ScriptToProbeDown(2400000u);
    ExpectEqual(bbr->pacingRate, Pace(2400000u, 90u), "the way down is paced under the bandwidth");
    script.now += 11u * SECOND;
    NetBbrOnSend(bbr, 0u, DATAGRAM, true, script.now);
    Expect(bbr->idleRestart, "a send after silence restarts from idle");
    ExpectEqual(bbr->pacingRate, Pace(2400000u, 100u), "at the bandwidth, whatever the phase");
    script.now += SCRIPT_RTT;
    ScriptAck(2400000u, 1200u, 1200u, true);
    Expect(bbr->state != NET_BBR_PROBE_RTT && !bbr->idleRestart && bbr->minRttStamp == script.now,
           "the silence itself emptied the path");
    /* В пути не пусто либо соединение не простаивало: это не возврат. */
    NetBbrOnSend(bbr, 1200u, DATAGRAM, true, script.now);
    NetBbrOnSend(bbr, 0u, DATAGRAM, false, script.now);
    Expect(!bbr->idleRestart, "only an idle connection restarts");
}

static void TestRecoveryAndUndo(void)
{
    /* Потеря начинает эпизод; потери пакетов, отправленных до его начала,
     * нового не начинают. Подтверждение пакета, отправленного после, его
     * кончает. */
    ScriptToProbeBw(2400000u);
    NetBbr *bbr = &script.bbr;
    const uint64_t window = bbr->window;
    script.now += MS;
    Expect(ScriptLose(96000u, 1200u, false), "the first loss starts an episode");
    Expect(bbr->inRecovery && bbr->recoveryStart == script.now && bbr->priorWindow == window,
           "which remembers the window");
    Expect(!ScriptLose(96000u, 2400u, true), "losses of the same flight belong to it");
    const NetBbrLoss fresh = {
        .now = script.now + 10u * MS,
        .sentTime = script.now + MS,
        .txInFlight = 96000u,
        .lost = 1200u,
        .size = DATAGRAM,
    };
    Expect(NetBbrOnLoss(bbr, &fresh), "a loss of a packet sent later starts another");
    ExpectEqual(bbr->recoveryStart, script.now + 10u * MS, "from its own time");
    ExpectEqual(bbr->priorWindow, window, "with the same window to return to");
    script.now += 10u * MS;
    ScriptRound(2000000u, 60000u);
    Expect(bbr->inRecovery, "packets sent before the loss do not end the recovery");
    const NetBbrAck after = {
        .now = script.now + SCRIPT_RTT,
        .delivered = script.delivered + 1200u,
        .inFlight = 60000u,
        .priorDelivered = 0u,
        .sampleDelivered = 60000u,
        .rate = 2000000u,
        .proven = 2000000u,
        .txInFlight = 60000u,
        .acked = 1200u,
        .sentTime = script.now + MS,
        .rttSample = SCRIPT_RTT,
        .smoothedRtt = SCRIPT_RTT,
    };
    script.now += SCRIPT_RTT;
    script.delivered += 1200u;
    NetBbrOnAck(bbr, &after);
    Expect(!bbr->inRecovery, "a packet sent after it does");

    /* Мнимые потери: короткая модель и окно возвращаются. */
    ScriptToProbeBw(2400000u);
    ScriptRound(1000000u, 40000u);
    script.now += MS;
    (void)ScriptLose(96000u, 1200u, false);
    ScriptRound(1000000u, 40000u);
    Expect(bbr->bwShort == 1680000u && bbr->window == window * 7u / 10u,
           "the loss round lowers the model");
    NetBbrUndo(bbr);
    Expect(!bbr->inRecovery && bbr->bwShort == UNLIMITED && bbr->inflightShort == UNLIMITED &&
               bbr->window == window && !bbr->lossInRound && bbr->lossEvents == 0u,
           "spurious losses give everything back");

    /* Проба, прерванная мнимыми потерями, повторяется. */
    ScriptToProbeUp(2400000u);
    script.now += MS;
    (void)ScriptLose(120000u, 3600u, false);
    Expect(bbr->state == NET_BBR_PROBE_DOWN && bbr->inflightLong != UNLIMITED,
           "losses cut the probe short");
    NetBbrUndo(bbr);
    Expect(bbr->state == NET_BBR_PROBE_REFILL && bbr->inflightLong == UNLIMITED,
           "spurious ones restart it with the old limit");
    NetBbrUndo(bbr);
    Expect(bbr->state == NET_BBR_PROBE_REFILL, "undoing twice does nothing more");

    /* STARTUP, оконченный мнимыми потерями, продолжается. */
    ScriptBegin();
    NetBbrSeed(bbr, SCRIPT_RTT);
    ScriptRound(1000000u, 60000u);
    ScriptRound(2000000u, 120000u);
    script.now += MS;
    for (uint32_t index = 0u; index < 6u; ++index)
        (void)ScriptLose(120000u, (index + 1u) * DATAGRAM, false);
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        script.now += MS;
        ScriptAckFull(UINT64_C(4000000) << (index < 6u ? index : 6u), 1200u, 120000u, true, false,
                      7200u, 240000u);
    }
    Expect(bbr->state != NET_BBR_STARTUP && bbr->fullBwReached, "losses end startup");
    NetBbrUndo(bbr);
    Expect(bbr->state == NET_BBR_STARTUP && !bbr->fullBwReached && bbr->pacingGain == 277u &&
               bbr->inflightLong == UNLIMITED,
           "spurious ones resume it");
    Expect(bbr->fullBw == 0u && bbr->fullBwCount == 0u, "with the plateau detector reset");

    /* Окно, урезанное посреди восстановления, прежнего не затирает: новый
     * эпизод помнит большее. */
    ScriptToProbeBw(2400000u);
    const uint64_t full = bbr->window;
    script.now += MS;
    (void)ScriptLose(96000u, 1200u, false);
    script.sentAt = script.now - MS;
    ScriptRound(1000000u, 40000u);
    Expect(bbr->inRecovery && bbr->window < full, "a loss round cuts the window in the recovery");
    const NetBbrLoss again = {
        .now = script.now,
        .sentTime = script.now - MS,
        .txInFlight = 40000u,
        .lost = 1200u,
        .size = DATAGRAM,
    };
    Expect(NetBbrOnLoss(bbr, &again), "a later loss starts another episode");
    ExpectEqual(bbr->priorWindow, full, "which keeps the window from before the cut");

    /* Окно, урезанное короткой моделью посреди восстановления, по его концу
     * возвращается, если модель тем временем забыта. */
    ScriptToProbeBw(2400000u);
    const uint64_t before = bbr->window;
    script.now += MS;
    (void)ScriptLose(96000u, 1200u, false);
    script.sentAt = script.now - MS;
    ScriptRound(1000000u, 40000u);
    Expect(bbr->window < before, "a loss round cuts the window");
    script.now += 3u * SECOND;
    ScriptAck(1000000u, 1200u, 40000u, true);
    Expect(bbr->state == NET_BBR_PROBE_REFILL && bbr->inflightShort == UNLIMITED && bbr->inRecovery,
           "a probe forgets the short-term model within the recovery");
    script.sentAt = 0u;
    script.now += MS;
    ScriptAck(2400000u, 1200u, 40000u, false);
    Expect(!bbr->inRecovery && bbr->window >= before, "the end of the recovery returns the window");

    /* Возврат окна его не сужает: окно, выросшее за восстановление в
     * STARTUP, остаётся. */
    ScriptBegin();
    NetBbrSeed(bbr, SCRIPT_RTT);
    ScriptRound(1000000u, 60000u);
    script.now += MS;
    (void)ScriptLose(60000u, 1200u, false);
    const uint64_t saved = bbr->priorWindow;
    script.sentAt = script.now - MS;
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        script.now += MS;
        ScriptAck(2000000u, 12000u, 60000u, false);
    }
    const uint64_t grown = bbr->window;
    Expect(bbr->inRecovery && grown > saved, "startup grows the window in the recovery");
    script.sentAt = 0u;
    script.now += SCRIPT_RTT;
    ScriptAck(2000000u, 12000u, 60000u, false);
    Expect(!bbr->inRecovery, "the recovery ends");
    ExpectEqual(bbr->window, grown + 12000u, "and the grown window is kept");
}

static void TestCollapseAndLimits(void)
{
    /* Затяжная перегрузка: окно — две датаграммы, и прежнее не
     * возвращается. */
    ScriptToProbeBw(2400000u);
    NetBbr *bbr = &script.bbr;
    script.now += MS;
    (void)ScriptLose(96000u, 1200u, false);
    NetBbrCollapse(bbr, script.now);
    Expect(bbr->window == 2u * DATAGRAM && bbr->inRecovery && bbr->priorWindow == 2u * DATAGRAM,
           "persistent congestion leaves two datagrams");
    /* Подтверждения, потерянные в обрыве, приходят следом за ним: пакеты
     * доставлены ещё до обрыва, и окна они не растят. */
    const uint64_t collapsed = script.now;
    script.now += MS;
    script.sentAt = collapsed - MS;
    ScriptAck(2400000u, 24000u, 2400u, false);
    script.sentAt = 0u;
    Expect(bbr->window == 2u * DATAGRAM && bbr->inRecovery,
           "packets sent before it do not grow the window");
    script.now = collapsed + SCRIPT_RTT + MS;
    ScriptAck(60000u, 1200u, 2400u, true);
    Expect(!bbr->inRecovery, "the recovery ends");
    ExpectEqual(bbr->window, 4u * DATAGRAM, "and the window restarts from the smallest pipe");
    script.now += MS;
    ScriptAck(60000u, 1200u, 2400u, false);
    ExpectEqual(bbr->window, 5u * DATAGRAM, "growing by what is acknowledged");

    /* Окно не меньше четырёх датаграмм, даже когда модель просит меньше:
     * путь держит одну. Проба вверх прибавляет к ним две. */
    ScriptBegin();
    NetBbrSeed(bbr, SCRIPT_RTT);
    for (uint32_t round = 0u; round < 4u; ++round)
        ScriptRound(30000u, 2400u);
    Expect(bbr->fullBwReached, "a thin path is found");
    uint64_t lowest = UNLIMITED;
    uint64_t highest = 0u;
    for (uint32_t index = 0u; index < 80u; ++index)
    {
        script.now += SCRIPT_RTT / 8u;
        ScriptAck(30000u, 150u, 1200u, index % 8u == 0u);
        lowest = bbr->window < lowest ? bbr->window : lowest;
        highest = bbr->window > highest ? bbr->window : highest;
    }
    ExpectEqual(lowest, 4u * DATAGRAM, "the window never drops under four datagrams");
    ExpectEqual(highest, 6u * DATAGRAM, "and a probe adds two");
    ExpectEqual(NetBbrQuantum(bbr), 2u * DATAGRAM, "the burst never drops under two");

    /* Большой темп: пачка — миллисекунда отправки, но не больше 64 КиБ. */
    ScriptToProbeBw(24000000u);
    ExpectEqual(NetBbrQuantum(bbr), 23760u, "a burst is a millisecond of the pace");
    /* Путь в 100 мкс при 24 МБ/с держит 2400 байт, а темп выпускает разом
     * миллисекунду отправки: окно не меньше этой пачки. Сначала одиночные
     * подтверждения вытесняют из памяти крупные пачки сценария. */
    for (uint32_t index = 0u; index < 11u; ++index)
    {
        script.now += 10u * MS;
        ScriptAck(24000000u, 1200u, 2400u, true);
    }
    ExpectEqual(bbr->extraAcked, 1200u, "single acknowledgements come as expected");
    const NetBbrAck shorter = {
        .now = script.now + MS,
        .delivered = script.delivered + 1200u,
        .inFlight = 2400u,
        .priorDelivered = script.delivered,
        .sampleDelivered = 2400u,
        .rate = 24000000u,
        .proven = 24000000u,
        .txInFlight = 2400u,
        .acked = 1200u,
        .sentTime = script.now + MS - 100u,
        .rttSample = 100u,
        .smoothedRtt = 100u,
    };
    script.now += MS;
    script.delivered += 1200u;
    NetBbrOnAck(bbr, &shorter);
    ExpectEqual(bbr->minRtt, 100u, "a round trip of 100 microseconds");
    Expect(bbr->state == NET_BBR_PROBE_CRUISE && 2u * 2400u + bbr->extraAcked < NetBbrQuantum(bbr),
           "the path volume is below one burst");
    Expect(bbr->window >= NetBbrQuantum(bbr), "the window holds a burst");
    /* 160 Гбит/с на 40 мс: окно и предел объёма не уходят за предел
     * транспорта. */
    ScriptToProbeUp(UINT64_C(20000000000));
    ExpectEqual(NetBbrQuantum(bbr), 65536u, "up to 64 KiB");
    ExpectEqual(bbr->window, NET_CONGESTION_WINDOW_LIMIT,
                "the window stops at the transport limit");
    /* Предел объёма, найденный у самого предела транспорта. */
    bbr->inflightLong = NET_CONGESTION_WINDOW_LIMIT - DATAGRAM;
    NetBbrOnSend(bbr, bbr->window - DATAGRAM, DATAGRAM, false, script.now);
    script.now += MS;
    ScriptAck(UINT64_C(20000000000), UINT64_C(4000000000), NET_CONGESTION_WINDOW_LIMIT - DATAGRAM,
              false);
    ExpectEqual(bbr->inflightLong, NET_CONGESTION_WINDOW_LIMIT, "the flight limit stops there too");
    ExpectEqual(bbr->window, NET_CONGESTION_WINDOW_LIMIT, "with the window");

    /* Размер датаграммы меняет наименьшее окно. */
    NetBbrReset(bbr, 576u, 2u * 576u, 1u, SECOND);
    NetBbrSetDatagram(bbr, 1452u);
    ExpectEqual(bbr->window, 2904u, "a larger datagram raises the smallest window");
    ExpectEqual(NetBbrQuantum(bbr), 2904u, "and the smallest burst");
    NetBbrSetDatagram(bbr, 576u);
    ExpectEqual(bbr->window, 2904u, "a smaller one keeps the window");
}

static void TestNewPath(void)
{
    /* Пакеты, отправленные до сброса, ушли по прежнему пути: ни модель, ни
     * окно по ним не меняются. */
    ScriptToProbeBw(2400000u);
    NetBbr *bbr = &script.bbr;
    script.now += SECOND;
    NetBbrReset(bbr, DATAGRAM, 2u * DATAGRAM, 9u, script.now);
    NetBbrAck stale = {
        .now = script.now + MS,
        .delivered = script.delivered + 1200u,
        .inFlight = 50000u,
        .priorDelivered = script.delivered,
        .sampleDelivered = 96000u,
        .rate = 2400000u,
        .proven = 2400000u,
        .txInFlight = 96000u,
        .acked = 1200u,
        .sentTime = script.now - MS,
        .rttSample = SCRIPT_RTT,
        .smoothedRtt = SCRIPT_RTT,
    };
    NetBbrOnAck(bbr, &stale);
    Expect(bbr->maxBw == 0u && bbr->window == 2u * DATAGRAM && bbr->minRtt == UNLIMITED &&
               bbr->roundCount == 0u && bbr->pacingRate == 0u,
           "an acknowledgement from the old path teaches nothing");
    ExpectEqual(bbr->delivered, script.delivered + 1200u, "though the delivered count goes on");
    const NetBbrLoss old = {
        .now = script.now + MS,
        .sentTime = script.now - MS,
        .txInFlight = 96000u,
        .lost = 9600u,
        .size = DATAGRAM,
    };
    Expect(!NetBbrOnLoss(bbr, &old) && !bbr->inRecovery && !bbr->lossInRound,
           "nor does a loss on it");
    /* Пакет нового пути учит. */
    stale.sentTime = script.now;
    stale.now = script.now + SCRIPT_RTT;
    NetBbrOnAck(bbr, &stale);
    Expect(bbr->maxBw == 2400000u && bbr->minRtt == SCRIPT_RTT && bbr->window == 4u * DATAGRAM,
           "a packet sent on the new path does, and the window restarts from the smallest pipe");

    /* Зерно задаёт паузы между пробами: потоки одного узкого места не
     * пробуют полосу разом. */
    ScriptToProbeBw(2400000u);
    const uint64_t first = bbr->probeWait;
    scriptSeed = 8u;
    ScriptToProbeBw(2400000u);
    const uint64_t second = bbr->probeWait;
    scriptSeed = 7u;
    ScriptToProbeBw(2400000u);
    Expect(first != second, "connections draw their own probe waits");
    ExpectEqual(bbr->probeWait, first, "and a seed always draws the same");

    /* Часы конечной точки идут давно: оценка оборота свежа с первого
     * подтверждения. */
    NetBbrReset(bbr, DATAGRAM, WINDOW, 1u, 3600u * SECOND);
    NetBbrSeed(bbr, SCRIPT_RTT);
    const NetBbrAck opening = {
        .now = 3600u * SECOND + SCRIPT_RTT,
        .delivered = 1200u,
        .inFlight = 10800u,
        .priorDelivered = 0u,
        .sampleDelivered = 12000u,
        .rate = 300000u,
        .proven = 300000u,
        .txInFlight = 12000u,
        .acked = 1200u,
        .sentTime = 3600u * SECOND,
        .rttSample = SCRIPT_RTT,
        .smoothedRtt = SCRIPT_RTT,
    };
    NetBbrOnAck(bbr, &opening);
    Expect(bbr->state == NET_BBR_STARTUP && !bbr->minRttExpired && bbr->minRtt == SCRIPT_RTT,
           "a connection opened late trusts its first round trip");
}

static void TestAckAggregation(void)
{
    /* Подтверждения пачками: раз в 20 мс приходит всё, что путь доставил за
     * это время. Пачка сверх ожидаемого с начала отрезка прибавляется к
     * окну, иначе между пачками слать было бы нечем. */
    ScriptToProbeBw(2400000u);
    NetBbr *bbr = &script.bbr;
    ExpectEqual(bbr->extraAcked, 12000u, "steady acknowledgements leave one of them as the excess");
    for (uint32_t round = 0u; round < 3u; ++round)
    {
        for (uint32_t burst = 0u; burst < 2u; ++burst)
        {
            script.now += 20u * MS;
            ScriptAck(2400000u, 48000u, 96000u, burst == 0u);
        }
    }
    ExpectEqual(bbr->extraAcked, 48000u, "a burst is the excess");
    ExpectEqual(bbr->window, 240000u, "and it widens the window beyond twice the path volume");
    /* Подтверждения снова идут ровно; избыток помнится десять оборотов. */
    script.now += 15u * MS;
    for (uint32_t round = 0u; round < 10u; ++round)
        ScriptRound(2400000u, 96000u);
    ExpectEqual(bbr->extraAcked, 48000u, "it is remembered for ten rounds");
    ScriptRound(2400000u, 96000u);
    ExpectEqual(bbr->extraAcked, 12000u, "and forgotten after them");
    ExpectEqual(bbr->window, 204000u, "with the window");

    /* Пока полоса не найдена, помнится один прошлый оборот. */
    ScriptBegin();
    NetBbrSeed(bbr, SCRIPT_RTT);
    script.now += SCRIPT_RTT;
    ScriptAck(1000000u, 12000u, 12000u, true);
    script.now += MS;
    ScriptAck(1000000u, 36000u, 12000u, false);
    /* 48000 байт за миллисекунду при ожидаемой тысяче; избыток не больше
     * окна, а оно — 24000. */
    ExpectEqual(bbr->extraAcked, 24000u, "the excess never exceeds the window");
    script.now += SCRIPT_RTT;
    ScriptAck(1000000u, 1200u, 12000u, true);
    ExpectEqual(bbr->extraAcked, 24000u, "startup remembers it for the next round");
    /* Сам этот оборот дал 6970: 49200 байт за 41 мс при ожидаемых 42230 — по
     * оценке полосы и ещё датаграмме за оборот. */
    script.now += SCRIPT_RTT;
    ScriptAck(1000000u, 1200u, 12000u, true);
    ExpectEqual(bbr->extraAcked, 6970u, "and no longer");

    /* Оценка полосы бывает ниже пути на датаграмму за оборот. Доставка на
     * столько сверх оценки — не избыток: иначе он рос бы без конца. */
    ScriptToProbeBw(2400000u);
    for (uint32_t index = 0u; index < 80u; ++index)
    {
        script.now += SCRIPT_RTT / 8u;
        ScriptAck(2400000u, 12150u, 96000u, index % 8u == 0u);
    }
    ExpectEqual(bbr->extraAcked, 12150u, "a datagram per round above the estimate is no excess");
    /* Две датаграммы за оборот — уже избыток, и он копится: 150 байт на
     * подтверждение. */
    for (uint32_t index = 0u; index < 80u; ++index)
    {
        script.now += SCRIPT_RTT / 8u;
        ScriptAck(2400000u, 12300u, 96000u, index % 8u == 0u);
    }
    ExpectEqual(bbr->extraAcked, 12150u + 80u * 150u, "two datagrams a round are, and they add up");
}

/* ---- Модель пути -------------------------------------------------------------------- */

#define RING 16384u
#define STATES 4096u

typedef struct Packet
{
    uint64_t sentTime;
    /* Когда получатель отправил подтверждение и когда оно дойдёт. */
    uint64_t ackClock;
    uint64_t ackAt;
    NetRateStamp stamp;
    bool dropped;
    bool resolved;
} Packet;

static struct
{
    /* Путь: узкое место в байтах в секунду, оборот без очереди, очередь в
     * байтах (0 — без предела), случайные потери, шаг, с которым получатель
     * читает сеть и подтверждает прочитанное (0 — он знает приход каждого
     * пакета точно). */
    uint64_t rate;
    uint64_t delay;
    uint64_t buffer;
    uint32_t lossPerMillion;
    uint64_t ackEvery;
    /* Столько следующих пакетов путь потеряет. */
    uint32_t dropNext;
    /* Сколько байт приложение хочет отправить; UNLIMITED — без конца. */
    uint64_t offered;
    /* Шаг обновления отправителя. */
    uint64_t tick;

    uint64_t now;
    uint64_t busyUntil;
    uint64_t busyRemainder;
    uint64_t random;

    NetBbr bbr;
    NetRate meter;
    uint64_t inFlight;
    int64_t paceBudget;
    uint32_t paceRemainder;
    uint64_t paceTime;
    uint64_t paceEarned;
    uint64_t paceSpent;
    uint64_t paceElapsed;
    uint64_t paceClock;
    uint64_t smoothedRtt;
    uint64_t minRtt;
    uint64_t latestRtt;
    uint64_t clockBase;
    bool clockKnown;
    uint64_t largestAcked;
    uint64_t next;
    uint64_t oldest;
    Packet packets[RING];

    /* Итоги. */
    uint64_t ackedBytes;
    uint64_t sentPackets;
    uint64_t lostPackets;
    uint64_t peakBacklog;
    uint32_t episodes;
    /* Состояния по порядку и время входа в каждое. */
    uint8_t states[STATES];
    uint64_t stateTimes[STATES];
    uint32_t stateCount;
} path;

static void PathBegin(uint64_t rate, uint64_t delay, uint64_t buffer)
{
    memset(&path, 0, sizeof(path));
    path.rate = rate;
    path.delay = delay;
    path.buffer = buffer;
    path.offered = UNLIMITED;
    path.tick = MS;
    path.ackEvery = MS;
    path.now = SECOND;
    path.random = UINT64_C(0x9E3779B97F4A7C15);
    NetBbrReset(&path.bbr, DATAGRAM, WINDOW, UINT64_C(0x6C616975), path.now);
    path.paceBudget = (int64_t)WINDOW;
    path.paceTime = path.now;
    /* Рукопожатие уже измерило оборот. */
    path.smoothedRtt = delay;
    path.minRtt = delay;
    path.latestRtt = delay;
    NetBbrSeed(&path.bbr, delay);
    path.states[0] = path.bbr.state;
    path.stateTimes[0] = path.now;
    path.stateCount = 1u;
}

static Packet *PathPacket(uint64_t number)
{
    return &path.packets[number & (RING - 1u)];
}

static uint64_t PathRandom(void)
{
    path.random ^= path.random << 13u;
    path.random ^= path.random >> 7u;
    path.random ^= path.random << 17u;
    return path.random;
}

/* Очередь узкого места в байтах. */
static uint64_t PathBacklog(void)
{
    return path.busyUntil > path.now ? NetScale(path.busyUntil - path.now, path.rate, SECOND) : 0u;
}

static void PathSendPacket(void)
{
    Packet *packet = PathPacket(path.next);
    memset(packet, 0, sizeof(*packet));
    packet->sentTime = path.now;
    NetBbrOnSend(&path.bbr, path.inFlight, DATAGRAM, path.meter.appLimited != 0u, path.now);
    path.inFlight += DATAGRAM;
    /* Время при ровном темпе, как его считает транспорт. */
    uint64_t paced = path.now;
    path.paceSpent += DATAGRAM;
    if (path.paceSpent < path.paceEarned)
        paced = path.now - path.paceElapsed +
                NetScale(path.paceSpent, path.paceElapsed, path.paceEarned);
    if (paced < path.paceClock)
        paced = path.paceClock;
    path.paceClock = paced;
    NetRateSent(&path.meter, &packet->stamp, path.inFlight, DATAGRAM, paced, path.now);
    path.paceBudget -= (int64_t)DATAGRAM;
    ++path.next;
    ++path.sentPackets;
    if (path.offered != UNLIMITED)
        path.offered -= path.offered < DATAGRAM ? path.offered : DATAGRAM;

    packet->dropped = path.dropNext != 0u ||
                      (path.lossPerMillion != 0u && PathRandom() % 1000000u < path.lossPerMillion);
    if (path.dropNext != 0u)
        --path.dropNext;
    if (packet->dropped)
        return;
    const uint64_t backlog = PathBacklog();
    if (path.buffer != 0u && backlog + DATAGRAM > path.buffer)
    {
        packet->dropped = true;
        return;
    }
    if (backlog > path.peakBacklog)
        path.peakBacklog = backlog;
    if (path.busyUntil <= path.now)
    {
        path.busyUntil = path.now;
        path.busyRemainder = 0u;
    }
    const uint64_t service = (uint64_t)DATAGRAM * SECOND + path.busyRemainder;
    path.busyUntil += service / path.rate;
    path.busyRemainder = service % path.rate;
    const uint64_t received = path.busyUntil + path.delay / 2u;
    packet->ackClock = path.ackEvery != 0u
                           ? (received + path.ackEvery - 1u) / path.ackEvery * path.ackEvery
                           : received;
    packet->ackAt = packet->ackClock + path.delay - path.delay / 2u;
}

static void PathRtt(uint64_t sample)
{
    path.latestRtt = sample != 0u ? sample : 1u;
    if (path.latestRtt < path.minRtt)
        path.minRtt = path.latestRtt;
    path.smoothedRtt = (7u * path.smoothedRtt + path.latestRtt) / 8u;
}

static void PathLose(uint64_t number)
{
    Packet *packet = PathPacket(number);
    packet->resolved = true;
    path.inFlight -= DATAGRAM;
    path.meter.lost += DATAGRAM;
    ++path.lostPackets;
    const Packet *previous = number != 0u ? PathPacket(number - 1u) : NULL;
    const NetBbrLoss loss = {
        .now = path.now,
        .sentTime = packet->sentTime,
        .txInFlight = packet->stamp.inFlight,
        .lost = path.meter.lost - packet->stamp.lost,
        .size = DATAGRAM,
        .appLimited = packet->stamp.appLimited,
        .contiguous = previous != NULL && previous->dropped && previous->resolved,
    };
    if (NetBbrOnLoss(&path.bbr, &loss))
        ++path.episodes;
}

/* Кадр подтверждения разобран: оборот, потери, замер, модель. */
static void PathFrame(NetRateSample *sample, const Packet *newest)
{
    PathRtt(path.now - newest->sentTime);
    /* Потеря — обгон на три пакета либо возраст больше 9/8 оборота. */
    const uint64_t reference =
        path.latestRtt > path.smoothedRtt ? path.latestRtt : path.smoothedRtt;
    for (uint64_t number = path.oldest; number < path.largestAcked; ++number)
    {
        const Packet *packet = PathPacket(number);
        if (packet->resolved || !packet->dropped)
            continue;
        if (number + 3u <= path.largestAcked || packet->sentTime + reference * 9u / 8u <= path.now)
            PathLose(number);
    }
    while (path.oldest < path.next && PathPacket(path.oldest)->resolved)
        ++path.oldest;
    NetRateFinish(&path.meter, sample, path.minRtt);
    const NetBbrAck ack = {
        .now = path.now,
        .delivered = path.meter.delivered,
        .inFlight = path.inFlight,
        .priorDelivered = sample->stamp.delivered,
        .sampleDelivered = sample->flight,
        .lost = sample->lost,
        .rate = sample->rate,
        .proven = sample->proven,
        .txInFlight = sample->stamp.inFlight,
        .acked = sample->acked,
        .sentTime = sample->sentTime,
        .rttSample = path.latestRtt,
        .smoothedRtt = path.smoothedRtt,
        .appLimited = sample->stamp.appLimited,
    };
    NetBbrOnAck(&path.bbr, &ack);
    if (NetBbrProbesRtt(&path.bbr))
        NetRateLimited(&path.meter, path.inFlight);
}

/* Одно обновление отправителя: подтверждения, потери, модель, отправка.
 * Получатель подтверждает одним кадром всё, что прочёл разом; отправитель
 * разбирает кадры по одному, как транспорт. */
static void PathStep(void)
{
    path.now += path.tick;
    NetRateSample sample = {0};
    const Packet *newest = NULL;
    for (uint64_t number = path.oldest; number < path.next; ++number)
    {
        Packet *packet = PathPacket(number);
        if (packet->resolved || packet->dropped)
            continue;
        if (packet->ackAt > path.now)
            break;
        if (!path.clockKnown)
        {
            path.clockKnown = true;
            path.clockBase = path.now - packet->ackClock;
        }
        const uint64_t clock = packet->ackClock + path.clockBase;
        if (newest != NULL && clock != sample.ackTime)
        {
            PathFrame(&sample, newest);
            newest = NULL;
        }
        if (newest == NULL)
        {
            memset(&sample, 0, sizeof(sample));
            sample.ackTime = clock;
            sample.ackAge = path.ackEvery;
        }
        packet->resolved = true;
        path.inFlight -= DATAGRAM;
        path.ackedBytes += DATAGRAM;
        NetRateAcked(&path.meter, &sample, &packet->stamp, number, packet->sentTime, DATAGRAM,
                     path.now);
        path.largestAcked = number;
        newest = packet;
    }
    if (newest != NULL)
        PathFrame(&sample, newest);

    /* Темп, как в транспорте. */
    const uint64_t elapsed = path.now - path.paceTime;
    path.paceTime = path.now;
    path.paceSpent = 0u;
    const uint64_t total = path.bbr.pacingRate * elapsed + path.paceRemainder;
    const uint64_t earned = total / SECOND;
    path.paceRemainder = (uint32_t)(total % SECOND);
    path.paceEarned = earned;
    path.paceElapsed = elapsed;
    const int64_t burst = (int64_t)NetBbrQuantum(&path.bbr);
    const int64_t ceiling = (int64_t)earned > burst ? (int64_t)earned : burst;
    path.paceBudget += (int64_t)earned;
    if (path.paceBudget > ceiling)
        path.paceBudget = ceiling;
    while (path.offered >= DATAGRAM && path.inFlight < path.bbr.window && path.paceBudget > 0 &&
           path.next - path.oldest < RING)
        PathSendPacket();
    if (path.inFlight < path.bbr.window && path.paceBudget > 0)
    {
        if (path.offered < DATAGRAM)
            NetRateLimited(&path.meter, path.inFlight);
    }
    else if (path.offered >= DATAGRAM && path.inFlight + DATAGRAM > path.bbr.window)
    {
        NetBbrWindowFilled(&path.bbr);
    }
    if (path.bbr.state != path.states[path.stateCount - 1u] && path.stateCount < STATES)
    {
        path.states[path.stateCount] = path.bbr.state;
        path.stateTimes[path.stateCount] = path.now;
        ++path.stateCount;
    }
}

static void PathRun(uint64_t duration)
{
    const uint64_t until = path.now + duration;
    while (path.now < until)
        PathStep();
}

/* Доля полосы узкого места, доставленная за duration, в процентах. */
static uint64_t PathUtilization(uint64_t duration)
{
    const uint64_t before = path.ackedBytes;
    PathRun(duration);
    return (path.ackedBytes - before) * 100u * SECOND / (path.rate * duration);
}

/* Первое вхождение состояния не раньше позиции from; STATES — нет. */
static uint32_t PathFindState(uint8_t state, uint32_t from)
{
    for (uint32_t index = from; index < path.stateCount; ++index)
    {
        if (path.states[index] == state)
            return index;
    }
    return STATES;
}

static void TestPathStartup(void)
{
    /* 20 Мбит/с, оборот 40 мс, очередь без предела. Объём пути — 100 кБ,
     * или 83 датаграммы. */
    PathBegin(2500000u, 40u * MS, 0u);
    while (path.bbr.state == NET_BBR_STARTUP && path.now < 10u * SECOND)
        PathStep();
    const NetBbr *bbr = &path.bbr;
    Expect(bbr->state == NET_BBR_DRAIN, "startup ends by itself");
    /* Окно удваивается за оборот: с десяти датаграмм до объёма пути — три
     * оборота с лишним, и ещё три уходит на плато. */
    ExpectBetween(bbr->roundCount, 7u, 10u, "in the rounds a doubling search takes");
    ExpectBetween(bbr->maxBw, 2475000u, 2500000u, "having measured the bottleneck and not more");
    ExpectEqual(path.lostPackets, 0u, "without losing a packet");
    Expect(path.peakBacklog < 300000u, "and with a queue of under three path volumes");
    ExpectBetween(bbr->minRtt, 40u * MS, 42u * MS, "the round trip is the path's");
    /* Очередь уходит, и соединение входит в цикл проб. Спускать больше
     * нечего: ход сразу ровный. */
    const uint64_t drainStart = path.now;
    while (path.bbr.state == NET_BBR_DRAIN && path.now < 10u * SECOND)
        PathStep();
    Expect(bbr->state == NET_BBR_PROBE_CRUISE, "the queue drains into the probing cycle");
    Expect(path.now - drainStart < 200u * MS, "within a few round trips");
    Expect(path.inFlight <= 100000u + 4u * DATAGRAM, "leaving a path volume in flight");
    PathRun(200u * MS);
    Expect(bbr->state == NET_BBR_PROBE_CRUISE && PathBacklog() < 4u * DATAGRAM,
           "then it cruises with no queue to speak of");
}

static void TestPathCycle(void)
{
    PathBegin(2500000u, 40u * MS, 0u);
    PathRun(SECOND);
    const uint64_t utilization = PathUtilization(9u * SECOND);
    ExpectBetween(utilization, 96u, 99u, "a clean path is used almost fully");
    ExpectEqual(path.lostPackets, 0u, "with no losses");
    /* Порядок фаз: ровный ход, наполнение, вверх, вниз — и снова ровный
     * ход. */
    uint32_t position = PathFindState(NET_BBR_PROBE_CRUISE, 0u);
    uint32_t probes = 0u;
    uint64_t previousUp = 0u;
    while (position + 4u < path.stateCount)
    {
        Expect(path.states[position + 1u] == NET_BBR_PROBE_REFILL &&
                   path.states[position + 2u] == NET_BBR_PROBE_UP &&
                   path.states[position + 3u] == NET_BBR_PROBE_DOWN &&
                   path.states[position + 4u] == NET_BBR_PROBE_CRUISE,
               "the probing cycle cruises, refills, probes up and comes down");
        const uint64_t up = path.stateTimes[position + 2u];
        const uint64_t down = path.stateTimes[position + 3u];
        /* Наполнение длится оборот, проба — три оборота плато; её очередь в
         * объём пути уходит на девяти десятых темпа за несколько оборотов. */
        ExpectBetween(up - path.stateTimes[position + 1u], 40u * MS, 50u * MS,
                      "refilling takes one round trip");
        ExpectBetween(down - up, 120u * MS, 220u * MS,
                      "a probe that finds nothing takes three or four");
        ExpectBetween(path.stateTimes[position + 4u] - down, 200u * MS, 400u * MS,
                      "and its queue drains in under ten");
        if (previousUp != 0u)
            ExpectBetween(up - previousUp, 2u * SECOND, 3400u * MS,
                          "probes are two to three seconds apart");
        previousUp = up;
        position += 4u;
        ++probes;
    }
    ExpectEqual(probes, 3u, "three probes fit in ten seconds");
    /* Проба копит очередь на четверть полосы, ровный ход — нет. */
    Expect(path.peakBacklog < 300000u, "the queue never exceeds the startup one");
    const NetBbr *bbr = &path.bbr;
    ExpectBetween(bbr->maxBw, 2475000u, 2500000u, "the estimate stays at the bottleneck");

    /* Получатель, знающий приход каждого пакета точно, меряет путь без
     * погрешности. */
    PathBegin(2500000u, 40u * MS, 0u);
    path.ackEvery = 0u;
    PathRun(SECOND);
    ExpectBetween(PathUtilization(9u * SECOND), 96u, 99u,
                  "exact arrival clocks use the path as well");
    ExpectEqual(bbr->maxBw, 2500000u, "and measure the bottleneck itself");
}

static void TestPathStartupLoss(void)
{
    /* Очередь в четверть объёма пути: STARTUP переполняет её и кончается по
     * потерям, а не по плато. */
    PathBegin(2500000u, 40u * MS, 25000u);
    while (path.bbr.state == NET_BBR_STARTUP && path.now < 10u * SECOND)
        PathStep();
    const NetBbr *bbr = &path.bbr;
    Expect(bbr->state != NET_BBR_STARTUP && bbr->fullBwReached, "a shallow queue ends startup");
    Expect(bbr->inflightLong != UNLIMITED, "by loss: the flight limit is known");
    ExpectBetween(bbr->inflightLong, 90000u, 150000u, "and it is about the path volume");
    ExpectBetween(bbr->maxBw, 2200000u, 2500000u, "the bandwidth is found all the same");
    Expect(path.lostPackets < 120u, "at the price of one overflow");
    /* Дальше потери редки: пробы упираются в найденный предел. */
    const uint64_t lost = path.lostPackets;
    const uint64_t sent = path.sentPackets;
    const uint64_t utilization = PathUtilization(20u * SECOND);
    ExpectBetween(utilization, 93u, 99u, "the path is still used");
    Expect((path.lostPackets - lost) * 100u < path.sentPackets - sent,
           "with under a percent of losses");
}

static void TestPathBandwidthChange(void)
{
    /* Полоса удваивается: следующая же проба находит её. */
    PathBegin(2500000u, 40u * MS, 0u);
    PathRun(5u * SECOND);
    path.rate = 5000000u;
    PathRun(4u * SECOND);
    const NetBbr *bbr = &path.bbr;
    ExpectBetween(bbr->maxBw, 4800000u, 5000000u, "a probe finds twice the bandwidth");
    ExpectBetween(PathUtilization(4u * SECOND), 95u, 99u, "and the flow uses it");

    /* Полоса падает вдвое. Очередь велика, потерь нет: старая оценка живёт
     * два цикла проб, потом соединение шлёт не быстрее нового пути, и
     * очередь уходит. */
    PathBegin(2500000u, 40u * MS, 0u);
    PathRun(5u * SECOND);
    path.rate = 1250000u;
    PathRun(7u * SECOND);
    ExpectBetween(bbr->maxBw, 1230000u, 1250000u, "two cycles later the estimate is the new path");
    PathRun(3u * SECOND);
    Expect(PathBacklog() < 10u * DATAGRAM, "and the queue is gone");

    /* То же с очередью в объём пути: потери опускают короткую модель, не
     * дожидаясь двух циклов. */
    PathBegin(2500000u, 40u * MS, 100000u);
    PathRun(5u * SECOND);
    path.rate = 1250000u;
    const uint64_t lost = path.lostPackets;
    PathRun(SECOND);
    Expect(bbr->bw <= 1300000u, "losses bring the model down within a second");
    PathRun(9u * SECOND);
    Expect(path.lostPackets - lost < 400u, "and stop");
}

static void TestPathRandomLoss(void)
{
    /* Случайные потери ниже порога полосу не отнимают. */
    PathBegin(6250000u, 100u * MS, 0u);
    path.lossPerMillion = 1000u;
    PathRun(2u * SECOND);
    ExpectBetween(PathUtilization(20u * SECOND), 88u, 99u, "a tenth of a percent of loss is noise");
    PathBegin(2500000u, 40u * MS, 0u);
    path.lossPerMillion = 10000u;
    PathRun(2u * SECOND);
    ExpectBetween(PathUtilization(30u * SECOND), 70u, 99u, "one percent leaves most of the path");
    /* Выше порога соединение отступает, но не останавливается. */
    PathBegin(2500000u, 40u * MS, 0u);
    path.lossPerMillion = 50000u;
    PathRun(2u * SECOND);
    ExpectBetween(PathUtilization(30u * SECOND), 5u, 60u, "five percent is treated as congestion");
}

static void TestPathProbeRtt(void)
{
    PathBegin(2500000u, 40u * MS, 0u);
    PathRun(9u * SECOND);
    const NetBbr *bbr = &path.bbr;
    const uint32_t before = path.stateCount;
    PathRun(3u * SECOND);
    const uint32_t entered = PathFindState(NET_BBR_PROBE_RTT, before);
    Expect(entered != STATES && entered + 1u < path.stateCount,
           "the round trip is measured again ten seconds after the first sample");
    ExpectBetween(path.stateTimes[entered], 11u * SECOND, 11200u * MS, "on time");
    /* Объём падает до половины пути, замер длится 200 мс и оборот. */
    ExpectBetween(path.stateTimes[entered + 1u] - path.stateTimes[entered], 200u * MS, 330u * MS,
                  "for a fifth of a second and a round trip");
    Expect(path.states[entered + 1u] == NET_BBR_PROBE_CRUISE, "then the flow cruises");
    ExpectBetween(bbr->minRtt, 40u * MS, 42u * MS, "with the same round trip");
    ExpectBetween(PathUtilization(8u * SECOND), 95u, 99u, "and the same bandwidth");
    Expect(PathFindState(NET_BBR_PROBE_RTT, entered + 1u) == STATES,
           "the next measurement is ten seconds away");

    /* Путь стал длиннее. Старый оборот живёт десять секунд, после замера
     * модель знает новый. */
    PathBegin(2500000u, 40u * MS, 0u);
    PathRun(3u * SECOND);
    path.delay = 80u * MS;
    PathRun(12u * SECOND);
    ExpectBetween(bbr->minRtt, 80u * MS, 82u * MS, "a longer path is learned by the measurement");
    ExpectBetween(PathUtilization(8u * SECOND), 94u, 99u, "and used as before");
}

static void TestPathApplicationLimited(void)
{
    /* Приложение даёт четверть полосы. Оценка не падает, и когда данных
     * становится много, соединение сразу шлёт во всю полосу. */
    PathBegin(2500000u, 40u * MS, 0u);
    PathRun(5u * SECOND);
    const NetBbr *bbr = &path.bbr;
    const uint64_t found = bbr->maxBw;
    path.offered = 0u;
    for (uint32_t tick = 0u; tick < 20000u; ++tick)
    {
        if (tick % 2u == 0u)
            path.offered += 1250u;
        PathStep();
    }
    Expect(bbr->maxBw >= found - found / 50u, "an application-limited flow keeps its estimate");
    Expect(bbr->state != NET_BBR_STARTUP && PathBacklog() == 0u, "and builds no queue");
    path.offered = UNLIMITED;
    ExpectBetween(PathUtilization(SECOND), 90u, 100u, "bulk data resumes at the full bandwidth");

    /* С самого начала данных мало: поиск полосы не кончается на мнимом
     * плато и продолжается, когда данных становится много. */
    PathBegin(2500000u, 40u * MS, 0u);
    path.offered = 0u;
    for (uint32_t tick = 0u; tick < 5000u; ++tick)
    {
        if (tick % 4u == 0u)
            path.offered += 1200u;
        PathStep();
    }
    Expect(bbr->state == NET_BBR_STARTUP && !bbr->fullBwReached,
           "a thin stream does not end the search");
    path.offered = UNLIMITED;
    PathRun(SECOND);
    Expect(bbr->fullBwReached, "bulk data completes it");
    ExpectBetween(bbr->maxBw, 2475000u, 2500000u, "at the bottleneck");
}

static void TestPathIdleRestart(void)
{
    /* Двенадцать секунд тишины: оборот устарел, но путь и так пуст. После
     * неё соединение шлёт в темпе пути, без пачки в целое окно и без замера
     * оборота. */
    PathBegin(2500000u, 40u * MS, 0u);
    PathRun(4u * SECOND);
    const NetBbr *bbr = &path.bbr;
    path.offered = 0u;
    PathRun(12u * SECOND);
    ExpectEqual(path.inFlight, 0u, "everything is delivered");
    const uint32_t before = path.stateCount;
    path.offered = UNLIMITED;
    path.peakBacklog = 0u;
    const uint64_t sent = path.sentPackets;
    PathRun(40u * MS);
    ExpectBetween((path.sentPackets - sent) * DATAGRAM, 90000u, 104000u,
                  "the first round trip carries one path volume");
    Expect(path.peakBacklog < 8u * DATAGRAM, "at the pace of the path");
    PathRun(2u * SECOND);
    Expect(PathFindState(NET_BBR_PROBE_RTT, before) == STATES,
           "silence stands in for the round trip measurement");
    ExpectBetween(bbr->minRtt, 40u * MS, 42u * MS, "the round trip is refreshed by the restart");
    ExpectBetween(PathUtilization(2u * SECOND), 95u, 99u, "and the flow goes on");
}

static void TestPathAggregation(void)
{
    /* Получатель подтверждает раз в 60 мс при обороте 40 мс. Без запаса на
     * пачки окно в два объёма пути простаивало бы между подтверждениями. */
    PathBegin(2500000u, 40u * MS, 0u);
    path.ackEvery = 60u * MS;
    PathRun(3u * SECOND);
    const NetBbr *bbr = &path.bbr;
    ExpectBetween(bbr->extraAcked, 100000u, 200000u, "the excess is about one acknowledgement");
    ExpectBetween(PathUtilization(20u * SECOND), 93u, 99u, "and the path stays busy");
    ExpectBetween(bbr->maxBw, 2400000u, 2500000u, "bursts of acknowledgements do not inflate it");
}

static void TestPathCoarseUpdates(void)
{
    /* Отправитель обновляется раз в 16 мс, получатель подтверждает раз в
     * 16 мс со своей фазой. Замер по ровному времени отправки и по часам
     * получателя не завышает полосу, и очередь не копится. */
    PathBegin(2500000u, 40u * MS, 0u);
    path.tick = 16u * MS;
    path.ackEvery = 13u * MS;
    PathRun(2u * SECOND);
    path.peakBacklog = 0u;
    const NetBbr *bbr = &path.bbr;
    uint64_t highest = 0u;
    uint64_t cruising = 0u;
    uint64_t delivered = path.ackedBytes;
    for (uint32_t step = 0u; step < 1250u; ++step)
    {
        PathStep();
        if (bbr->maxBw > highest)
            highest = bbr->maxBw;
        if (bbr->state == NET_BBR_PROBE_CRUISE && PathBacklog() > cruising)
            cruising = PathBacklog();
    }
    delivered = path.ackedBytes - delivered;
    Expect(highest <= 2500000u, "coarse updates never overstate the bandwidth");
    ExpectBetween(highest, 2400000u, 2500000u, "nor lose it");
    ExpectBetween(delivered * 100u / (2500000u * 20u), 93u, 99u, "the path is used");
    /* Пачка обновления — 40 кБ: на ровном ходу очередь — она одна. Проба
     * три-четыре оборота шлёт на четверть быстрее пути и копит свою поверх. */
    Expect(cruising < 40000u + 10u * DATAGRAM,
           "while cruising the queue is the burst of one update");
    Expect(path.peakBacklog < 200000u, "and a probe adds under two path volumes to it");
}

static void TestPathSmallWindow(void)
{
    /* Путь на шесть датаграмм: 180 кБ/с при обороте 40 мс. Пробы идут по
     * счёту оборотов, и полоса занята. */
    PathBegin(180000u, 40u * MS, 0u);
    PathRun(2u * SECOND);
    const uint32_t before = path.stateCount;
    ExpectBetween(PathUtilization(10u * SECOND), 90u, 100u, "a thin path is used");
    uint32_t probes = 0u;
    for (uint32_t index = before; index < path.stateCount; ++index)
        probes += path.states[index] == NET_BBR_PROBE_UP ? 1u : 0u;
    Expect(probes >= 12u, "and probed far more often than every two seconds");
    Expect(path.bbr.window >= 4u * DATAGRAM, "with at least four datagrams of window");
    /* Пакеты приходят раз в 6,7 мс, а получатель читает сеть каждую
     * миллисекунду: полоса известна с точностью до неё, а не до пакета из
     * шести. */
    ExpectBetween(path.bbr.maxBw, 176000u, 180000u, "and measured to the age of a read");
}

LAIUE_TEST_ENTRY(NetBbrTestEntryPoint)
{
    TestRateSteady();
    TestRateIntervals();
    TestRatePacedTime();
    TestRateReceiverClock();
    TestRateProven();
    TestRateApplicationLimited();
    TestRateIdleRestart();
    TestRateLossAndFlight();
    TestStartupRules();
    TestStartupLossRules();
    TestProbeCycleRules();
    TestProbeWaitSpread();
    TestRenoCoexistence();
    TestProbeLossRules();
    TestShortTermModel();
    TestProvenRate();
    TestMaxBwFilter();
    TestInflightProbing();
    TestPrecautionaryProbe();
    TestProbeRttRules();
    TestRecoveryAndUndo();
    TestCollapseAndLimits();
    TestNewPath();
    TestAckAggregation();
    TestPathStartup();
    TestPathCycle();
    TestPathStartupLoss();
    TestPathBandwidthChange();
    TestPathRandomLoss();
    TestPathProbeRtt();
    TestPathApplicationLimited();
    TestPathIdleRestart();
    TestPathAggregation();
    TestPathCoarseUpdates();
    TestPathSmallWindow();
    LaiueTestRuntimeWrite("net bbr: passed\n");
    LAIUE_TEST_SUCCESS();
}
