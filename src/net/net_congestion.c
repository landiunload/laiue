#include "net/net_internal.h"

#include <string.h>

/* Окно перегрузки: CUBIC (RFC 9438) с медленным стартом HyStart++
 * (RFC 9406).
 *
 * Медленный старт удваивает окно за оборот и следит за задержкой. Когда
 * наименьшее время оборота раунда вырастает на восьмую часть (от 4 до 16 мс),
 * на узком месте копится очередь, и рост становится вчетверо осторожнее.
 * Пять осторожных раундов без возврата задержки — и окно переходит к CUBIC,
 * не переполнив очередь, как это делал бы классический старт. Возврат
 * задержки значит, что рост был лишь шумом, и старт продолжается.
 *
 * Сверх RFC: если и в следующем раунде задержка вырастает на порог, очередь
 * копится наверняка, и старт кончается сразу. Рост задержки заметен лишь
 * через пару раундов, когда окно уже в разы больше пути, и пять осторожных
 * раундов почти утроили бы его, переполнив очередь глубиной в пять оборотов.
 *
 * CUBIC после потери сокращает окно до 0,7 и возвращает его к прежнему
 * максимуму по кубической кривой от времени, а не от числа оборотов: быстро
 * вдали от максимума, осторожно рядом с ним и всё быстрее за ним, в поисках
 * нового. Длинный широкий путь он восстанавливает за секунды там, где Reno
 * тратил бы минуты, а на коротком обороте окно ведёт оценка Reno, и CUBIC
 * не агрессивнее него. Время кривой идёт, только пока окно тесно:
 * приложение, которому нечего слать, не копит права на рывок. */

/* β = 0,7 и быстрая сходимость (1 + β) / 2 = 0,85. */
#define CUBIC_BETA_NUMERATOR 7u
#define CUBIC_BETA_DENOMINATOR 10u
#define CUBIC_CONVERGENCE_NUMERATOR 17u
#define CUBIC_CONVERGENCE_DENOMINATOR 20u
/* α оценки Reno: 3(1 − β) / (1 + β) = 9/17, пока оценка не дошла до окна
 * перед сокращением, затем 1. */
#define CUBIC_ALPHA_REDUCED 9u
#define CUBIC_ALPHA_FULL 17u
#define CUBIC_ALPHA_DENOMINATOR 17u
/* C = 0,4 датаграммы за секунду в кубе. При времени d в миллисекундах
 * C·d³ = d³ / 2,5·10⁹ датаграмм, то есть d³ / 2,5·10⁶ тысячных долей. */
#define CUBIC_THOUSANDTHS_DIVISOR UINT64_C(2500000)
#define CUBIC_PLATEAU_SCALE UINT64_C(2500000000)
/* Дальше от плато кривая заведомо выше любого окна, а куб расстояния ещё
 * помещается в 64 бита. */
#define CUBIC_DISTANCE_LIMIT (UINT64_C(1) << 21u)

/* Рекомендованные значения RFC 9406, 4.3. Отправка размерена, поэтому рост
 * за подтверждение не ограничен (L = ∞). */
#define HYSTART_MIN_THRESHOLD (UINT64_C(4) * NET_MILLISECOND)
#define HYSTART_MAX_THRESHOLD (UINT64_C(16) * NET_MILLISECOND)
#define HYSTART_THRESHOLD_DIVISOR 8u
#define HYSTART_SAMPLES 8u
#define HYSTART_CAUTIOUS_DIVISOR 4u
#define HYSTART_CAUTIOUS_ROUNDS 5u

static uint64_t MinimumWindow(const NetCongestion *congestion)
{
    return 2u * (uint64_t)congestion->datagramBytes;
}

static uint64_t Bounded(uint64_t window)
{
    return window < NET_CONGESTION_WINDOW_LIMIT ? window : NET_CONGESTION_WINDOW_LIMIT;
}

uint64_t NetCubeRoot(uint64_t value)
{
    /* По три бита за шаг, старшие первыми. (r + 1)³ − r³ = 3(r² + r) + 1, а
     * сравнение со сдвигом вправо не переполняется. */
    uint64_t root = 0u;
    uint64_t square = 0u;
    for (uint32_t step = 0u; step < 22u; ++step)
    {
        const uint32_t shift = 63u - 3u * step;
        root *= 2u;
        square *= 4u;
        const uint64_t increment = 3u * (square + root) + 1u;
        if ((value >> shift) >= increment)
        {
            value -= increment << shift;
            square += 2u * root + 1u;
            ++root;
        }
    }
    return root;
}

void NetCongestionReset(NetCongestion *congestion, uint32_t datagramBytes, uint64_t window,
                        uint64_t nextNumber)
{
    memset(congestion, 0, sizeof(*congestion));
    congestion->datagramBytes = datagramBytes;
    congestion->window = window;
    congestion->threshold = UINT64_MAX;
    congestion->priorThreshold = UINT64_MAX;
    congestion->roundEnd = nextNumber;
    congestion->roundMinRtt = UINT64_MAX;
    congestion->lastRoundMinRtt = UINT64_MAX;
    congestion->baselineRtt = UINT64_MAX;
}

/* ---- HyStart++ ---------------------------------------------------------------------- */

/* Наименьший оборот раунда вырос на восьмую часть прошлого, от 4 до 16 мс:
 * на узком месте копится очередь. */
static bool DelayGrew(const NetCongestion *congestion)
{
    if (congestion->lastRoundMinRtt == UINT64_MAX)
        return false;
    uint64_t threshold = congestion->lastRoundMinRtt / HYSTART_THRESHOLD_DIVISOR;
    if (threshold < HYSTART_MIN_THRESHOLD)
        threshold = HYSTART_MIN_THRESHOLD;
    if (threshold > HYSTART_MAX_THRESHOLD)
        threshold = HYSTART_MAX_THRESHOLD;
    return congestion->roundMinRtt >= congestion->lastRoundMinRtt + threshold;
}

/* Путь заполнен, и первый медленный старт кончается без потери. Кривая CUBIC
 * ещё не начиналась, прежнего максимума у неё нет, и она сразу ищет новый от
 * нынешнего окна. */
static void SlowStartEnd(NetCongestion *congestion)
{
    congestion->cautious = false;
    congestion->threshold = congestion->window;
    congestion->cubic.priorWindow = congestion->window;
}

static void HyStart(NetCongestion *congestion, const NetCongestionAck *ack)
{
    if (ack->largest >= congestion->roundEnd)
    {
        congestion->roundEnd = ack->nextNumber;
        congestion->lastRoundMinRtt = congestion->roundMinRtt;
        congestion->roundMinRtt = UINT64_MAX;
        congestion->roundSamples = 0u;
        /* Раунд, в середине которого рост стал осторожным, тоже считается. */
        if (congestion->cautious && ++congestion->cautiousRounds >= HYSTART_CAUTIOUS_ROUNDS)
            SlowStartEnd(congestion);
    }
    /* Ведёт только первый медленный старт: следующие кончает найденный
     * порог. */
    if (ack->rttSample == 0u || congestion->threshold != UINT64_MAX)
        return;
    if (ack->rttSample < congestion->roundMinRtt)
        congestion->roundMinRtt = ack->rttSample;
    ++congestion->roundSamples;
    if (congestion->roundSamples < HYSTART_SAMPLES)
        return;
    if (!congestion->cautious)
    {
        if (DelayGrew(congestion))
        {
            congestion->cautious = true;
            congestion->cautiousRounds = 0u;
            congestion->baselineRtt = congestion->roundMinRtt;
        }
    }
    else if (congestion->roundMinRtt < congestion->baselineRtt)
    {
        /* Задержка вернулась к прежней: рост был шумом, старт продолжается. */
        congestion->cautious = false;
        congestion->baselineRtt = UINT64_MAX;
    }
    else if (congestion->cautiousRounds != 0u && DelayGrew(congestion))
    {
        /* Задержка снова выросла на порог: очередь копится раунд за раундом,
         * и это не шум. Осторожный рост длится не дольше пяти раундов; ждать
         * их до конца значило бы почти утроить окно сверх заполненного пути. */
        SlowStartEnd(congestion);
    }
}

/* ---- CUBIC ---------------------------------------------------------------------------- */

/* W_cubic(t) = C·(t − K)³ + W_max; t в микросекундах. */
static uint64_t CubicWindow(const NetCongestion *congestion, uint64_t elapsed)
{
    const NetCubic *cubic = &congestion->cubic;
    const uint64_t time = elapsed / NET_MILLISECOND;
    const bool before = time < cubic->plateau;
    const uint64_t distance = before ? cubic->plateau - time : time - cubic->plateau;
    if (distance >= CUBIC_DISTANCE_LIMIT)
        return before ? 0u : NET_CONGESTION_WINDOW_LIMIT;
    const uint64_t offset = distance * distance * distance / CUBIC_THOUSANDTHS_DIVISOR *
                            congestion->datagramBytes / 1000u;
    if (before)
        return offset < cubic->maxWindow ? cubic->maxWindow - offset : 0u;
    return Bounded(cubic->maxWindow + offset);
}

static void CubicStart(NetCongestion *congestion)
{
    NetCubic *cubic = &congestion->cubic;
    const uint64_t window = congestion->window;
    cubic->started = true;
    cubic->elapsed = 0u;
    cubic->renoWindow = window;
    if (cubic->maxWindow > window)
    {
        /* K = ∛((W_max − cwnd) / C). */
        cubic->plateau = NetCubeRoot((cubic->maxWindow - window) * CUBIC_PLATEAU_SCALE /
                                     congestion->datagramBytes);
    }
    else
    {
        /* Прежнего максимума нет либо окно уже выше него: после HyStart++ и
         * затяжной перегрузки кривая начинается с поиска нового. */
        cubic->plateau = 0u;
        cubic->maxWindow = window;
    }
}

static void CubicGrow(NetCongestion *congestion, const NetCongestionAck *ack, uint64_t bytes)
{
    NetCubic *cubic = &congestion->cubic;
    const uint64_t datagram = congestion->datagramBytes;
    if (!cubic->started)
    {
        CubicStart(congestion);
    }
    else if (congestion->lastAckLimited)
    {
        /* Время кривой идёт между подтверждениями тесного окна. Занятый путь
         * подтверждает чаще раза за оборот, а пауза дольше оборота значит,
         * что отправитель молчал: её кривая не считает (RFC 9438, 5.8). */
        uint64_t gap = ack->now - congestion->lastAckTime;
        if (gap > ack->smoothedRtt)
            gap = ack->smoothedRtt;
        cubic->elapsed += gap;
    }
    const uint64_t window = congestion->window;

    /* Оценка Reno растёт на α датаграмм за окно подтверждённых байт. */
    const uint64_t alpha =
        cubic->renoWindow >= cubic->priorWindow ? CUBIC_ALPHA_FULL : CUBIC_ALPHA_REDUCED;
    const uint64_t renoShare = alpha * datagram * bytes + congestion->renoRemainder;
    const uint64_t renoDivisor = CUBIC_ALPHA_DENOMINATOR * window;
    cubic->renoWindow = Bounded(cubic->renoWindow + renoShare / renoDivisor);
    congestion->renoRemainder = renoShare % renoDivisor;

    uint64_t grown = window;
    if (CubicWindow(congestion, cubic->elapsed) < cubic->renoWindow)
    {
        /* Область Reno: на коротком обороте кривая растёт медленнее. */
        if (grown < cubic->renoWindow)
            grown = cubic->renoWindow;
    }
    else
    {
        /* Цель — кривая через оборот, но не больше полутора окон; за окно
         * подтверждённых байт окно доходит до цели. */
        uint64_t target = CubicWindow(congestion, cubic->elapsed + ack->smoothedRtt);
        if (target > window + window / 2u)
            target = window + window / 2u;
        if (target > window)
        {
            const uint64_t share = (target - window) * bytes + congestion->windowRemainder;
            grown = window + share / window;
            congestion->windowRemainder = share % window;
            if (grown > target)
                grown = target;
        }
    }
    congestion->window = Bounded(grown);
}

/* ---- События ---------------------------------------------------------------------------- */

void NetCongestionOnAck(NetCongestion *congestion, const NetCongestionAck *ack)
{
    HyStart(congestion, ack);
    uint64_t bytes = ack->limitedBytes;
    /* Потеря, найденная этим же подтверждением, уже начала восстановление, и
     * подтверждённые им пакеты окно не расширяют (RFC 9002, A.7). */
    if (NetCongestionInRecovery(congestion, ack->limitedSentTime))
        bytes = 0u;
    if (bytes != 0u)
    {
        if (congestion->window < congestion->threshold)
        {
            const uint64_t growth = congestion->cautious ? bytes / HYSTART_CAUTIOUS_DIVISOR : bytes;
            congestion->window = Bounded(congestion->window + growth);
        }
        else
        {
            CubicGrow(congestion, ack, bytes);
        }
    }
    congestion->lastAckTime = ack->now;
    congestion->lastAckLimited = bytes != 0u;
}

bool NetCongestionOnLoss(NetCongestion *congestion, uint64_t sentTime, uint64_t now)
{
    /* Одно сокращение на полёт. */
    if (NetCongestionInRecovery(congestion, sentTime))
        return false;
    NetCubic *cubic = &congestion->cubic;
    congestion->priorCubic = *cubic;
    congestion->priorThreshold = congestion->threshold;
    /* Быстрая сходимость: окно не дотянуло до прежнего максимума, значит,
     * путь делят с другими, и место им уступается раньше. */
    if (congestion->window < cubic->maxWindow)
        cubic->maxWindow =
            congestion->window * CUBIC_CONVERGENCE_NUMERATOR / CUBIC_CONVERGENCE_DENOMINATOR;
    else
        cubic->maxWindow = congestion->window;
    cubic->priorWindow = congestion->window;
    cubic->started = false;
    uint64_t threshold = congestion->window * CUBIC_BETA_NUMERATOR / CUBIC_BETA_DENOMINATOR;
    if (threshold < MinimumWindow(congestion))
        threshold = MinimumWindow(congestion);
    congestion->threshold = threshold;
    congestion->window = threshold;
    congestion->inRecovery = true;
    congestion->recoveryStart = now;
    congestion->cautious = false;
    return true;
}

void NetCongestionCollapse(NetCongestion *congestion, uint64_t now)
{
    congestion->window = MinimumWindow(congestion);
    congestion->inRecovery = true;
    congestion->recoveryStart = now;
    congestion->cautious = false;
    /* Прежний максимум о новом пути ничего не говорит: кривая начнётся от
     * окна в начале предотвращения перегрузки (RFC 9438, 4.8). */
    congestion->cubic.maxWindow = 0u;
    congestion->cubic.started = false;
}

void NetCongestionSetDatagram(NetCongestion *congestion, uint32_t datagramBytes)
{
    congestion->datagramBytes = datagramBytes;
    if (congestion->window < MinimumWindow(congestion))
        congestion->window = MinimumWindow(congestion);
}

void NetCongestionUndo(NetCongestion *congestion)
{
    const uint64_t prior = congestion->cubic.priorWindow;
    congestion->cubic = congestion->priorCubic;
    congestion->threshold = congestion->priorThreshold;
    if (congestion->window < prior)
        congestion->window = prior;
    congestion->inRecovery = false;
}
