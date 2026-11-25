#pragma once

/* Внутреннее устройство laiue.net: формат пакетов, состояние соединения и
 * связи между файлами модуля. Формат описан в docs/net.md. */

#include "net/net.h"
#include "net/net_crypto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Формат (все целые little-endian, переменные — LEB128) -----------------
 *
 * Пакеты рукопожатия открыты; первый байт несёт старший бит:
 *
 *   HELLO   0x81 | u16 версия | u8 флаги | u64 протокол | u64 CID клиента |
 *           32 эфемерный ключ клиента | 16 параметры | u8 длина метки |
 *           метка | нули до размера датаграммы
 *   RETRY   0x82 | u16 версия | u64 CID клиента | 17 метка
 *   ACCEPT  0x83 | u16 версия | u64 CID клиента | u64 CID сервера |
 *           32 эфемерный ключ сервера | 16 параметры, зашифрованы | 16 тег
 *   REJECT  0x84 | u16 версия | u64 CID клиента | u8 причина | u32 подробность
 *
 * Параметры: u16 наибольшая датаграмма | u8 каналы | u8 ноль |
 *            u32 наибольшее сообщение | u32 окно приёма | u32 простой, мс
 *
 * Пакет данных:
 *
 *   0x40 | u64 CID получателя | u32 младшие биты номера | кадры, зашифрованы |
 *   16 тег
 *
 * Заголовок входит в подтверждаемые данные AEAD; nonce — четыре нулевых
 * байта и 64-битный номер пакета. У каждого направления свой ключ. */

#define NET_PROTOCOL_VERSION 1u

#define NET_DATAGRAM_MINIMUM 576u
#define NET_DATAGRAM_MAXIMUM 1452u
/* Проходит почти по любому пути в интернете: с него соединение начинает, а
 * больший размер находит поиском. */
#define NET_DATAGRAM_BASE 1200u

#define NET_PACKET_LONG 0x80u
#define NET_PACKET_HELLO 0x81u
#define NET_PACKET_RETRY 0x82u
#define NET_PACKET_ACCEPT 0x83u
#define NET_PACKET_REJECT 0x84u
#define NET_PACKET_SHORT 0x40u

#define NET_SHORT_HEADER_SIZE 13u
#define NET_SHORT_OVERHEAD (NET_SHORT_HEADER_SIZE + NET_TAG_SIZE)

#define NET_HELLO_PINNED 0x01u
#define NET_HELLO_PRESHARED 0x02u
#define NET_HELLO_FIXED_SIZE 68u
#define NET_PARAMETERS_SIZE 16u
/* Флаг параметров: сторона просит часы получателя в кадрах ACK — ими мерит
 * путь её BBR. Незнакомые флаги не мешают. */
#define NET_PARAMETERS_ACK_CLOCK 0x01u
#define NET_COOKIE_SIZE 17u
#define NET_RETRY_SIZE 28u
#define NET_ACCEPT_HEADER_SIZE 51u
#define NET_ACCEPT_SIZE (NET_ACCEPT_HEADER_SIZE + NET_PARAMETERS_SIZE + NET_TAG_SIZE)
#define NET_REJECT_SIZE 16u

/* Кадры пакета данных:
 *
 *   PADDING    0x00
 *   PING       0x01
 *   ACK        0x02 | наибольший | задержка, мкс | число отрезков | первый |
 *              пары (пропуск, длина) — как в QUIC
 *   CLOSE      0x03 | u8 вид | код
 *   STREAM     0x04 | u8 канал | смещение | длина | байты
 *   DATAGRAM   0x05 | u8 канал | длина | байты
 *   SEQUENCED  0x06 | u8 канал | номер | длина | байты
 *   ACK_CLOCK  0x07 | наибольший | задержка, мкс | часы получателя |
 *              их погрешность | число отрезков | первый | пары — ACK для
 *              собеседника, попросившего часы получателя */
#define NET_FRAME_PADDING 0x00u
#define NET_FRAME_PING 0x01u
#define NET_FRAME_ACK 0x02u
#define NET_FRAME_CLOSE 0x03u
#define NET_FRAME_STREAM 0x04u
#define NET_FRAME_DATAGRAM 0x05u
#define NET_FRAME_SEQUENCED 0x06u
#define NET_FRAME_ACK_CLOCK 0x07u

/* Вид закрытия в кадре CLOSE; NONE — собеседника не уведомлять. */
#define NET_CLOSE_APPLICATION 0u
#define NET_CLOSE_PROTOCOL 1u
#define NET_CLOSE_RESOURCE 2u
#define NET_CLOSE_NONE 255u

/* ---- Время, микросекунды ------------------------------------------------------ */

#define NET_SECOND UINT64_C(1000000)
#define NET_MILLISECOND UINT64_C(1000)
#define NET_TIMER_GRANULARITY NET_MILLISECOND
#define NET_INITIAL_RTT (UINT64_C(333) * NET_MILLISECOND)
/* Дольше подтверждение не откладывается; столько же отправитель закладывает
 * в ожидание ответа. */
#define NET_MAX_ACK_DELAY (UINT64_C(25) * NET_MILLISECOND)
#define NET_HELLO_INTERVAL_FIRST (UINT64_C(250) * NET_MILLISECOND)
#define NET_HELLO_INTERVAL_LIMIT NET_SECOND
#define NET_COOKIE_LIFETIME (UINT64_C(10) * NET_SECOND)
#define NET_CLOSE_LINGER_LIMIT (UINT64_C(3) * NET_SECOND)
#define NET_CLOSE_REPEAT_INTERVAL (UINT64_C(20) * NET_MILLISECOND)
/* Столько раз закрывшая сторона повторяет уведомление сама, не дожидаясь
 * пакетов собеседника. */
#define NET_CLOSE_RESENDS 2u
#define NET_PROBE_INTERVAL_LIMIT (UINT64_C(60) * NET_SECOND)
/* Пакеты соединения, не прошедшие проверку подлинности: дальше ключу
 * ChaCha20-Poly1305 доверять нельзя (RFC 9001, 6.6). */
#define NET_FORGERY_LIMIT (UINT64_C(1) << 36u)

/* ---- Пределы -------------------------------------------------------------------- */

/* Окно номеров принятых пакетов: защита от повторов и источник кадра ACK. */
#define NET_WINDOW_BITS 1024u
#define NET_WINDOW_WORDS 16u
#define NET_ACK_RANGES 32u
/* Столько пакетов, требующих подтверждения, копится до немедленного ACK. */
#define NET_ACK_BURST 64u
/* Пороги обнаружения потерь: обгон на столько пакетов, требовавших
 * подтверждения, и возраст в столько восьмых долей времени оборота (9/8 по
 * RFC 9002). После мнимых потерь пороги растут до пределов. */
#define NET_PACKET_THRESHOLD 3u
#define NET_PACKET_THRESHOLD_LIMIT 64u
#define NET_TIME_THRESHOLD 9u
#define NET_TIME_THRESHOLD_LIMIT 16u

#define NET_RECORD_RANGES 3u
#define NET_RECORDS_INITIAL 32u
#define NET_RECORDS_LIMIT 4096u

#define NET_RECEIVED_RANGES 32u
#define NET_ACKED_RANGES 32u
#define NET_LOST_RANGES 16u

#define NET_MESSAGE_LIMIT (UINT32_C(1) << 30u)
#define NET_WINDOW_MINIMUM UINT32_C(4096)
#define NET_WINDOW_LIMIT (UINT32_C(1) << 28u)
#define NET_UNRELIABLE_QUEUE_BYTES (UINT32_C(128) * 1024u)
#define NET_ASSEMBLY_RETAIN (UINT32_C(64) * 1024u)
#define NET_FLUSH_PACKETS 512u
#define NET_RECEIVE_DATAGRAMS 16384u
#define NET_ACCEPTS_PER_UPDATE 32u
/* Запросы с верной меткой сверх бюджета обновления ждут следующих; степень
 * двойки. */
#define NET_DEFERRED_HELLOS 1024u
#define NET_RETRY_RESENDS 3u
#define NET_ARENA_CHUNK (UINT32_C(64) * 1024u)
/* Часы получателя в кадре ACK: младшие 30 бит микросекунд, не больше четырёх
 * байт. Сколько они прошли с прошлого кадра, отправитель восстанавливает по
 * ходу своих часов: ходы расходятся лишь на разницу задержек, а не на длину
 * тишины. Погрешность этих часов в том же кадре больше не бывает. */
#define NET_ACK_CLOCK_MASK ((UINT64_C(1) << 30u) - 1u)
#define NET_CONGESTION_WINDOW_LIMIT (UINT64_C(1) << 30u)
/* Предел темпа отправки в байтах в секунду: наибольшее окно за миллисекунду.
 * Произведение темпа на десять секунд помещается в 64 бита. */
#define NET_PACING_RATE_LIMIT (NET_CONGESTION_WINDOW_LIMIT * UINT64_C(1000))

/* ---- Состояние ------------------------------------------------------------------ */

typedef struct NetRange
{
    uint64_t start;
    uint64_t end;
} NetRange;

/* Отрезок потока, уехавший в пакете: по нему подтверждение и потеря
 * находят свои байты. */
typedef struct NetSentRange
{
    uint64_t offset;
    uint16_t length;
    uint8_t channel;
} NetSentRange;

/* Замер скорости доставки (net_rate.c): счётчики соединения. */
typedef struct NetRate
{
    /* Байты, подтверждённые за всё соединение, и когда получатель принял
     * последний из подтверждённых пакетов: по его часам, приведённым к своим.
     * Время, в которое подтверждение разобрано здесь, известно лишь с
     * точностью до шага обновления, и отрезки между подтверждениями по нему
     * шумели бы. */
    uint64_t delivered;
    uint64_t deliveredTime;
    /* Погрешность deliveredTime: пакет пришёл не раньше чем за столько до
     * него. Получатель тоже читает сеть раз в обновление. */
    uint64_t deliveredAge;
    /* Когда это подтверждение разобрано, по своим часам. */
    uint64_t ackedTime;
    /* Время отправки пакета, подтверждённого последним, а после тишины —
     * первого отправленного: от него считается скорость отправки. */
    uint64_t firstSentTime;
    /* Байты, объявленные потерянными. */
    uint64_t lost;
    /* Отметка в счёте delivered: замеры по пакетам, отправленным до её
     * подтверждения, ограничены приложением, а не путём. 0 — отметки нет. */
    uint64_t appLimited;
    /* Получатель отправил последнее подтверждение, не дочитав сеть: пакеты,
     * которых в нём нет, могли прийти и раньше deliveredTime, но не раньше
     * чем за deliveredAge до него. */
    bool deliveredOpen;
} NetRate;

/* Снимок счётчиков в момент отправки пакета. */
typedef struct NetRateStamp
{
    uint64_t delivered;
    uint64_t deliveredTime;
    uint64_t firstSentTime;
    uint64_t lost;
    /* Когда пакет ушёл бы при ровном темпе. Соединение шлёт пачками, раз в
     * обновление; по настоящему времени вся пачка ушла бы мгновенно, и
     * скорость отправки, а с ней и замер, вышли бы завышенными. */
    uint64_t pacedTime;
    /* Байты в пути сразу после отправки, вместе с самим пакетом. */
    uint32_t inFlight;
    /* Погрешность deliveredTime и недочитанная сеть, как в NetRate. */
    uint32_t deliveredAge;
    bool deliveredOpen;
    bool appLimited;
} NetRateStamp;

/* Замер одним подтверждением: его даёт самый новый из подтверждённых
 * пакетов. */
typedef struct NetRateSample
{
    /* Часы кадра ACK, приведённые к своим, и их погрешность: когда
     * получатель принял самый новый из названных пакетов и за сколько до
     * этого тот мог прийти; ackOpen — сеть он при этом не дочитал. Задаёт
     * тот, кто разбирает кадр. */
    uint64_t ackTime;
    uint64_t ackAge;
    uint64_t number;
    uint64_t sentTime;
    NetRateStamp stamp;
    /* Байты, подтверждённые этим ACK. */
    uint64_t acked;
    /* За interval микросекунд, пока пакет был в пути, доставлено delivered
     * байт и потеряно lost. */
    uint64_t delivered;
    uint64_t lost;
    uint64_t interval;
    /* Байты в секунду; 0 — замера скорости нет. rate — скорость доставки, как
     * её считает черновик; proven — скорость, которую путь заведомо выдал:
     * она не выше rate. */
    uint64_t rate;
    uint64_t proven;
    /* Наибольшее доставленное за полёт среди всех пакетов этого ACK. Один
     * ACK подтверждает пачку, и самый новый её пакет ушёл, когда в пути было
     * меньше всего: по нему одному объём пути вышел бы заниженным. */
    uint64_t flight;
    /* Самый большой из пакетов этого ACK: без него считается скорость,
     * которую путь заведомо выдал. */
    uint32_t largest;
    bool ackOpen;
    bool filled;
} NetRateSample;

#define NET_RECORD_SENT 1u
#define NET_RECORD_ACKED 2u
#define NET_RECORD_LOST 3u

typedef struct NetSentPacket
{
    uint64_t number;
    uint64_t sentTime;
    uint16_t size;
    /* Младшие биты эпизода восстановления, в котором пакет объявлен
     * потерянным: мнимая потеря по ним находит, чью реакцию откатывать. */
    uint16_t lossEpisode;
    uint8_t state;
    uint8_t rangeCount;
    /* Окно перегрузки при отправке было занято хотя бы наполовину: только
     * такие пакеты расширяют его подтверждением. */
    bool windowLimited;
    /* Проба размера датаграммы: её потеря о перегрузке не говорит. */
    bool sizeProbe;
    NetSentRange ranges[NET_RECORD_RANGES];
} NetSentPacket;

/* Поиск размера датаграммы пути (net_pmtu.c). Размеры — датаграммы целиком,
 * с заголовком и тегом. */
typedef struct NetPmtu
{
    /* Когда слать следующую пробу; UINT64_MAX — искать больше нечего. */
    uint64_t searchAt;
    /* Начало нынешнего поиска: пробы, ушедшие раньше, ничего не решают. */
    uint64_t epoch;
    /* Последняя чёрная дыра (UINT64_MAX — не было) и пауза перед поиском
     * после неё. */
    uint64_t fallAt;
    uint64_t holdOff;
    /* Номер последнего дошедшего пакета полного размера: потери пакетов,
     * ушедших раньше, объясняет перегрузка. */
    uint64_t provenNumber;
    /* Размер, доказанный рукопожатием, и наибольший, о котором договорились
     * стороны. */
    uint32_t base;
    uint32_t limit;
    /* Подтверждённый размер: им идут все пакеты. */
    uint32_t current;
    /* Поиск идёт в (current, ceiling]. */
    uint32_t ceiling;
    /* Размер следующей пробы и пробы в пути; 0 — её нет. */
    uint32_t next;
    uint32_t probing;
    /* Потерянные подряд пробы размера next. */
    uint32_t failures;
    /* Подозрительные потери пакетов больше базового размера и наибольший из
     * них; 0 — подозрений нет. */
    uint32_t suspects;
    uint32_t suspectSize;
} NetPmtu;

typedef struct NetChannel
{
    /* Отправка надёжного потока: кольцо байт [sendAcked, sendEnd). */
    uint8_t *sendData;
    uint32_t sendCapacity;
    uint64_t sendAcked;
    uint64_t sendNext;
    uint64_t sendEnd;
    NetRange ackedRanges[NET_ACKED_RANGES];
    uint32_t ackedCount;
    NetRange lostRanges[NET_LOST_RANGES];
    uint32_t lostCount;
    uint64_t sequenceNext;

    /* Приём: всё ниже receiveOffset уже разобрано на сообщения. */
    uint64_t receiveOffset;
    uint8_t *receiveData;
    NetRange receivedRanges[NET_RECEIVED_RANGES];
    uint32_t receivedCount;
    uint64_t sequenceSeen;

    /* Сборка сообщения, пришедшего частями. */
    uint8_t *assembly;
    uint32_t assemblyCapacity;
    uint32_t messageSize;
    uint32_t messageFilled;
    uint64_t headerValue;
    uint32_t headerShift;
    bool haveHeader;
} NetChannel;

/* Кривая CUBIC одного эпизода предотвращения перегрузки (RFC 9438). */
typedef struct NetCubic
{
    /* W_max: окно перед последним сокращением; быстрая сходимость делает
     * его меньше. */
    uint64_t maxWindow;
    /* cwnd_prior: окно перед последним сокращением. */
    uint64_t priorWindow;
    /* W_est: окно, каким его вёл бы Reno. */
    uint64_t renoWindow;
    /* t, микросекунды: время предотвращения перегрузки, пока окно тесно. */
    uint64_t elapsed;
    /* K, миллисекунды: через столько от начала кривая возвращается к
     * maxWindow. */
    uint64_t plateau;
    /* Кривую запускает первое подтверждение после восстановления. */
    bool started;
} NetCubic;

/* Окно перегрузки соединения: CUBIC с медленным стартом HyStart++
 * (RFC 9406). */
typedef struct NetCongestion
{
    uint64_t window;
    /* Порог медленного старта; UINT64_MAX — идёт первый медленный старт. */
    uint64_t threshold;
    /* Пакеты, отправленные до начала восстановления, окно не меняют. */
    uint64_t recoveryStart;
    uint64_t lastAckTime;
    /* Дробные остатки приращений: окно растёт на доли байта за
     * подтверждение. */
    uint64_t windowRemainder;
    uint64_t renoRemainder;
    NetCubic cubic;
    /* Кривая и порог до сокращения: мнимая потеря возвращает их. */
    NetCubic priorCubic;
    uint64_t priorThreshold;

    /* HyStart++. Раунд — один полёт: он кончается, когда подтверждён пакет с
     * номером не меньше roundEnd. */
    uint64_t roundEnd;
    uint64_t roundMinRtt;
    uint64_t lastRoundMinRtt;
    /* Наименьший оборот раунда, в котором рост стал осторожным. */
    uint64_t baselineRtt;
    uint32_t roundSamples;
    uint32_t cautiousRounds;

    uint32_t datagramBytes;
    bool inRecovery;
    /* Окно было тесно и при прошлом подтверждении: время между ними идёт в
     * кривую. */
    bool lastAckLimited;
    /* Осторожный рост медленного старта (CSS). */
    bool cautious;
} NetCongestion;

/* Подтверждение глазами окна перегрузки. */
typedef struct NetCongestionAck
{
    uint64_t now;
    /* Старший подтверждённый номер и номер следующего пакета: по ним
     * считаются раунды. */
    uint64_t largest;
    uint64_t nextNumber;
    /* Замер времени оборота этим подтверждением; 0 — замера нет. */
    uint64_t rttSample;
    uint64_t smoothedRtt;
    /* Подтверждённые байты пакетов, которым окно было тесно и которые
     * отправлены после начала восстановления, и время отправки последнего из
     * них. */
    uint64_t limitedBytes;
    uint64_t limitedSentTime;
} NetCongestionAck;

/* Состояния BBR. */
#define NET_BBR_STARTUP 0u
#define NET_BBR_DRAIN 1u
#define NET_BBR_PROBE_DOWN 2u
#define NET_BBR_PROBE_CRUISE 3u
#define NET_BBR_PROBE_REFILL 4u
#define NET_BBR_PROBE_UP 5u
#define NET_BBR_PROBE_RTT 6u

/* Окно оценки лишних подтверждений: нынешний оборот и столько прошлых. */
#define NET_BBR_EXTRA_ROUNDS 10u

/* Модель пути и управление BBRv3 (net_bbr.c). Объёмы — байты, время —
 * микросекунды, скорости — байты в секунду; UINT64_MAX у предела значит, что
 * предела нет. */
typedef struct NetBbr
{
    /* Итог: окно и темп отправки. Темп 0 — отправку сдерживает одно окно. */
    uint64_t window;
    uint64_t pacingRate;

    /* Скорость. maxBw — наибольший замер нынешнего и прошлого циклов проб
     * полосы, bwShort — предел после потерь нынешнего цикла, bw — меньшее. */
    uint64_t maxBwSlots[2];
    uint64_t maxBw;
    uint64_t bwShort;
    uint64_t bw;
    /* Наибольшие скорость и объём доставки за оборот потерь. */
    uint64_t bwLatest;
    uint64_t inflightLatest;

    /* Объём. minRtt — наименьший оборот за десять секунд; inflightLong —
     * объём в пути, при котором проба полосы встретила потери выше порога;
     * inflightShort — предел после потерь нынешнего цикла. */
    uint64_t minRtt;
    uint64_t minRttStamp;
    uint64_t inflightLong;
    uint64_t inflightShort;

    /* Подтверждения пачками: сколько доставлено сверх ожидаемого по bw с
     * начала отрезка и наибольший избыток по оборотам. */
    uint64_t extraStart;
    uint64_t extraDelivered;
    uint64_t extraAcked;
    uint64_t extraRing[NET_BBR_EXTRA_ROUNDS + 1u];

    /* Обороты считаются пакетами: оборот кончается подтверждением пакета,
     * отправленного после его начала. delivered повторяет счётчик замера
     * скорости. */
    uint64_t delivered;
    uint64_t nextRoundDelivered;
    uint64_t roundCount;
    uint64_t lossRoundDelivered;
    uint64_t drainStartRound;

    /* Полная полоса: скорость, от которой ждут роста на четверть. */
    uint64_t fullBw;

    /* Цикл проб полосы: его начало, пауза до следующей пробы и рост
     * inflightLong — на датаграмму за каждые upAckedPerStep подтверждённых
     * байт. */
    uint64_t cycleStamp;
    uint64_t probeWait;
    uint64_t upAcked;
    uint64_t upAckedPerStep;

    /* Замер оборота: когда его можно кончать (0 — объём ещё не упал) и окно
     * на его время, запомненное по прежнему minRtt. */
    uint64_t probeRttDone;
    uint64_t probeRttWindow;

    /* Восстановление после потерь и то, что вернёт отмена мнимых. */
    uint64_t priorWindow;
    uint64_t recoveryStart;
    uint64_t undoBwShort;
    uint64_t undoInflightShort;
    uint64_t undoInflightLong;

    uint64_t initialWindow;
    /* С этого времени путь нынешний: пакеты, отправленные раньше, о нём
     * ничего не говорят. */
    uint64_t since;
    uint64_t random;

    uint32_t datagramBytes;
    uint32_t pacingGain;
    uint32_t windowGain;
    uint32_t fullBwCount;
    /* Серии потерь и замеры скорости за оборот потерь. */
    uint32_t lossEvents;
    uint32_t lossRoundSamples;
    uint32_t upRounds;
    uint32_t roundsSinceProbe;
    /* Кольцо extraRing: нынешний оборот и сколько прошлых в нём годны. */
    uint32_t extraIndex;
    uint32_t extraSpan;

    uint8_t state;
    uint8_t undoState;
    uint8_t cycle;
    bool roundStart;
    bool lossRoundStart;
    bool lossInRound;
    bool fullBwNow;
    bool fullBwReached;
    /* Потери ещё говорят о последней пробе полосы. */
    bool checkProbeLoss;
    /* Проба кончилась, но подтверждения её пакетов ещё идут. */
    bool probeStopping;
    /* Прошлая проба встретила потери выше порога; нынешняя из-за этого
     * прервана у прежнего предела. */
    bool probeTooHigh;
    bool probePrecautionary;
    bool minRttExpired;
    bool probeRttRoundDone;
    bool idleRestart;
    bool inRecovery;
    /* Окно было занято целиком в нынешнем и в прошлом обороте. */
    bool windowFilled;
    bool windowFilledPrior;
    /* Начальный темп уже посчитан по измеренному обороту. */
    bool paced;
} NetBbr;

/* Подтверждение глазами BBR. */
typedef struct NetBbrAck
{
    uint64_t now;
    /* Счётчик доставленного и байты в пути после этого подтверждения. */
    uint64_t delivered;
    uint64_t inFlight;
    /* Замер скорости самым новым из подтверждённых пакетов: счётчик
     * доставленного при его отправке, доставлено и потеряно за его полёт,
     * скорость доставки и скорость, которую путь заведомо выдал (0 — замера
     * нет), объём в пути сразу после его отправки. */
    uint64_t priorDelivered;
    uint64_t sampleDelivered;
    uint64_t lost;
    uint64_t rate;
    uint64_t proven;
    uint64_t txInFlight;
    /* Байты, подтверждённые этим ACK. */
    uint64_t acked;
    /* Время отправки того же пакета: по нему кончается восстановление. */
    uint64_t sentTime;
    /* Замер оборота этим подтверждением и сглаженный оборот; 0 — нет. */
    uint64_t rttSample;
    uint64_t smoothedRtt;
    /* Замер ограничен приложением, а не путём. */
    bool appLimited;
} NetBbrAck;

/* Потеря пакета глазами BBR. */
typedef struct NetBbrLoss
{
    uint64_t now;
    uint64_t sentTime;
    /* Объём в пути сразу после отправки пакета и потеряно с тех пор, вместе
     * с ним самим. */
    uint64_t txInFlight;
    uint64_t lost;
    uint32_t size;
    bool appLimited;
    /* Пакет, отправленный прямо перед этим, тоже потерян: серия одна. */
    bool contiguous;
} NetBbrLoss;

#define NET_STATE_CONNECTING 1u
#define NET_STATE_HALF_OPEN 2u
#define NET_STATE_CONNECTED 3u
#define NET_STATE_FLUSHING 4u
#define NET_STATE_CLOSING 5u

typedef struct NetParameters
{
    uint32_t datagramBytes;
    uint32_t channelCount;
    uint32_t maxMessageBytes;
    uint32_t windowBytes;
    uint32_t idleMilliseconds;
    /* Сторона просит часы получателя в кадрах ACK. */
    bool ackClock;
} NetParameters;

/* Всё, что нужно одному BBR: модель пути, замер скорости доставки со
 * снимками для записей отправленных пакетов, часы собеседника из кадров ACK
 * и ровный темп внутри обновления. Блок выделяется только соединению BBR:
 * память соединения CUBIC и его записи не больше, чем были бы без BBR, а
 * от их размера зависит, сколько соединений сервер обходит за такт. */
typedef struct NetModel
{
    NetBbr bbr;
    NetRate rate;
    /* Снимок замера при отправке пакета — на том же месте, что его запись в
     * очереди отправленных, и с той же ёмкостью. */
    NetRateStamp *stamps;
    /* Последний кадр ACK, продвинувший часы собеседника: их младшие биты,
     * время по ним, приведённое к часам этой стороны, и когда кадр разобран
     * по своим. */
    uint64_t ackClock;
    uint64_t ackClockTime;
    uint64_t ackClockSeen;
    /* Доля байта, заработанная темпом и ещё не выданная, в миллионных:
     * частые обновления на медленном пути не теряют её округлением. */
    uint64_t paceRemainder;
    /* Ровный темп внутри обновления: за paceElapsed микросекунд заработано
     * paceEarned байт, из них отправлено paceSpent. paceClock — время, когда
     * при ровном темпе ушёл бы последний пакет. */
    uint64_t paceEarned;
    uint64_t paceSpent;
    uint64_t paceElapsed;
    uint64_t paceClock;
    bool ackClockKnown;
} NetModel;

/* Поля сгруппированы по назначению, а внутри группы идут от широких к узким:
 * так структура почти не тратит места на выравнивание. */
typedef struct NetPeer
{
    void *userData;
    uint64_t localCid;
    uint64_t remoteCid;
    LaiueNetAddress address;
    uint32_t slot;
    uint32_t activeIndex;
    uint32_t state;
    /* Соседи в очереди ожидающих первого пакета: слот плюс один, ноль — нет. */
    uint32_t halfOpenPrevious;
    uint32_t halfOpenNext;
    /* Приложение знает описатель и ждёт событие DISCONNECTED. */
    bool announced;
    bool initiator;

    /* Рукопожатие. */
    uint64_t handshakeDeadline;
    uint64_t handshakeResendAt;
    uint64_t handshakeInterval;
    uint64_t helloSentAt;
    uint32_t retryResends;
    uint8_t ephemeralPrivate[NET_KEY_SIZE];
    uint8_t ephemeralPublic[NET_KEY_SIZE];
    /* Клиент: закреплённый ключ сервера. Сервер: эфемерный ключ клиента. */
    uint8_t remoteKey[NET_KEY_SIZE];
    uint8_t sendKey[NET_KEY_SIZE];
    uint8_t receiveKey[NET_KEY_SIZE];
    uint8_t cookie[NET_COOKIE_SIZE];
    uint8_t accept[NET_ACCEPT_SIZE];
    uint8_t cookieSize;
    bool handshakeStarted;
    bool pinned;
    bool acceptFailed;
    /* Ответ сервера нельзя приписать одному запросу: время оборота по нему
     * не меряется. */
    bool helloResent;
    /* Собеседник прислал подлинный пакет данных. Сервер узнаёт о соединении
     * только по первому пакету клиента, поэтому клиент до этого не знает,
     * установлено ли оно с той стороны. */
    bool confirmed;

    /* Согласованные параметры. Размер датаграммы — подтверждённый поиском
     * размер пути. */
    uint64_t idleTimeout;
    NetChannel *channels;
    NetParameters remote;
    NetPmtu pmtu;
    uint32_t datagramBytes;
    uint32_t channelCount;

    /* Номера принятых пакетов. */
    uint64_t largestSeen;
    /* Старший из пакетов, требовавших подтверждения, и время его приёма:
     * кадр ACK называет его и то, сколько ответ ждал отправки. */
    uint64_t largestEliciting;
    uint64_t largestElicitingTime;
    /* Когда принят последний по времени пакет, требовавший подтверждения, и
     * когда сеть до этого была прочитана до конца: между этими моментами он
     * и пришёл. Ведутся, только если собеседник просил часы получателя:
     * кадр ACK_CLOCK несёт оба, и по ним его BBR меряет скорость доставки. */
    uint64_t arrivalTime;
    uint64_t arrivalFloor;
    uint64_t seenBits[NET_WINDOW_WORDS];
    uint64_t ackBits[NET_WINDOW_WORDS];
    uint64_t ackPendingSince;
    uint64_t lastReceiveTime;
    /* Пакеты с CID соединения, не прошедшие проверку подлинности. */
    uint64_t forgeries;
    uint32_t ackPendingCount;
    bool anySeen;
    bool anyEliciting;
    bool ackImmediate;

    /* Отправленные пакеты, требовавшие подтверждения: очередь в порядке
     * отправки. */
    uint64_t nextNumber;
    NetSentPacket *records;
    uint64_t largestAcked;
    uint32_t recordCapacity;
    uint32_t recordHead;
    uint32_t recordCount;
    bool anyAcked;
    bool pingPending;

    /* Оценка пути и обнаружение потерь. */
    uint64_t smoothedRtt;
    uint64_t rttVariance;
    uint64_t minRtt;
    uint64_t latestRtt;
    uint64_t firstRttTime;
    uint64_t lossTime;
    uint64_t lastAckElicitingSent;
    uint32_t ackElicitingOutstanding;
    uint32_t probeCount;
    uint32_t probesPending;
    uint32_t packetThreshold;
    /* Восьмые доли времени оборота. */
    uint32_t timeThreshold;
    bool haveRtt;
    /* С начала прошлого эпизода восстановления была мнимая потеря. */
    bool spuriousSeen;

    /* Управление перегрузкой: окно CUBIC либо, если конечная точка выбрала
     * BBR, отдельный блок его модели пути. */
    NetCongestion congestion;
    NetModel *model;
    uint64_t bytesInFlight;
    int64_t paceBudget;
    uint64_t paceTime;
    /* Эпизод восстановления: сколько потерь в нём объявлено и сколько из них
     * оказались мнимыми. Когда мнимы все, окно возвращается к прежнему. */
    uint32_t recoveryEpisode;
    uint32_t recoveryLosses;
    uint32_t recoverySpurious;
    bool recoveryUndoable;

    /* Закрытие. */
    uint64_t closeDeadline;
    uint64_t closeRepeatAt;
    uint64_t closeResendAt;
    uint64_t closeInterval;
    uint32_t closeCode;
    uint32_t closeResends;
    uint8_t closeKind;

    /* Очередь ненадёжных сообщений: записи подряд в одном буфере. Каналы
     * обслуживаются по кругу с roundRobin. */
    uint8_t *unreliable;
    uint32_t unreliableHead;
    uint32_t unreliableTail;
    uint32_t unreliableCapacity;
    uint32_t roundRobin;

    uint64_t queuedReliable;
    uint64_t unreliableDropped;
    uint64_t packetsSent;
    uint64_t packetsReceived;
    uint64_t packetsLost;
    uint64_t bytesSent;
    uint64_t bytesReceived;
} NetPeer;

typedef struct NetSlot
{
    NetPeer *peer;
    uint32_t generation;
    /* Следующий свободный слот плюс один; ноль — конец списка. */
    uint32_t nextFree;
} NetSlot;

/* Проверенный запрос, которому не хватило бюджета обмена ключами. */
typedef struct NetDeferredHello
{
    uint64_t serverCid;
    uint64_t receivedAt;
    LaiueNetAddress from;
    NetParameters remote;
    uint8_t hello[NET_HELLO_FIXED_SIZE];
} NetDeferredHello;

typedef struct NetCidEntry
{
    uint64_t cid;
    uint32_t slot;
    uint32_t used;
} NetCidEntry;

typedef struct NetArenaChunk
{
    struct NetArenaChunk *next;
    uint32_t capacity;
    uint32_t used;
} NetArenaChunk;

/* Группы по назначению, внутри — от широких полей к узким. */
struct LaiueNetEndpoint
{
    LaiueNetDriverV1 driver;
    uint64_t protocolId;
    uint64_t unreliableLifetime;
    uint64_t handshakeTimeout;
    LaiueNetRandomFn random;
    void *randomContext;
    uint64_t now;
    /* Когда сеть в последний раз была прочитана до конца: всё, что прочитано
     * позже, пришло после этого. */
    uint64_t drainedTime;
    NetParameters local;
    uint32_t flags;
    uint32_t maxPeers;
    uint32_t sendBufferBytes;
    uint32_t eventBacklogBytes;
    uint8_t identityPrivate[NET_KEY_SIZE];
    uint8_t preshared[NET_KEY_SIZE];
    bool hasIdentity;
    bool hasPreshared;
    bool driverBroken;
    /* Соединения этой конечной точки ведёт BBR. */
    bool modelBased;

    NetSlot *slots;
    uint32_t *active;
    NetCidEntry *cids;
    uint32_t slotCapacity;
    uint32_t slotCount;
    uint32_t freeSlot;
    uint32_t activeCount;
    /* Закрытые, но ещё повторяющие уведомление: в предел maxPeers не входят. */
    uint32_t lingerCount;
    /* Очередь принятых, но не подтвердивших ключи: старейший в голове. */
    uint32_t halfOpenHead;
    uint32_t halfOpenTail;
    uint32_t cidCapacity;
    uint32_t cidCount;

    /* Метки обратного пути: два ключа, чередуются. Третий ключ постоянен: по
     * нему из запроса клиента выводится CID сервера, и повторный запрос
     * находит уже созданного участника. */
    uint64_t cookieRotateAt;
    /* Кольцо отложенных запросов; память — при первой нужде. */
    NetDeferredHello *deferred;
    uint8_t cookieKeys[2][NET_KEY_SIZE];
    uint8_t cidKey[NET_KEY_SIZE];
    uint32_t acceptBudget;
    uint32_t deferredHead;
    uint32_t deferredCount;
    bool cookieReady;
    uint8_t cookieEpoch;

    /* События: [eventHead, eventCount) ждут приложения. eventReserved мест
     * обещано событиям DISCONNECTED живых участников. */
    LaiueNetEvent *events;
    NetArenaChunk *arenaFirst;
    NetArenaChunk *arenaCurrent;
    uint64_t arenaBytes;
    uint32_t eventHead;
    uint32_t eventCount;
    uint32_t eventCapacity;
    uint32_t eventReserved;

    uint8_t *receiveBuffer;
    uint8_t *packetBuffer;

    LaiueNetEndpointStats stats;
};

/* ---- Байты ---------------------------------------------------------------------- */

static inline uint16_t NetLoad16(const uint8_t *bytes)
{
    return (uint16_t)((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u));
}

static inline uint32_t NetLoad32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

static inline uint64_t NetLoad64(const uint8_t *bytes)
{
    return (uint64_t)NetLoad32(bytes) | ((uint64_t)NetLoad32(bytes + 4) << 32u);
}

static inline void NetStore16(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static inline void NetStore32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static inline void NetStore64(uint8_t *bytes, uint64_t value)
{
    NetStore32(bytes, (uint32_t)value);
    NetStore32(bytes + 4, (uint32_t)(value >> 32u));
}

/* net_wire.c */
uint32_t NetVarintSize(uint64_t value);
/* Пишет не больше десяти байт и возвращает их число. */
uint32_t NetVarintWrite(uint8_t *output, uint64_t value);
bool NetVarintRead(const uint8_t **cursor, const uint8_t *end, uint64_t *outValue);
void NetParametersWrite(const NetParameters *parameters, uint8_t output[NET_PARAMETERS_SIZE]);
/* false — параметры собеседника вне допустимых границ. */
bool NetParametersRead(const uint8_t input[NET_PARAMETERS_SIZE], NetParameters *outParameters);

/* net_endpoint.c */
bool NetRandom(LaiueNetEndpoint *endpoint, void *output, uint32_t size);
void NetTransmit(LaiueNetEndpoint *endpoint, const LaiueNetAddress *to, const uint8_t *bytes,
                 uint32_t size);
/* Новый участник. cid — его идентификатор для входящих пакетов либо NULL,
 * тогда он случаен. NULL — нет памяти или случайных байт (outStatus говорит,
 * чего именно). */
NetPeer *NetPeerCreate(LaiueNetEndpoint *endpoint, const LaiueNetAddress *address,
                       const uint64_t *cid, LaiueNetStatus *outStatus);
void NetPeerSetState(LaiueNetEndpoint *endpoint, NetPeer *peer, uint32_t state);
/* Освобождает место старейшим из не подтвердивших ключи. false — таких нет. */
bool NetPeerEvictHalfOpen(LaiueNetEndpoint *endpoint);
/* Участники, входящие в предел maxPeers. */
uint32_t NetPeerCount(const LaiueNetEndpoint *endpoint);
void NetPeerRelease(LaiueNetEndpoint *endpoint, NetPeer *peer);
NetPeer *NetPeerFindByCid(const LaiueNetEndpoint *endpoint, uint64_t cid);
/* Обещает место событию DISCONNECTED и ещё extra обычным событиям. */
bool NetEventReserveFinal(LaiueNetEndpoint *endpoint, uint32_t extra);
/* Копирует данные в память событий. false — нет памяти. */
bool NetEventPush(LaiueNetEndpoint *endpoint, const NetPeer *peer, LaiueNetEventType type,
                  uint32_t channel, LaiueNetDelivery delivery, const uint8_t *data, uint32_t size);
/* Сообщает приложению о входящем соединении. false — нет памяти. */
bool NetPeerAnnounce(LaiueNetEndpoint *endpoint, NetPeer *peer);
/* Закрывает соединение с любой стороны: событие приложению, если оно знает
 * участника; кадр CLOSE собеседнику, если closeKind не NET_CLOSE_NONE и ключи
 * есть; освобождение сразу либо после выдержки. */
void NetPeerClose(LaiueNetEndpoint *endpoint, NetPeer *peer, uint32_t reason, uint32_t code,
                  uint32_t closeKind);

/* net_handshake.c */
bool NetHandshakeBegin(LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t *serverPublicKey);
void NetHandshakeReceive(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from,
                         const uint8_t *bytes, uint32_t size);
void NetHandshakeUpdate(LaiueNetEndpoint *endpoint, NetPeer *peer);
/* Принимает отложенные запросы, пока хватает бюджета обновления. */
void NetHandshakeAdmitDeferred(LaiueNetEndpoint *endpoint);

/* net_packet.c */
/* Готовит участника к обмену данными: каналы, записи пакетов, оценки пути. */
bool NetPeerEstablish(LaiueNetEndpoint *endpoint, NetPeer *peer);
void NetPeerFreeTransport(NetPeer *peer);
void NetShortReceive(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from, uint8_t *bytes,
                     uint32_t size);
/* Разбирает расшифрованные кадры пакета. Итог — NET_FRAMES_*. */
uint32_t NetPeerProcessFrames(LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t *payload,
                              uint32_t size, bool *outAckEliciting);
void NetPeerUpdate(LaiueNetEndpoint *endpoint, NetPeer *peer);
void NetPeerSendClose(LaiueNetEndpoint *endpoint, NetPeer *peer);
uint32_t NetPeerMaxUnreliable(const NetPeer *peer);
uint64_t NetPeerProbeTimeout(const NetPeer *peer);
/* Первая оценка времени оборота: по рукопожатию, до первого пакета данных. */
void NetPeerSeedRtt(NetPeer *peer, uint64_t sample, uint64_t now);

/* net_congestion.c */
/* Окно в начале соединения либо на новом пути: первый медленный старт. */
void NetCongestionReset(NetCongestion *congestion, uint32_t datagramBytes, uint64_t window,
                        uint64_t nextNumber);
void NetCongestionOnAck(NetCongestion *congestion, const NetCongestionAck *ack);
/* Потеря пакета, отправленного в sentTime. true — началось восстановление и
 * окно сокращено; потери пакетов, отправленных раньше, окно не режут. */
bool NetCongestionOnLoss(NetCongestion *congestion, uint64_t sentTime, uint64_t now);
/* Затяжная перегрузка: окно сжимается до двух датаграмм. */
void NetCongestionCollapse(NetCongestion *congestion, uint64_t now);
/* Все потери последнего сокращения мнимы: окно, порог и кривая
 * возвращаются к прежним. */
void NetCongestionUndo(NetCongestion *congestion);
/* Размер датаграммы пути изменился; окно в байтах остаётся прежним. */
void NetCongestionSetDatagram(NetCongestion *congestion, uint32_t datagramBytes);
/* Целый кубический корень с округлением вниз. */
uint64_t NetCubeRoot(uint64_t value);

static inline bool NetCongestionInRecovery(const NetCongestion *congestion, uint64_t sentTime)
{
    return congestion->inRecovery && sentTime <= congestion->recoveryStart;
}

/* value * factor / divisor. Произведение, которому не хватает 64 бит, делится
 * раньше, чем умножается: теряет точность, а не разряды. */
static inline uint64_t NetScale(uint64_t value, uint64_t factor, uint64_t divisor)
{
    if (factor == 0u)
        return 0u;
    if (value <= UINT64_MAX / factor)
        return value * factor / divisor;
    const uint64_t quotient = value / divisor;
    return quotient <= UINT64_MAX / factor ? quotient * factor : UINT64_MAX;
}

/* net_rate.c */
/* Пакет в size байт отправлен; inFlight — байты в пути вместе с ним,
 * pacedTime — когда он ушёл бы при ровном темпе, не позже now. */
void NetRateSent(NetRate *rate, NetRateStamp *stamp, uint64_t inFlight, uint32_t size,
                 uint64_t pacedTime, uint64_t now);
/* Пакет подтверждён: доставленное растёт, а замер берёт снимок самого нового
 * из пакетов этого ACK. Часы кадра и их погрешность уже стоят в замере. */
void NetRateAcked(NetRate *rate, NetRateSample *sample, const NetRateStamp *stamp, uint64_t number,
                  uint64_t sentTime, uint32_t size, uint64_t now);
/* Все пакеты ACK учтены, потери объявлены: считает замер. minRtt — наименьший
 * оборот соединения (0 — не измерен); за отрезок короче него скорость не
 * меряется. */
void NetRateFinish(NetRate *rate, NetRateSample *sample, uint64_t minRtt);
/* Слать нечего, хотя окно и темп позволяют: замеры ближайшего оборота
 * говорят о приложении. inFlight — байты в пути. */
void NetRateLimited(NetRate *rate, uint64_t inFlight);

/* net_bbr.c */
/* Начало соединения либо новый путь. seed — зерно случайных пауз между пробами
 * полосы. */
void NetBbrReset(NetBbr *bbr, uint32_t datagramBytes, uint64_t window, uint64_t seed, uint64_t now);
/* Время оборота измерено: до первого замера скорости темп считается по нему. */
void NetBbrSeed(NetBbr *bbr, uint64_t smoothedRtt);
/* Перед отправкой пакета в size байт. inFlight — байты в пути до него,
 * limited — замеры сейчас ограничены приложением. */
void NetBbrOnSend(NetBbr *bbr, uint64_t inFlight, uint32_t size, bool limited, uint64_t now);
void NetBbrOnAck(NetBbr *bbr, const NetBbrAck *ack);
/* true — началось восстановление: потери пакетов, отправленных раньше, нового
 * эпизода не начинают. */
bool NetBbrOnLoss(NetBbr *bbr, const NetBbrLoss *loss);
/* Затяжная перегрузка: окно сжимается до двух датаграмм и растёт заново;
 * пакеты, ушедшие раньше, модель не учат. */
void NetBbrCollapse(NetBbr *bbr, uint64_t now);
/* Все потери последнего эпизода мнимы: окно, пределы модели и состояние
 * возвращаются к прежним. */
void NetBbrUndo(NetBbr *bbr);
/* Размер датаграммы пути изменился. */
void NetBbrSetDatagram(NetBbr *bbr, uint32_t datagramBytes);
/* Столько может уйти разом, не дожидаясь темпа: миллисекунда отправки, но не
 * меньше двух датаграмм и не больше 64 КиБ. */
uint64_t NetBbrQuantum(const NetBbr *bbr);

/* Окно при замере оборота нарочно мало, и скорость доставки говорит о нём,
 * а не о пути. */
static inline bool NetBbrProbesRtt(const NetBbr *bbr)
{
    return bbr->state == NET_BBR_PROBE_RTT;
}

/* Данным нет места в окне: оно занято целиком. */
static inline void NetBbrWindowFilled(NetBbr *bbr)
{
    bbr->windowFilled = true;
}

/* Окно перегрузки соединения, какой бы алгоритм его ни вёл. */
static inline uint64_t NetPeerWindow(const NetPeer *peer)
{
    return peer->model != NULL ? peer->model->bbr.window : peer->congestion.window;
}

/* Итог разбора кадров и приёма данных канала. */
#define NET_FRAMES_OK 0u
/* Данные не сохранены: пакет не подтверждать, отправитель повторит. */
#define NET_FRAMES_DEFER 1u
#define NET_FRAMES_VIOLATION 2u
#define NET_FRAMES_RESOURCE 3u
/* Собеседник закрыл соединение; участник уже освобождён. */
#define NET_FRAMES_CLOSED 4u

/* net_channel.c */
void NetChannelsFree(NetPeer *peer);
LaiueNetStatus NetChannelQueueReliable(const LaiueNetEndpoint *endpoint, NetPeer *peer,
                                       uint32_t channelIndex, const void *bytes, uint32_t size);
LaiueNetStatus NetPeerQueueUnreliable(const LaiueNetEndpoint *endpoint, NetPeer *peer,
                                      uint32_t channelIndex, bool sequenced, const void *bytes,
                                      uint32_t size);
uint32_t NetPeerQueuedUnreliable(const NetPeer *peer);
/* Выбрасывает сообщения, чей срок ожидания истёк к моменту now. */
void NetPeerExpireUnreliable(NetPeer *peer, uint64_t now);
/* Пишут очередной кадр в output и возвращают его размер; 0 — нечего
 * отправлять либо не поместилось. */
uint32_t NetPeerWriteUnreliable(NetPeer *peer, uint8_t *output, uint32_t space);
uint32_t NetChannelWriteStream(NetPeer *peer, uint32_t channelIndex, uint8_t *output,
                               uint32_t space, NetSentRange *outRange);
bool NetPeerHasStreamData(const NetPeer *peer);
/* В потоках есть байты, ещё не уехавшие или ждущие повтора, пусть даже
 * отправить их сейчас не даёт окно приёма собеседника. */
bool NetPeerStreamPending(const NetPeer *peer);
void NetChannelOnAcked(NetPeer *peer, const NetSentRange *range);
void NetChannelOnLost(NetPeer *peer, const NetSentRange *range);
uint32_t NetChannelReceiveStream(LaiueNetEndpoint *endpoint, NetPeer *peer, uint32_t channelIndex,
                                 uint64_t offset, const uint8_t *bytes, uint32_t length);
uint32_t NetChannelReceiveUnreliable(LaiueNetEndpoint *endpoint, NetPeer *peer,
                                     uint32_t channelIndex, bool sequenced, uint64_t sequence,
                                     const uint8_t *bytes, uint32_t length);

/* net_pmtu.c */
/* Новый путь: размер base, поиск до limit начинается сразу. */
void NetPmtuReset(NetPmtu *pmtu, uint32_t base, uint32_t limit, uint64_t now);
/* Размер пробы, которую пора отправить; 0 — не пора. */
uint32_t NetPmtuProbeSize(const NetPmtu *pmtu, uint64_t now);
void NetPmtuProbeSent(NetPmtu *pmtu, uint32_t size);
/* Исход пробы с номером number, отправленной в sentTime. true —
 * подтверждённый размер вырос. */
bool NetPmtuProbeAcked(NetPmtu *pmtu, uint64_t number, uint32_t size, uint64_t sentTime,
                       uint64_t now);
void NetPmtuProbeLost(NetPmtu *pmtu, uint32_t size, uint64_t sentTime, uint64_t now);
/* Обычные пакеты. true — большие датаграммы пропадают, и размер упал до
 * базового. */
void NetPmtuAcked(NetPmtu *pmtu, uint64_t number, uint32_t size);
bool NetPmtuLost(NetPmtu *pmtu, uint64_t number, uint32_t size, uint64_t now);
/* Пробный таймер сработал timeouts раз подряд, а самый большой пакет в пути,
 * кроме проб, — largest байт. true — размер упал до базового. */
bool NetPmtuTimeout(NetPmtu *pmtu, uint32_t timeouts, uint32_t largest, uint64_t now);
