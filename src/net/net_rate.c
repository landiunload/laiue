#include "net/net_internal.h"

/* Замер скорости доставки (draft-ietf-ccwg-bbr-07, раздел 4.1).
 *
 * Каждый пакет увозит снимок счётчиков соединения. Когда он подтверждён,
 * известно, сколько байт доставлено за время его полёта, и скорость — это
 * доставленное, делённое на больший из двух отрезков: за который эти байты
 * были отправлены и за который пришли их подтверждения. Первый не даёт
 * подтверждениям, пришедшим пачкой, завысить скорость, второй — пачке
 * отправленных пакетов. Замер верен, только если всё это время было что
 * слать: остальные помечены как ограниченные приложением.
 *
 * Соединение шлёт и читает сеть раз в обновление, и по часам этого шага оба
 * отрезка шумели бы на его долю, а наибольший из шумных замеров завышен.
 * Поэтому время отправки здесь не настоящее, а то, в которое пакет ушёл бы
 * при ровном темпе, — иначе пачка одного обновления считалась бы отправленной
 * мгновенно; а время подтверждения взято по часам получателя: между двумя
 * его подтверждениями принято ровно то, что названо во втором.
 *
 * Получатель тоже читает сеть раз в обновление, и отрезок между двумя его
 * подтверждениями кончается не приходом пакета, а чтением. Путь со скоростью
 * R за отрезок T доставляет R·T байт и ещё до одного пакета — того, что был
 * на подходе к началу отрезка. Скорость доставки поэтому бывает выше пути на
 * пакет за отрезок, и наибольший из замеров как раз таков. На пути, который
 * держит десяток пакетов, это десятая часть: по такой оценке очередь на
 * узком месте не рассасывалась бы.
 *
 * Поэтому рядом считается скорость, которую путь заведомо выдал. Пакеты
 * приходят не чаще, чем их пропускает узкое место. Все пакеты отрезка пришли
 * после его начала — значит, все, кроме первого, уложились в T. А если
 * последний пакет перед отрезком пришёл не раньше чем за G до его начала, то
 * все пакеты отрезка уложились в T + G. Получатель называет G в каждом
 * подтверждении: это время с прошлого чтения сети. Когда он читает её чаще,
 * чем приходят пакеты, почти точна вторая граница, когда реже — первая. По
 * этой скорости судят о полосе пути; скорость доставки остаётся тому, кто
 * следит, как она меняется. */

/* Заведомая скорость считается без одного пакета, но не больше чем без такой
 * доли доставленного. На пути в несколько пакетов замер иначе вышел бы ниже
 * оценки и при пробе полосы, что шлёт на четверть быстрее, и оценка падала бы
 * от пробы к пробе. */
#define RATE_EARLY_SHARE 8u

void NetRateSent(NetRate *rate, NetRateStamp *stamp, uint64_t inFlight, uint32_t size,
                 uint64_t pacedTime, uint64_t now)
{
    /* В пути пусто: отсчёт идёт от этой отправки, а не от давнего
     * подтверждения. Часы получателя за время тишины ушли вперёд на столько
     * же, на сколько свои, и раньше этого момента новые пакеты не придут. */
    if (inFlight == size)
    {
        rate->firstSentTime = pacedTime;
        rate->deliveredTime += now - rate->ackedTime;
        rate->deliveredAge = 0u;
        rate->deliveredOpen = false;
        rate->ackedTime = now;
    }
    stamp->delivered = rate->delivered;
    stamp->deliveredTime = rate->deliveredTime;
    stamp->deliveredAge =
        rate->deliveredAge > UINT32_MAX ? UINT32_MAX : (uint32_t)rate->deliveredAge;
    stamp->deliveredOpen = rate->deliveredOpen;
    stamp->firstSentTime = rate->firstSentTime;
    stamp->lost = rate->lost;
    stamp->pacedTime = pacedTime;
    stamp->inFlight = inFlight > UINT32_MAX ? UINT32_MAX : (uint32_t)inFlight;
    stamp->appLimited = rate->appLimited != 0u;
}

void NetRateAcked(NetRate *rate, NetRateSample *sample, const NetRateStamp *stamp, uint64_t number,
                  uint64_t sentTime, uint32_t size, uint64_t now)
{
    rate->delivered += size;
    /* Подтверждение, обогнанное более поздним, времени назад не ведёт. Кадр
     * с теми же часами — о том же чтении сети: если первый отправлен посреди
     * него, то второй говорит, что оно кончилось. */
    if (sample->ackTime >= rate->deliveredTime)
    {
        rate->deliveredTime = sample->ackTime;
        rate->deliveredAge = sample->ackAge;
        rate->deliveredOpen = sample->ackOpen;
    }
    rate->ackedTime = now;
    sample->acked += size;
    if (size > sample->largest)
        sample->largest = size;
    const uint64_t flight = rate->delivered - stamp->delivered;
    if (flight > sample->flight)
        sample->flight = flight;
    if (sample->filled && number < sample->number)
        return;
    sample->filled = true;
    sample->number = number;
    sample->sentTime = sentTime;
    sample->stamp = *stamp;
    rate->firstSentTime = stamp->pacedTime;
}

void NetRateFinish(NetRate *rate, NetRateSample *sample, uint64_t minRtt)
{
    /* Всё, что было в пути при отметке, доставлено: пузырь тишины вышел. */
    if (rate->appLimited != 0u && rate->delivered > rate->appLimited)
        rate->appLimited = 0u;
    if (!sample->filled)
        return;
    const NetRateStamp *stamp = &sample->stamp;
    const uint64_t sendElapsed =
        stamp->pacedTime > stamp->firstSentTime ? stamp->pacedTime - stamp->firstSentTime : 0u;
    const uint64_t ackElapsed = rate->deliveredTime - stamp->deliveredTime;
    sample->interval = sendElapsed > ackElapsed ? sendElapsed : ackElapsed;
    sample->delivered = rate->delivered - stamp->delivered;
    sample->lost = rate->lost - stamp->lost;
    if (sample->interval == 0u || sample->interval < minRtt)
        return;
    sample->rate = NetScale(sample->delivered, NET_SECOND, sample->interval);

    /* Сколько путь выдал заведомо. Когда отрезок начат подтверждением,
     * отправленным посреди чтения сети, его пакеты могли прийти и до начала:
     * первой границы у такого отрезка нет. */
    uint64_t arrival = 0u;
    if (!stamp->deliveredOpen && ackElapsed != 0u)
    {
        uint64_t early = sample->largest;
        if (early > sample->delivered / RATE_EARLY_SHARE)
            early = sample->delivered / RATE_EARLY_SHARE;
        arrival = NetScale(sample->delivered - early, NET_SECOND, ackElapsed);
    }
    const uint64_t spread = ackElapsed + stamp->deliveredAge;
    if (spread != 0u)
    {
        const uint64_t within = NetScale(sample->delivered, NET_SECOND, spread);
        if (within > arrival)
            arrival = within;
    }
    /* О приходе ничего не известно: остаётся скорость отправки. */
    sample->proven = arrival != 0u && arrival < sample->rate ? arrival : sample->rate;
}

void NetRateLimited(NetRate *rate, uint64_t inFlight)
{
    const uint64_t mark = rate->delivered + inFlight;
    rate->appLimited = mark != 0u ? mark : 1u;
}
