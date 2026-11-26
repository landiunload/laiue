#include "net/net_internal.h"

#include <string.h>

/* BBRv3 (draft-ietf-ccwg-bbr-07): управление перегрузкой по модели пути.
 *
 * CUBIC узнаёт о пределе пути по потере и потому принимает за перегрузку и
 * случайную потерю. BBR меряет сам путь: наибольшую скорость доставки и
 * наименьшее время оборота. Их произведение — объём, который путь держит без
 * очереди; темп отправки следует скорости, а окно — небольшому кратному
 * этого объёма. Потери меняют модель, только когда их доля за оборот выше
 * двух процентов.
 *
 * Состояния:
 *
 *   STARTUP  темп 2,77 скорости: она удваивается за оборот, пока три оборота
 *            подряд не вырастет меньше чем на четверть либо потери не станут
 *            выше порога;
 *   DRAIN    темп 0,5: уходит очередь, набранная в STARTUP;
 *   PROBE    цикл DOWN (0,9) — CRUISE (1,0) — REFILL (1,0) — UP (1,25): раз в
 *            две-три секунды, а на коротком пути чаще, соединение пробует
 *            слать быстрее и узнаёт, не выросла ли полоса;
 *   PROBE_RTT раз в десять секунд без нового наименьшего оборота объём в пути
 *            на 200 мс падает до половины: без своей очереди оборот виден
 *            настоящим.
 *
 * Модель двух масштабов. Долгий — наибольшая скорость двух последних циклов
 * и inflightLong, объём, при котором проба встретила потери выше порога.
 * Короткий — bwShort и inflightShort: вне пробы оборот с потерями опускает
 * их к тому, что путь в этом обороте доставил, но не ниже 0,7 прежнего;
 * следующая проба их сбрасывает.
 *
 * Арифметика целая: симулятор обязан давать одну и ту же сеть на любой
 * платформе. Множители — проценты. */

#define BBR_INFINITE UINT64_MAX
#define BBR_PERCENT 100u

#define BBR_STARTUP_PACING 277u
#define BBR_DRAIN_PACING 50u
#define BBR_DOWN_PACING 90u
#define BBR_CRUISE_PACING 100u
#define BBR_UP_PACING 125u
#define BBR_WINDOW_GAIN 200u
#define BBR_UP_WINDOW_GAIN 225u
#define BBR_PROBE_RTT_WINDOW_GAIN 50u
/* Темп на процент ниже скорости: очередь на узком месте не копится. */
#define BBR_PACING_MARGIN 1u
/* Доля потерь за оборот, которую проба полосы считает допустимой. */
#define BBR_LOSS_THRESHOLD 2u
#define BBR_BETA 70u
/* Доля inflightLong, оставленная свободной другим потокам. */
#define BBR_HEADROOM 15u
/* Наименьшее окно в датаграммах: получатель подтверждает через пакет. */
#define BBR_MIN_PIPE 4u
#define BBR_FULL_BW_GROWTH 125u
#define BBR_FULL_BW_ROUNDS 3u
/* Серий потерь за оборот, с которых STARTUP считает очередь полной: одна
 * длинная серия — случай, а не перегрузка. */
#define BBR_STARTUP_LOSS_EVENTS 6u
#define BBR_LOSS_ROUND_SAMPLES 8u
#define BBR_DRAIN_ROUNDS 3u
/* Оборотов между пробами не больше, чем понадобилось бы Reno. */
#define BBR_RENO_ROUNDS 63u
#define BBR_UP_ROUNDS_LIMIT 30u
#define BBR_MIN_RTT_WINDOW (UINT64_C(10) * NET_SECOND)
#define BBR_PROBE_RTT_DURATION (UINT64_C(200) * NET_MILLISECOND)
#define BBR_PROBE_WAIT (UINT64_C(2) * NET_SECOND)
#define BBR_PROBE_WAIT_SPREAD NET_SECOND
#define BBR_QUANTUM_LIMIT (UINT64_C(64) * 1024u)

#define BBR_UNDO_NONE 0u
#define BBR_UNDO_STARTUP 1u
#define BBR_UNDO_UP 2u

static uint64_t Smaller(uint64_t first, uint64_t second)
{
    return first < second ? first : second;
}

static uint64_t Larger(uint64_t first, uint64_t second)
{
    return first > second ? first : second;
}

static uint64_t Sum(uint64_t first, uint64_t second)
{
    return first <= UINT64_MAX - second ? first + second : UINT64_MAX;
}

static uint64_t Bounded(uint64_t window)
{
    return window < NET_CONGESTION_WINDOW_LIMIT ? window : NET_CONGESTION_WINDOW_LIMIT;
}

/* SplitMix64. */
static uint64_t Random(NetBbr *bbr)
{
    bbr->random += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t value = bbr->random;
    value = (value ^ (value >> 30u)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27u)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31u);
}

static bool InProbeBw(const NetBbr *bbr)
{
    return bbr->state >= NET_BBR_PROBE_DOWN && bbr->state <= NET_BBR_PROBE_UP;
}

/* Соединение нарочно шлёт быстрее пути: потери тут — цена пробы. */
static bool ProbingBw(const NetBbr *bbr)
{
    return bbr->state == NET_BBR_STARTUP || bbr->state == NET_BBR_PROBE_REFILL ||
           bbr->state == NET_BBR_PROBE_UP;
}

static bool WindowLimited(const NetBbr *bbr)
{
    return bbr->windowFilled || bbr->windowFilledPrior;
}

static uint64_t MinPipe(const NetBbr *bbr)
{
    return (uint64_t)BBR_MIN_PIPE * bbr->datagramBytes;
}

/* ---- Объёмы ------------------------------------------------------------------------- */

/* Объём, который путь держит без очереди при скорости bw. */
static uint64_t Bdp(const NetBbr *bbr, uint64_t bw)
{
    /* Оборот ещё не измерен. */
    if (bbr->minRtt == BBR_INFINITE)
        return bbr->initialWindow;
    return NetScale(bw, bbr->minRtt, NET_SECOND);
}

static uint64_t BdpMultiple(const NetBbr *bbr, uint64_t bw, uint32_t gain)
{
    if (bbr->minRtt == BBR_INFINITE)
        return bbr->initialWindow;
    return NetScale(Bdp(bbr, bw), gain, BBR_PERCENT);
}

uint64_t NetBbrQuantum(const NetBbr *bbr)
{
    const uint64_t quantum = bbr->pacingRate / 1000u;
    if (quantum > BBR_QUANTUM_LIMIT)
        return BBR_QUANTUM_LIMIT;
    return Larger(quantum, 2u * (uint64_t)bbr->datagramBytes);
}

/* Объём не меньше пачки и наименьшего окна; проба вверх получает две
 * датаграммы сверху, иначе на малом объёме она не поднялась бы над ним. */
static uint64_t Quantized(const NetBbr *bbr, uint64_t inflight)
{
    inflight = Larger(inflight, NetBbrQuantum(bbr));
    inflight = Larger(inflight, MinPipe(bbr));
    if (bbr->state == NET_BBR_PROBE_UP)
        inflight = Sum(inflight, 2u * (uint64_t)bbr->datagramBytes);
    return inflight;
}

static uint64_t Inflight(const NetBbr *bbr, uint64_t bw, uint32_t gain)
{
    return Quantized(bbr, BdpMultiple(bbr, bw, gain));
}

/* Сколько соединение хочет держать в пути: объём пути, если окно не меньше. */
static uint64_t TargetInflight(const NetBbr *bbr)
{
    return Smaller(Bdp(bbr, bbr->bw), bbr->window);
}

static uint64_t InflightWithHeadroom(const NetBbr *bbr)
{
    if (bbr->inflightLong == BBR_INFINITE)
        return BBR_INFINITE;
    const uint64_t headroom =
        Larger(bbr->datagramBytes, NetScale(bbr->inflightLong, BBR_HEADROOM, BBR_PERCENT));
    const uint64_t remaining = bbr->inflightLong > headroom ? bbr->inflightLong - headroom : 0u;
    return Larger(remaining, MinPipe(bbr));
}

static uint64_t ProbeRttWindow(const NetBbr *bbr)
{
    const uint64_t window = BdpMultiple(bbr, bbr->bw, BBR_PROBE_RTT_WINDOW_GAIN);
    return Larger(Smaller(window, bbr->probeRttWindow), MinPipe(bbr));
}

/* Потери за полёт пакета выше порога. */
static bool InflightTooHigh(uint64_t lost, uint64_t txInFlight)
{
    return NetScale(lost, BBR_PERCENT, BBR_LOSS_THRESHOLD) > txInFlight;
}

/* ---- Темп и окно -------------------------------------------------------------------- */

static void InitPacingRate(NetBbr *bbr, uint64_t smoothedRtt)
{
    const uint64_t nominal = NetScale(bbr->initialWindow, NET_SECOND, smoothedRtt);
    bbr->pacingRate = NetScale(nominal, BBR_STARTUP_PACING, BBR_PERCENT);
    bbr->paced = true;
}

static void SetPacingRate(NetBbr *bbr, uint32_t gain)
{
    const uint64_t rate = NetScale(bbr->bw, (uint64_t)gain * (BBR_PERCENT - BBR_PACING_MARGIN),
                                   (uint64_t)BBR_PERCENT * BBR_PERCENT);
    /* Пока полоса не найдена, темп только растёт: замеры, ограниченные
     * приложением, его не снижают. */
    if (bbr->fullBwReached || rate > bbr->pacingRate)
        bbr->pacingRate = rate;
}

static void SaveWindow(NetBbr *bbr)
{
    if (!bbr->inRecovery && bbr->state != NET_BBR_PROBE_RTT)
        bbr->priorWindow = bbr->window;
    else
        bbr->priorWindow = Larger(bbr->priorWindow, bbr->window);
}

static void RestoreWindow(NetBbr *bbr)
{
    bbr->window = Larger(bbr->window, bbr->priorWindow);
}

static void SetWindow(NetBbr *bbr, const NetBbrAck *ack)
{
    const uint64_t maxInflight =
        Quantized(bbr, Sum(BdpMultiple(bbr, bbr->bw, bbr->windowGain), bbr->extraAcked));
    /* Окно растёт не быстрее подтверждений, а выше нужного модели падает
     * сразу. Пока полоса не найдена, модели верить рано, и сверху окно она не
     * держит. */
    if (bbr->fullBwReached)
        bbr->window = Smaller(Sum(bbr->window, ack->acked), maxInflight);
    else if (bbr->window < maxInflight || ack->delivered < bbr->initialWindow)
        bbr->window = Sum(bbr->window, ack->acked);
    bbr->window = Larger(bbr->window, MinPipe(bbr));
    if (bbr->state == NET_BBR_PROBE_RTT)
        bbr->window = Smaller(bbr->window, ProbeRttWindow(bbr));

    /* Пределы, найденные по потерям. */
    uint64_t cap = BBR_INFINITE;
    if (bbr->state == NET_BBR_PROBE_RTT || bbr->state == NET_BBR_PROBE_CRUISE)
        cap = InflightWithHeadroom(bbr);
    else if (InProbeBw(bbr))
        cap = bbr->inflightLong;
    cap = Larger(Smaller(cap, bbr->inflightShort), MinPipe(bbr));
    bbr->window = Bounded(Smaller(bbr->window, cap));
}

/* ---- Обороты и фильтры -------------------------------------------------------------- */

static void StartRound(NetBbr *bbr)
{
    bbr->nextRoundDelivered = bbr->delivered;
}

static void UpdateRound(NetBbr *bbr, const NetBbrAck *ack)
{
    bbr->roundStart = ack->priorDelivered >= bbr->nextRoundDelivered;
    if (!bbr->roundStart)
        return;
    StartRound(bbr);
    ++bbr->roundCount;
    if (bbr->roundsSinceProbe < UINT32_MAX)
        ++bbr->roundsSinceProbe;
    bbr->windowFilledPrior = bbr->windowFilled;
    bbr->windowFilled = false;
    bbr->extraIndex = (bbr->extraIndex + 1u) % (NET_BBR_EXTRA_ROUNDS + 1u);
    bbr->extraRing[bbr->extraIndex] = 0u;
    if (bbr->extraSpan < NET_BBR_EXTRA_ROUNDS)
        ++bbr->extraSpan;
}

static void UpdateMaxBw(NetBbr *bbr, const NetBbrAck *ack)
{
    UpdateRound(bbr, ack);
    /* О полосе пути говорит скорость, которую он заведомо выдал: скорость
     * доставки бывает выше пути на датаграмму за оборот (net_rate.c), и
     * наибольшая из таких держала бы темп выше пути весь ровный ход. Замер,
     * ограниченный приложением, говорит о пути, только если он выше оценки. */
    if (ack->proven == 0u || (ack->proven < bbr->maxBw && ack->appLimited))
        return;
    bbr->maxBwSlots[bbr->cycle] = Larger(bbr->maxBwSlots[bbr->cycle], ack->proven);
    bbr->maxBw = Larger(bbr->maxBwSlots[0], bbr->maxBwSlots[1]);
}

/* Новый цикл проб: замеры позапрошлого забываются. Цикл без единого замера
 * прежних не вытесняет, иначе оценка упала бы до нуля. */
static void AdvanceMaxBwFilter(NetBbr *bbr)
{
    if (bbr->maxBwSlots[bbr->cycle] == 0u)
        return;
    bbr->cycle = (uint8_t)(bbr->cycle ^ 1u);
    bbr->maxBwSlots[bbr->cycle] = 0u;
    bbr->maxBw = Larger(bbr->maxBwSlots[0], bbr->maxBwSlots[1]);
}

/* Подтверждения приходят пачками: сколько их пришло сверх ожидаемого по bw.
 * На столько растёт окно, иначе в тишине между пачками слать было бы нечем. */
static void UpdateAckAggregation(NetBbr *bbr, const NetBbrAck *ack)
{
    /* Оценка полосы ниже пути не больше чем на датаграмму за оборот
     * (net_rate.c). Ожидаемое считается по верхней границе: по самой оценке
     * доставленное всё время шло бы сверх ожидаемого, и избыток рос бы без
     * конца. */
    uint64_t rate = bbr->bw;
    if (bbr->minRtt != BBR_INFINITE)
        rate = Sum(rate, NetScale(bbr->datagramBytes, NET_SECOND, bbr->minRtt));
    uint64_t expected = NetScale(rate, ack->now - bbr->extraStart, NET_SECOND);
    /* Подтверждения отстали от ожидаемого: пачка кончилась. */
    if (bbr->extraDelivered <= expected)
    {
        bbr->extraDelivered = 0u;
        bbr->extraStart = ack->now;
        expected = 0u;
    }
    bbr->extraDelivered += ack->acked;
    const uint64_t extra = Smaller(bbr->extraDelivered - expected, bbr->window);
    bbr->extraRing[bbr->extraIndex] = Larger(bbr->extraRing[bbr->extraIndex], extra);
    /* Пока полоса не найдена, оценка скорости отстаёт от пути и избыток
     * завышен: помнится один прошлый оборот, и в окно найденной полосы эти
     * обороты уже не попадут. */
    if (!bbr->fullBwReached && bbr->extraSpan > 1u)
        bbr->extraSpan = 1u;
    uint64_t largest = 0u;
    for (uint32_t back = 0u; back <= bbr->extraSpan; ++back)
    {
        const uint32_t index =
            (bbr->extraIndex + NET_BBR_EXTRA_ROUNDS + 1u - back) % (NET_BBR_EXTRA_ROUNDS + 1u);
        largest = Larger(largest, bbr->extraRing[index]);
    }
    bbr->extraAcked = largest;
}

/* ---- Потери и модель ---------------------------------------------------------------- */

static void ResetCongestionSignals(NetBbr *bbr)
{
    bbr->lossInRound = false;
    bbr->lossEvents = 0u;
    bbr->bwLatest = 0u;
    bbr->inflightLatest = 0u;
}

static void ResetShortTermModel(NetBbr *bbr)
{
    bbr->bwShort = BBR_INFINITE;
    bbr->inflightShort = BBR_INFINITE;
}

static void ResetFullBw(NetBbr *bbr)
{
    bbr->fullBw = 0u;
    bbr->fullBwCount = 0u;
    bbr->fullBwNow = false;
}

/* Скорость доставки в счёт модели. Темп на процент ниже оценки — правило
 * отправки, а не сведения о пути: путь, доставивший всё в этом темпе, выдал
 * то, что от него просили. Без поправки короткая модель после каждого
 * оборота с потерями опускалась бы ещё на процент, и проценты копились бы до
 * следующей пробы. Замер черновика это прятал: наибольший из его шумных
 * замеров выше темпа; точный замер (net_rate.c) — нет. */
static uint64_t Credited(uint64_t rate)
{
    return NetScale(rate, BBR_PERCENT, BBR_PERCENT - BBR_PACING_MARGIN);
}

static void UpdateLatestDeliverySignals(NetBbr *bbr, const NetBbrAck *ack)
{
    bbr->bwLatest = Larger(bbr->bwLatest, Credited(ack->rate));
    bbr->inflightLatest = Larger(bbr->inflightLatest, ack->sampleDelivered);
    if (bbr->lossRoundSamples < UINT32_MAX)
        ++bbr->lossRoundSamples;
    /* Оборот потерь не короче оборота пакетов и нескольких замеров. Когда
     * подтверждения редки, один замер на оборот шумит на долю шага
     * обновления, и короткая модель, идущая за ним, только падала бы:
     * сниженный темп не даёт следующему замеру быть выше. */
    bbr->lossRoundStart = ack->priorDelivered >= bbr->lossRoundDelivered &&
                          bbr->lossRoundSamples >= BBR_LOSS_ROUND_SAMPLES;
    if (bbr->lossRoundStart)
    {
        bbr->lossRoundDelivered = ack->delivered;
        bbr->lossRoundSamples = 0u;
    }
}

static void UpdateCongestionSignals(NetBbr *bbr, const NetBbrAck *ack)
{
    UpdateMaxBw(bbr, ack);
    if (!bbr->lossRoundStart)
        return;
    /* Оборот с потерями вне пробы: путь, видимо, стал держать меньше, и
     * короткая модель идёт к тому, что он в этом обороте доставил. */
    if (bbr->lossInRound && !ProbingBw(bbr))
    {
        if (bbr->bwShort == BBR_INFINITE)
            bbr->bwShort = bbr->maxBw;
        if (bbr->inflightShort == BBR_INFINITE)
            bbr->inflightShort = bbr->window;
        bbr->bwShort = Larger(bbr->bwLatest, NetScale(bbr->bwShort, BBR_BETA, BBR_PERCENT));
        bbr->inflightShort =
            Larger(bbr->inflightLatest, NetScale(bbr->inflightShort, BBR_BETA, BBR_PERCENT));
    }
    bbr->lossInRound = false;
}

static void AdvanceLatestDeliverySignals(NetBbr *bbr, const NetBbrAck *ack)
{
    if (!bbr->lossRoundStart)
        return;
    bbr->bwLatest = Credited(ack->rate);
    bbr->inflightLatest = ack->sampleDelivered;
    bbr->lossEvents = 0u;
}

/* Скорость перестала расти: три оборота подряд меньше чем на четверть. */
static void CheckFullBwReached(NetBbr *bbr, const NetBbrAck *ack)
{
    if (bbr->fullBwNow || !bbr->roundStart || ack->appLimited)
        return;
    if (ack->rate >= NetScale(bbr->fullBw, BBR_FULL_BW_GROWTH, BBR_PERCENT))
    {
        ResetFullBw(bbr);
        bbr->fullBw = ack->rate;
        return;
    }
    ++bbr->fullBwCount;
    bbr->fullBwNow = bbr->fullBwCount >= BBR_FULL_BW_ROUNDS;
    if (bbr->fullBwNow)
        bbr->fullBwReached = true;
}

/* ---- Состояния ---------------------------------------------------------------------- */

static void EnterStartup(NetBbr *bbr)
{
    bbr->state = NET_BBR_STARTUP;
    bbr->pacingGain = BBR_STARTUP_PACING;
    bbr->windowGain = BBR_WINDOW_GAIN;
}

static void StartProbeDown(NetBbr *bbr, uint64_t now)
{
    ResetCongestionSignals(bbr);
    bbr->upAckedPerStep = BBR_INFINITE;
    /* Случайны и срок по часам, и счёт оборотов: потоки одного узкого места
     * не пробуют полосу разом. */
    const uint64_t random = Random(bbr);
    bbr->roundsSinceProbe = (uint32_t)(random & 1u);
    bbr->probeWait = BBR_PROBE_WAIT + (random >> 1u) % BBR_PROBE_WAIT_SPREAD;
    bbr->cycleStamp = now;
    bbr->probeStopping = true;
    StartRound(bbr);
    bbr->state = NET_BBR_PROBE_DOWN;
    bbr->pacingGain = BBR_DOWN_PACING;
    bbr->windowGain = BBR_WINDOW_GAIN;
}

static void StartProbeCruise(NetBbr *bbr)
{
    bbr->state = NET_BBR_PROBE_CRUISE;
    bbr->pacingGain = BBR_CRUISE_PACING;
    bbr->windowGain = BBR_WINDOW_GAIN;
}

/* Перед пробой путь наполняется до оценки, а короткая модель забывается:
 * потери чужих потоков больше не держат соединение. */
static void StartProbeRefill(NetBbr *bbr)
{
    ResetShortTermModel(bbr);
    bbr->upRounds = 0u;
    bbr->upAcked = 0u;
    bbr->probePrecautionary = false;
    bbr->probeStopping = false;
    StartRound(bbr);
    bbr->state = NET_BBR_PROBE_REFILL;
    bbr->pacingGain = BBR_CRUISE_PACING;
    bbr->windowGain = BBR_WINDOW_GAIN;
}

/* Прирост inflightLong удваивается с каждым оборотом пробы: 1, 2, 4...
 * датаграмм, поровну на весь оборот. */
static void RaiseInflightSlope(NetBbr *bbr)
{
    const uint64_t growth = UINT64_C(1) << bbr->upRounds;
    if (bbr->upRounds < BBR_UP_ROUNDS_LIMIT)
        ++bbr->upRounds;
    bbr->upAckedPerStep = Larger(bbr->window / growth, bbr->datagramBytes);
}

static void StartProbeUp(NetBbr *bbr, uint64_t rate)
{
    StartRound(bbr);
    ResetFullBw(bbr);
    bbr->fullBw = rate;
    bbr->state = NET_BBR_PROBE_UP;
    bbr->pacingGain = BBR_UP_PACING;
    bbr->windowGain = BBR_UP_WINDOW_GAIN;
    RaiseInflightSlope(bbr);
}

static void EnterDrain(NetBbr *bbr)
{
    bbr->state = NET_BBR_DRAIN;
    bbr->pacingGain = BBR_DRAIN_PACING;
    bbr->windowGain = BBR_WINDOW_GAIN;
    bbr->drainStartRound = bbr->roundCount;
}

/* Потери за последний оборот STARTUP выше порога: очередь узкого места
 * полна, и полоса найдена, хотя замеры скорости об этом ещё не сказали. */
static void CheckStartupHighLoss(NetBbr *bbr, const NetBbrAck *ack)
{
    if (bbr->fullBwReached || !bbr->lossRoundStart || !bbr->inRecovery ||
        bbr->lossEvents < BBR_STARTUP_LOSS_EVENTS || !InflightTooHigh(ack->lost, ack->txInFlight))
        return;
    bbr->undoState = BBR_UNDO_STARTUP;
    bbr->fullBwReached = true;
    bbr->inflightLong = Larger(Bdp(bbr, bbr->bw), bbr->inflightLatest);
}

static void ProbeInflightUpward(NetBbr *bbr, const NetBbrAck *ack)
{
    /* Предел растёт, только пока соединение в него упирается. */
    if (!WindowLimited(bbr) || bbr->window < bbr->inflightLong)
        return;
    bbr->upAcked += ack->acked;
    if (bbr->upAcked >= bbr->upAckedPerStep)
    {
        const uint64_t steps = bbr->upAcked / bbr->upAckedPerStep;
        bbr->upAcked -= steps * bbr->upAckedPerStep;
        bbr->inflightLong = Bounded(Sum(bbr->inflightLong, steps * bbr->datagramBytes));
    }
    if (bbr->roundStart)
        RaiseInflightSlope(bbr);
}

/* Долгая модель по подтверждениям пробы. true — состояние сменилось. */
static bool AdaptLongTermModel(NetBbr *bbr, const NetBbrAck *ack)
{
    if (bbr->probeStopping && bbr->roundStart)
    {
        /* Подтверждения пробы кончились. */
        bbr->checkProbeLoss = false;
        bbr->probeStopping = false;
        if (InProbeBw(bbr) && !ack->appLimited)
            AdvanceMaxBwFilter(bbr);
        /* Проба, прерванная у прежнего предела, обошлась без потерь выше
         * порога: следующая идёт сразу и предела уже не боится. */
        if (InProbeBw(bbr) && bbr->probePrecautionary && !bbr->probeTooHigh)
        {
            StartProbeRefill(bbr);
            return true;
        }
    }
    if (InflightTooHigh(ack->lost, ack->txInFlight) || bbr->inflightLong == BBR_INFINITE)
        return false;
    bbr->inflightLong = Larger(bbr->inflightLong, ack->txInFlight);
    if (bbr->state == NET_BBR_PROBE_UP)
        ProbeInflightUpward(bbr, ack);
    return false;
}

static bool TimeToProbeBw(NetBbr *bbr, uint64_t now)
{
    /* Reno вернул бы себе такое окно за столько же оборотов, сколько в нём
     * датаграмм: чаще пробовать — отнимать полосу у его потоков. */
    uint64_t rounds = (TargetInflight(bbr) + bbr->datagramBytes - 1u) / bbr->datagramBytes;
    rounds = Larger(Smaller(rounds, BBR_RENO_ROUNDS), 1u);
    if (now <= bbr->cycleStamp + bbr->probeWait && bbr->roundsSinceProbe < rounds)
        return false;
    StartProbeRefill(bbr);
    return true;
}

static bool TimeToCruise(const NetBbr *bbr, const NetBbrAck *ack)
{
    return ack->inFlight <= InflightWithHeadroom(bbr) &&
           ack->inFlight <= Inflight(bbr, bbr->maxBw, BBR_PERCENT);
}

static bool TimeToGoDown(NetBbr *bbr, const NetBbrAck *ack)
{
    /* Прошлая проба у этого объёма встретила потери: дольше тут не стоят. */
    if (bbr->probeTooHigh && ack->inFlight >= bbr->inflightLong)
    {
        bbr->probePrecautionary = true;
        return true;
    }
    /* Скорость держит не путь, а предел объёма: о полосе её плато не
     * говорит. */
    if (WindowLimited(bbr) && bbr->window >= bbr->inflightLong)
    {
        ResetFullBw(bbr);
        bbr->fullBw = ack->rate;
        return false;
    }
    return bbr->fullBwNow;
}

static void UpdateProbeBwCyclePhase(NetBbr *bbr, const NetBbrAck *ack)
{
    if (!bbr->fullBwReached || AdaptLongTermModel(bbr, ack))
        return;
    switch (bbr->state)
    {
    case NET_BBR_PROBE_DOWN:
        if (!TimeToProbeBw(bbr, ack->now) && TimeToCruise(bbr, ack))
            StartProbeCruise(bbr);
        break;
    case NET_BBR_PROBE_CRUISE:
        (void)TimeToProbeBw(bbr, ack->now);
        break;
    case NET_BBR_PROBE_REFILL:
        /* Путь наполнялся оборот: дальше потери говорят о пробе. */
        if (bbr->roundStart)
        {
            bbr->checkProbeLoss = true;
            StartProbeUp(bbr, ack->rate);
        }
        break;
    case NET_BBR_PROBE_UP:
        if (TimeToGoDown(bbr, ack))
        {
            bbr->probeTooHigh = false;
            StartProbeDown(bbr, ack->now);
        }
        break;
    default:
        break;
    }
}

static void UpdateMinRtt(NetBbr *bbr, const NetBbrAck *ack)
{
    bbr->minRttExpired = ack->now > bbr->minRttStamp + BBR_MIN_RTT_WINDOW;
    /* Окно на время замера считается по прежнему обороту: новый, с очередью
     * в пути, оставил бы в нём слишком много. */
    if (bbr->minRttExpired)
        bbr->probeRttWindow = BdpMultiple(bbr, bbr->bw, BBR_PROBE_RTT_WINDOW_GAIN);
    if (ack->rttSample != 0u && (ack->rttSample < bbr->minRtt || bbr->minRttExpired))
    {
        bbr->minRtt = ack->rttSample;
        bbr->minRttStamp = ack->now;
    }
}

static void ExitProbeRtt(NetBbr *bbr, uint64_t now)
{
    ResetShortTermModel(bbr);
    if (!bbr->fullBwReached)
    {
        EnterStartup(bbr);
        return;
    }
    /* Отсчёт до пробы полосы начинается заново, а очереди после замера нет:
     * спускать нечего. */
    StartProbeDown(bbr, now);
    StartProbeCruise(bbr);
}

static void CheckProbeRttDone(NetBbr *bbr, uint64_t now)
{
    if (bbr->probeRttDone == 0u || now <= bbr->probeRttDone)
        return;
    bbr->minRttStamp = now;
    RestoreWindow(bbr);
    ExitProbeRtt(bbr, now);
}

static void CheckProbeRtt(NetBbr *bbr, const NetBbrAck *ack)
{
    /* Тишина сама опустошила путь: после неё замер не нужен. */
    if (bbr->state != NET_BBR_PROBE_RTT && bbr->minRttExpired && !bbr->idleRestart)
    {
        SaveWindow(bbr);
        bbr->state = NET_BBR_PROBE_RTT;
        bbr->pacingGain = BBR_CRUISE_PACING;
        bbr->windowGain = BBR_PROBE_RTT_WINDOW_GAIN;
        bbr->probeRttDone = 0u;
        bbr->probeStopping = true;
        StartRound(bbr);
    }
    if (bbr->state == NET_BBR_PROBE_RTT)
    {
        if (bbr->probeRttDone == 0u)
        {
            /* Объём упал: теперь замер длится 200 мс и не меньше оборота. */
            if (ack->inFlight <= ProbeRttWindow(bbr))
            {
                bbr->probeRttDone = ack->now + BBR_PROBE_RTT_DURATION;
                bbr->probeRttRoundDone = false;
                StartRound(bbr);
            }
        }
        else
        {
            if (bbr->roundStart)
                bbr->probeRttRoundDone = true;
            if (bbr->probeRttRoundDone)
                CheckProbeRttDone(bbr, ack->now);
        }
    }
    if (ack->sampleDelivered != 0u)
        bbr->idleRestart = false;
}

/* ---- События ------------------------------------------------------------------------ */

void NetBbrReset(NetBbr *bbr, uint32_t datagramBytes, uint64_t window, uint64_t seed, uint64_t now)
{
    memset(bbr, 0, sizeof(*bbr));
    bbr->datagramBytes = datagramBytes;
    bbr->window = window;
    bbr->initialWindow = window;
    bbr->random = seed;
    bbr->since = now;
    bbr->minRtt = BBR_INFINITE;
    bbr->minRttStamp = now;
    bbr->extraStart = now;
    bbr->inflightLong = BBR_INFINITE;
    bbr->upAckedPerStep = BBR_INFINITE;
    bbr->probeRttWindow = BBR_INFINITE;
    ResetShortTermModel(bbr);
    EnterStartup(bbr);
}

void NetBbrSeed(NetBbr *bbr, uint64_t smoothedRtt)
{
    if (!bbr->paced && smoothedRtt != 0u)
        InitPacingRate(bbr, smoothedRtt);
}

void NetBbrOnSend(NetBbr *bbr, uint64_t inFlight, uint32_t size, bool limited, uint64_t now)
{
    /* Отправка после тишины. Путь пуст, и возвращаться к прежнему объёму
     * надо ровно со скоростью пути, а не с множителем состояния. */
    if (inFlight == 0u && limited)
    {
        bbr->idleRestart = true;
        bbr->extraStart = now;
        if (InProbeBw(bbr))
            SetPacingRate(bbr, BBR_CRUISE_PACING);
        else if (bbr->state == NET_BBR_PROBE_RTT)
            CheckProbeRttDone(bbr, now);
    }
    if (inFlight + size + bbr->datagramBytes > bbr->window)
        bbr->windowFilled = true;
}

void NetBbrOnAck(NetBbr *bbr, const NetBbrAck *ack)
{
    bbr->delivered = ack->delivered;
    /* Пакет ушёл по прежнему пути: ни его скорость, ни право растить окно к
     * нынешнему не относятся. */
    if (ack->sentTime < bbr->since)
        return;
    /* Восстановление кончается подтверждением пакета, отправленного после
     * его начала. Модель этого подтверждения ещё видит соединение в нём:
     * оборот потерь кончается тем же пакетом. */
    const bool recovered = bbr->inRecovery && ack->sentTime > bbr->recoveryStart;

    UpdateLatestDeliverySignals(bbr, ack);
    UpdateCongestionSignals(bbr, ack);
    UpdateAckAggregation(bbr, ack);
    CheckFullBwReached(bbr, ack);
    CheckStartupHighLoss(bbr, ack);
    if (bbr->state == NET_BBR_STARTUP && bbr->fullBwReached)
        EnterDrain(bbr);
    /* Очередь обычно уходит за оборот; дольше трёх её не ждут: значит,
     * завышена сама оценка полосы, и исправит её цикл проб. */
    if (bbr->state == NET_BBR_DRAIN && (ack->inFlight <= Inflight(bbr, bbr->bw, BBR_PERCENT) ||
                                        bbr->roundCount > bbr->drainStartRound + BBR_DRAIN_ROUNDS))
        StartProbeDown(bbr, ack->now);
    UpdateProbeBwCyclePhase(bbr, ack);
    UpdateMinRtt(bbr, ack);
    CheckProbeRtt(bbr, ack);
    AdvanceLatestDeliverySignals(bbr, ack);
    bbr->bw = Smaller(bbr->maxBw, bbr->bwShort);

    if (recovered)
    {
        bbr->inRecovery = false;
        RestoreWindow(bbr);
    }
    NetBbrSeed(bbr, ack->smoothedRtt);
    SetPacingRate(bbr, bbr->pacingGain);
    SetWindow(bbr, ack);
}

bool NetBbrOnLoss(NetBbr *bbr, const NetBbrLoss *loss)
{
    /* Потерян на прежнем пути: о нынешнем это не говорит. */
    if (loss->sentTime < bbr->since)
        return false;
    const bool episode = !bbr->inRecovery || loss->sentTime > bbr->recoveryStart;
    if (episode)
    {
        /* То, что вернёт отмена, если потери эпизода окажутся мнимыми. */
        SaveWindow(bbr);
        bbr->undoState = BBR_UNDO_NONE;
        bbr->undoBwShort = bbr->bwShort;
        bbr->undoInflightShort = bbr->inflightShort;
        bbr->undoInflightLong = bbr->inflightLong;
        bbr->inRecovery = true;
        bbr->recoveryStart = loss->now;
    }
    /* Оборот потерь считается от первой из них: за него видна вся
     * пропускная способность пути. */
    if (!bbr->lossInRound)
    {
        bbr->lossRoundDelivered = bbr->delivered;
        bbr->lossRoundSamples = 0u;
    }
    bbr->lossInRound = true;
    if (!loss->contiguous && bbr->lossEvents < UINT32_MAX)
        ++bbr->lossEvents;
    if (!bbr->checkProbeLoss || !InflightTooHigh(loss->lost, loss->txInFlight))
        return episode;

    /* Проба полосы встретила потери выше порога. Потерю объявляют позже,
     * чем она случилась, поэтому предел — не объём при подтверждении, а тот,
     * на котором доля потерь перешла порог: до этого пакета либо на его
     * части. */
    const uint64_t before = loss->txInFlight > loss->size ? loss->txInFlight - loss->size : 0u;
    const uint64_t lostBefore = loss->lost > loss->size ? loss->lost - loss->size : 0u;
    const uint64_t allowed = before * BBR_LOSS_THRESHOLD;
    const uint64_t spent = lostBefore * BBR_PERCENT;
    const uint64_t prefix =
        allowed > spent ? (allowed - spent) / (BBR_PERCENT - BBR_LOSS_THRESHOLD) : 0u;
    bbr->probeTooHigh = true;
    /* На одну пробу — один ответ. */
    bbr->checkProbeLoss = false;
    if (!loss->appLimited)
        bbr->inflightLong =
            Larger(before + prefix, NetScale(TargetInflight(bbr), BBR_BETA, BBR_PERCENT));
    if (bbr->state == NET_BBR_PROBE_UP)
    {
        bbr->undoState = BBR_UNDO_UP;
        StartProbeDown(bbr, loss->now);
    }
    return episode;
}

void NetBbrCollapse(NetBbr *bbr, uint64_t now)
{
    bbr->window = 2u * (uint64_t)bbr->datagramBytes;
    /* Прежнее окно не возвращается: дальше его растят подтверждения. */
    bbr->priorWindow = bbr->window;
    bbr->inRecovery = true;
    bbr->recoveryStart = now;
    /* Пакеты, ушедшие до обрыва, о пути после него не говорят: ни их
     * скорость, ни право растить окно. Подтверждения, потерянные вместе с
     * ними, иначе разом вернули бы окно, принятое ещё до обрыва. */
    bbr->since = now;
}

void NetBbrUndo(NetBbr *bbr)
{
    bbr->inRecovery = false;
    RestoreWindow(bbr);
    bbr->lossInRound = false;
    bbr->lossEvents = 0u;
    ResetFullBw(bbr);
    bbr->bwShort = Larger(bbr->bwShort, bbr->undoBwShort);
    bbr->inflightShort = Larger(bbr->inflightShort, bbr->undoInflightShort);
    bbr->inflightLong = Larger(bbr->inflightLong, bbr->undoInflightLong);
    /* Мнимые потери прервали поиск полосы: он продолжается. */
    if (bbr->undoState == BBR_UNDO_STARTUP && bbr->state != NET_BBR_STARTUP)
    {
        bbr->fullBwReached = false;
        if (bbr->state != NET_BBR_PROBE_RTT)
            EnterStartup(bbr);
    }
    else if (bbr->undoState == BBR_UNDO_UP && bbr->state != NET_BBR_PROBE_UP &&
             bbr->state != NET_BBR_PROBE_RTT)
    {
        StartProbeRefill(bbr);
    }
    bbr->undoState = BBR_UNDO_NONE;
}

void NetBbrSetDatagram(NetBbr *bbr, uint32_t datagramBytes)
{
    bbr->datagramBytes = datagramBytes;
    bbr->window = Larger(bbr->window, 2u * (uint64_t)datagramBytes);
}
