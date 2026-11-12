#include "net/net_internal.h"

/* Размер датаграммы пути: DPLPMTUD (RFC 8899) поверх подтверждений.
 *
 * Рукопожатие доказывает базовый размер, 1200 байт, который проходит почти по
 * любому пути. Дальше соединение пробует большие: проба — пакет из PING и
 * заполнителя нужной длины. Подтверждённая проба поднимает размер, а три
 * потерянные подряд закрывают свой. Первая проба поиска — наибольший размер,
 * о котором договорились стороны: на обычном пути он и проходит. Иначе поиск
 * делит промежуток пополам, пока тот не станет уже 16 байт; размер ниже
 * предела через десять минут ищется снова, ведь путь мог измениться. Потеря
 * пробы о перегрузке не говорит, и окно она не трогает.
 *
 * Путь, вдруг переставший пропускать большие датаграммы, — чёрная дыра:
 * большие пакеты пропадают, пока малые доходят, либо подтверждений нет вовсе
 * и пробный таймер срабатывает дважды, пока в пути большие пакеты. Потери пакетов, за которыми
 * дошёл пакет полного размера, объясняет перегрузка: очередь маршрутизатора теряет пачками, а путь
 * размер пропускает. Остальные потери пакетов больше базового подозрительны, и восемь
 * подозрительных — чёрная дыра; подозрение снимает дошедший пакет не меньше подозрительных. Размер
 * тогда падает до базового, а поиск, снова с прежнего размера, начинается после паузы. Пауза
 * удваивается с каждой следующей чёрной дырой, пока их нет десять минут: путь, то пропускающий
 * большие датаграммы, то нет, не дёргает размер раз за разом. Пробы, ушедшие до чёрной дыры или до
 * смены пути, нового поиска не решают. */

/* MAX_PROBES из RFC 8899. */
#define PMTU_PROBES 3u
#define PMTU_RESOLUTION 16u
#define PMTU_BLACK_HOLE_LOSSES 8u
#define PMTU_BLACK_HOLE_TIMEOUTS 2u
/* PMTU_RAISE_TIMER из RFC 8899. */
#define PMTU_RAISE_INTERVAL (UINT64_C(600) * NET_SECOND)
#define PMTU_HOLD_OFF NET_SECOND

/* Поиск в (current, ceiling] с пробы самого ceiling, начиная с момента at. */
static void Search(NetPmtu *pmtu, uint32_t ceiling, uint64_t at)
{
    pmtu->ceiling = ceiling;
    pmtu->next = ceiling;
    pmtu->failures = 0u;
    pmtu->searchAt = ceiling > pmtu->current ? at : UINT64_MAX;
}

/* Промежуток сузился: следующая проба — его середина, а промежуток уже
 * PMTU_RESOLUTION — конец поиска до проверки через десять минут. */
static void Narrow(NetPmtu *pmtu, uint64_t now)
{
    pmtu->failures = 0u;
    if (pmtu->ceiling < pmtu->current + PMTU_RESOLUTION)
        Search(pmtu, pmtu->limit, now + PMTU_RAISE_INTERVAL);
    else
        pmtu->next = pmtu->current + (pmtu->ceiling - pmtu->current + 1u) / 2u;
}

/* Пакет number размера size дошёл: путь этот размер пропускает. */
static void Proven(NetPmtu *pmtu, uint64_t number, uint32_t size)
{
    if (size >= pmtu->current && number > pmtu->provenNumber)
        pmtu->provenNumber = number;
    /* Дошёл пакет не меньше подозрительных: их терял не размер. */
    if (size >= pmtu->suspectSize)
    {
        pmtu->suspects = 0u;
        pmtu->suspectSize = 0u;
    }
}

/* Чёрная дыра: размер падает до базового, поиск — после паузы. */
static bool Fall(NetPmtu *pmtu, uint64_t now)
{
    pmtu->suspects = 0u;
    pmtu->suspectSize = 0u;
    if (pmtu->current <= pmtu->base)
        return false;
    /* Сбрасывают паузу десять минут без чёрных дыр после неё самой. */
    if (pmtu->fallAt != UINT64_MAX && now - pmtu->fallAt < pmtu->holdOff + PMTU_RAISE_INTERVAL)
    {
        pmtu->holdOff *= 2u;
        if (pmtu->holdOff > PMTU_RAISE_INTERVAL)
            pmtu->holdOff = PMTU_RAISE_INTERVAL;
    }
    else
    {
        pmtu->holdOff = PMTU_HOLD_OFF;
    }
    pmtu->fallAt = now;
    pmtu->epoch = now;
    const uint32_t failed = pmtu->current;
    pmtu->current = pmtu->base;
    pmtu->probing = 0u;
    /* Чёрную дыру мог изобразить и обрыв связи: тогда первая же проба
     * вернёт прежний размер. */
    Search(pmtu, failed, now + pmtu->holdOff);
    return true;
}

void NetPmtuReset(NetPmtu *pmtu, uint32_t base, uint32_t limit, uint64_t now)
{
    pmtu->base = base < limit ? base : limit;
    pmtu->limit = limit;
    pmtu->current = pmtu->base;
    pmtu->probing = 0u;
    pmtu->provenNumber = 0u;
    pmtu->suspects = 0u;
    pmtu->suspectSize = 0u;
    pmtu->epoch = now;
    pmtu->fallAt = UINT64_MAX;
    Search(pmtu, limit, now);
}

uint32_t NetPmtuProbeSize(const NetPmtu *pmtu, uint64_t now)
{
    /* Проба в пути одна: исход каждой меняет следующую. */
    return pmtu->probing == 0u && now >= pmtu->searchAt ? pmtu->next : 0u;
}

void NetPmtuProbeSent(NetPmtu *pmtu, uint32_t size)
{
    pmtu->probing = size;
}

bool NetPmtuProbeAcked(NetPmtu *pmtu, uint64_t number, uint32_t size, uint64_t sentTime,
                       uint64_t now)
{
    if (sentTime < pmtu->epoch)
        return false;
    if (size == pmtu->probing)
        pmtu->probing = 0u;
    const bool raised = size > pmtu->current;
    if (raised)
    {
        /* Даже запоздалое подтверждение пробы, уже сочтённой потерянной,
         * доказывает, что размер проходит. Если он выше закрытого
         * промежутка, поиск на этом и кончается. */
        pmtu->current = size;
        Narrow(pmtu, now);
    }
    Proven(pmtu, number, size);
    return raised;
}

void NetPmtuProbeLost(NetPmtu *pmtu, uint32_t size, uint64_t sentTime, uint64_t now)
{
    if (sentTime < pmtu->epoch || size != pmtu->probing)
        return;
    pmtu->probing = 0u;
    if (size != pmtu->next || ++pmtu->failures < PMTU_PROBES)
        return;
    pmtu->ceiling = size - 1u;
    Narrow(pmtu, now);
}

void NetPmtuAcked(NetPmtu *pmtu, uint64_t number, uint32_t size)
{
    Proven(pmtu, number, size);
}

bool NetPmtuLost(NetPmtu *pmtu, uint64_t number, uint32_t size, uint64_t now)
{
    /* Считаются пакеты больше базового, но не больше нынешнего размера:
     * пакеты прежнего, большего, ушли до чёрной дыры и о ней уже сказали.
     * Потерю пакета, за которым дошёл пакет полного размера, объясняет
     * перегрузка. */
    if (size <= pmtu->base || size > pmtu->current || number < pmtu->provenNumber)
        return false;
    if (pmtu->suspectSize < size)
        pmtu->suspectSize = size;
    if (++pmtu->suspects < PMTU_BLACK_HOLE_LOSSES)
        return false;
    return Fall(pmtu, now);
}

bool NetPmtuTimeout(NetPmtu *pmtu, uint32_t timeouts, uint32_t largest, uint64_t now)
{
    /* Молчание, пока в пути одни малые пакеты, о размере не говорит. */
    return timeouts >= PMTU_BLACK_HOLE_TIMEOUTS && largest > pmtu->base && Fall(pmtu, now);
}
