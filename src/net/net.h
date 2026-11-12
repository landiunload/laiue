#pragma once

/* laiue.net — транспорт поверх датаграмм без сетевой политики.
 *
 * Модуль доставляет сообщения между конечными точками и не знает, что в них
 * лежит: протокол приложения, сервер, репликация, лобби и права участников
 * остаются приложению. Клиент и сервер — одна и та же конечная точка; та,
 * что принимает входящие соединения, отличается одним флагом.
 *
 * Что даёт транспорт:
 *  - соединение с обменом ключами X25519 и шифрованием ChaCha20-Poly1305
 *    каждого пакета; сервер можно закрепить открытым ключом, а круг
 *    участников — общим секретом;
 *  - каналы: надёжные упорядоченные сообщения любого размера до
 *    maxMessageBytes и ненадёжные сообщения в одну датаграмму, в том числе
 *    с отбрасыванием устаревших;
 *  - подтверждения, обнаружение потерь, повторную отправку, управление
 *    перегрузкой с размеренной отправкой, проверку связи и тайм-ауты;
 *  - защиту от подмены адреса и усиления: до проверки обратного пути сервер
 *    не хранит состояние и отвечает короче запроса;
 *  - жёсткие пределы памяти на соединение и на конечную точку.
 *
 * Сеть подключается таблицей LaiueNetDriverV1: готовый драйвер UDP, свой
 * (ретранслятор, сеть платформы) или симулятор из этого же модуля, с
 * которым сетевой код приложения проверяется детерминированно: задержки,
 * потери, перестановки, повторы и порча задаются числом, а время движет
 * сам тест.
 *
 * Потоки: конечная точка, сокет и симулятор принадлежат одному потоку.
 * Разные конечные точки независимы. Время передаёт вызывающий — монотонные
 * микросекунды с произвольным началом отсчёта. */

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t LaiueNetStatus;
#define LAIUE_NET_OK UINT32_C(0)
#define LAIUE_NET_INVALID_ARGUMENT UINT32_C(1)
#define LAIUE_NET_OUT_OF_MEMORY UINT32_C(2)
/* Возможности нет на этой платформе: например, сокетов. */
#define LAIUE_NET_UNSUPPORTED UINT32_C(3)
/* Драйвер или ОС отказали: сокет не открылся или сломался. */
#define LAIUE_NET_IO_ERROR UINT32_C(4)
/* Описатель участника устарел: соединение уже закрыто. */
#define LAIUE_NET_NO_PEER UINT32_C(5)
#define LAIUE_NET_LIMIT_REACHED UINT32_C(6)
/* Очередь отправки полна; сообщение не принято, повторите позже. */
#define LAIUE_NET_WOULD_BLOCK UINT32_C(7)
/* Сообщение больше, чем собеседник согласен принять этим способом. */
#define LAIUE_NET_TOO_LARGE UINT32_C(8)
/* Соединение ещё не установлено или уже закрывается. */
#define LAIUE_NET_NOT_CONNECTED UINT32_C(9)

#define LAIUE_NET_KEY_BYTES 32u
#define LAIUE_NET_MAX_CHANNELS 64u

/* ---- Адреса ----------------------------------------------------------------- */

#define LAIUE_NET_FAMILY_IPV4 4u
#define LAIUE_NET_FAMILY_IPV6 6u
/* Ёмкости текста с запасом для любого адреса с зоной и портом. */
#define LAIUE_NET_ADDRESS_TEXT_CAPACITY 64u

typedef struct LaiueNetAddress
{
    /* Сетевой порядок байт; у IPv4 заняты первые четыре. */
    uint8_t ip[16];
    /* Зона IPv6-адреса канального уровня, иначе ноль. */
    uint32_t scope;
    /* Порядок хоста. */
    uint16_t port;
    uint8_t family;
    uint8_t reserved;
} LaiueNetAddress;

/* Разбирает "203.0.113.7:4000", "[2001:db8::1]:4000", "[fe80::1%3]:4000".
 * Без порта ("203.0.113.7", "2001:db8::1") порт равен нулю. Имена узлов
 * здесь не разрешаются: для них есть LaiueNetResolve. */
LAIUE_NET_API LaiueNetStatus LaiueNetAddressParse(const char *text, LaiueNetAddress *outAddress);
/* Пишет текст с завершающим нулём и возвращает его длину; 0 — адрес неверен
 * или не поместился. */
LAIUE_NET_API uint32_t LaiueNetAddressFormat(const LaiueNetAddress *address, char *output,
                                             uint32_t capacity);
LAIUE_NET_API bool LaiueNetAddressEqual(const LaiueNetAddress *first,
                                        const LaiueNetAddress *second);

/* ---- Драйвер датаграмм ------------------------------------------------------ */

/* Сеть, какой её видит транспорт: ненадёжная доставка датаграмм по адресу.
 * Вызовы не блокируют. Таблица копируется при создании конечной точки;
 * context должен жить дольше неё. Датаграмму больше пути драйвер теряет, а
 * не дробит: транспорт ищет размер пути пробами, и фрагменты скрыли бы от
 * него настоящий. */
typedef struct LaiueNetDriverV1
{
    uint32_t structSize;
    void *context;
    /* 1 — датаграмма принята к отправке; 0 — потеряна, но драйвер исправен;
     * меньше нуля — драйвер неисправен. */
    int32_t (*send)(void *context, const LaiueNetAddress *to, const void *bytes, uint32_t size);
    /* Размер принятой датаграммы; 0 — пока ничего нет; меньше нуля — драйвер
     * неисправен. Датаграмма длиннее capacity обрезается до capacity. */
    int32_t (*receive)(void *context, LaiueNetAddress *outFrom, void *buffer, uint32_t capacity);
} LaiueNetDriverV1;

/* ---- Конечная точка --------------------------------------------------------- */

typedef struct LaiueNetEndpoint LaiueNetEndpoint;

/* Описатель участника: 0 — пустой. Описатель закрытого соединения не
 * совпадает ни с одним живым. */
typedef uint64_t LaiueNetPeer;

/* Принимать входящие соединения. */
#define LAIUE_NET_ENDPOINT_ACCEPT UINT32_C(1)

/* Источник случайных байт для ключей и идентификаторов. false — отказ. */
typedef bool (*LaiueNetRandomFn)(void *context, void *output, uint32_t size);

typedef struct LaiueNetEndpointDesc
{
    uint32_t structSize;
    uint32_t flags;
    const LaiueNetDriverV1 *driver;
    /* Протокол приложения и его версия. Стороны с разными значениями не
     * соединяются. */
    uint64_t protocolId;
    /* Предел одновременных соединений; 0 — 64. */
    uint32_t maxPeers;
    /* Число каналов, от 1 до LAIUE_NET_MAX_CHANNELS; 0 — один. Соединение
     * пользуется меньшим из чисел двух сторон. */
    uint32_t channelCount;
    /* Наибольшая датаграмма, от 576 до 1452 байт; 0 — 1452. Стороны берут
     * меньшее. Соединение начинает с 1200 байт, что проходит почти по любому
     * пути в интернете, и поднимает размер, пока путь его пропускает. */
    uint32_t maxDatagramBytes;
    /* Наибольшее надёжное сообщение, которое примет эта сторона; 0 — 1 МиБ. */
    uint32_t maxMessageBytes;
    /* Сколько неподтверждённых байт надёжных сообщений держит очередь одного
     * соединения; 0 — 4 МиБ. Сверх этого LaiueNetSend отвечает
     * LAIUE_NET_WOULD_BLOCK. */
    uint32_t sendBufferBytes;
    /* Сколько ненадёжное сообщение может ждать отправки. Окно перегрузки и
     * размеренная отправка задерживают его на плохом пути, а состояние,
     * пришедшее с опозданием, хуже свежего: дождавшееся срока сообщение
     * выбрасывается. 0 — 200 мс. */
    uint32_t unreliableLifetimeMilliseconds;
    /* Окно приёма одного канала: столько байт собеседник может прислать
     * впереди ещё не полученных; округляется вверх до степени двойки.
     * 0 — 256 КиБ. Вместе с maxMessageBytes это предел памяти канала. */
    uint32_t receiveWindowBytes;
    /* Память неразобранных событий всех соединений. Сверх неё конечная точка
     * перестаёт читать сеть, пока приложение не заберёт события: датаграммы
     * ждут в драйвере, а потерянное там собеседники пришлют повторно.
     * 0 — 64 МиБ. */
    uint32_t eventBacklogBytes;
    /* 0 — 10 секунд. */
    uint32_t handshakeTimeoutMilliseconds;
    /* Соединение без единого пакета за это время считается потерянным;
     * 0 — 30 секунд. Стороны берут меньшее, а проверку связи шлют сами. */
    uint32_t idleTimeoutMilliseconds;
    /* Закрытый ключ сервера, LAIUE_NET_KEY_BYTES байт, либо NULL. С ним
     * клиент, знающий открытый ключ, убеждается, что говорит с этим
     * сервером. */
    const uint8_t *identityPrivateKey;
    /* Общий секрет, LAIUE_NET_KEY_BYTES байт, либо NULL. Соединяются только
     * стороны с одинаковым секретом. */
    const uint8_t *presharedKey;
    /* NULL — системный генератор. Свой источник делает рукопожатие
     * воспроизводимым в тестах; в игре он обязан быть криптографически
     * стойким. */
    LaiueNetRandomFn random;
    void *randomContext;
} LaiueNetEndpointDesc;

typedef struct LaiueNetConnectDesc
{
    uint32_t structSize;
    LaiueNetAddress address;
    /* Открытый ключ сервера, LAIUE_NET_KEY_BYTES байт, либо NULL. Без него
     * и без общего секрета соединение зашифровано, но собеседник не
     * проверен: активный посредник на пути может им притвориться. */
    const uint8_t *serverPublicKey;
    void *userData;
} LaiueNetConnectDesc;

typedef uint32_t LaiueNetDelivery;
/* Каждое сообщение приходит один раз и в порядке отправки внутри канала. */
#define LAIUE_NET_RELIABLE UINT32_C(0)
/* Одна датаграмма: сообщение может потеряться или прийти не по порядку,
 * но не дважды. Размер ограничен maxUnreliableBytes соединения. */
#define LAIUE_NET_UNRELIABLE UINT32_C(1)
/* То же, но сообщение, отправленное раньше уже полученного, отбрасывается. */
#define LAIUE_NET_SEQUENCED UINT32_C(2)

/* Сначала доставить надёжные сообщения из очереди, затем закрыть. */
#define LAIUE_NET_DISCONNECT_FLUSH UINT32_C(1)

typedef uint32_t LaiueNetEventType;
/* Соединение установлено: исходящее — после ответа сервера, входящее —
 * когда клиент доказал владение ключами. */
#define LAIUE_NET_EVENT_CONNECTED UINT32_C(1)
/* Соединение закрыто; описатель больше не действует. Приходит ровно один
 * раз для каждого участника, о котором приложение знает. */
#define LAIUE_NET_EVENT_DISCONNECTED UINT32_C(2)
#define LAIUE_NET_EVENT_MESSAGE UINT32_C(3)

/* Причины LAIUE_NET_EVENT_DISCONNECTED. */
/* Закрыто этой стороной вызовом LaiueNetDisconnect. */
#define LAIUE_NET_REASON_LOCAL UINT32_C(1)
/* Закрыто собеседником; code — его код. */
#define LAIUE_NET_REASON_REMOTE UINT32_C(2)
/* Собеседник не ответил вовремя. */
#define LAIUE_NET_REASON_TIMEOUT UINT32_C(3)
/* Сервер отказал; code — LAIUE_NET_REJECT_*. */
#define LAIUE_NET_REASON_REJECTED UINT32_C(4)
/* Собеседник нарушил протокол либо сообщил о нарушении. */
#define LAIUE_NET_REASON_PROTOCOL UINT32_C(5)
/* Не хватило памяти или генератор случайных чисел отказал — у этой стороны
 * либо, по её сообщению, у собеседника. */
#define LAIUE_NET_REASON_RESOURCE UINT32_C(6)
/* Ответ сервера не прошёл проверку: закреплённый ключ или общий секрет не
 * совпали. Либо соединению пришло столько поддельных пакетов (2^36), что
 * подлинности остальных больше нельзя доверять. */
#define LAIUE_NET_REASON_AUTHENTICATION UINT32_C(7)

#define LAIUE_NET_REJECT_VERSION UINT32_C(1)
#define LAIUE_NET_REJECT_PROTOCOL UINT32_C(2)
#define LAIUE_NET_REJECT_FULL UINT32_C(3)
/* Клиент закрепил ключ сервера, а у сервера его нет. */
#define LAIUE_NET_REJECT_IDENTITY UINT32_C(4)
/* Общий секрет задан только у одной стороны. */
#define LAIUE_NET_REJECT_PRESHARED_KEY UINT32_C(5)

typedef struct LaiueNetEvent
{
    uint32_t structSize;
    LaiueNetEventType type;
    LaiueNetPeer peer;
    void *userData;
    /* MESSAGE: канал и способ доставки. */
    uint32_t channel;
    LaiueNetDelivery delivery;
    /* DISCONNECTED: причина и её код. */
    uint32_t reason;
    uint32_t code;
    /* MESSAGE: байты действительны до следующего LaiueNetUpdate. */
    const void *data;
    uint32_t size;
} LaiueNetEvent;

typedef uint32_t LaiueNetPeerState;
#define LAIUE_NET_PEER_CONNECTING UINT32_C(1)
#define LAIUE_NET_PEER_CONNECTED UINT32_C(2)
/* Идёт LaiueNetDisconnect с LAIUE_NET_DISCONNECT_FLUSH. */
#define LAIUE_NET_PEER_DISCONNECTING UINT32_C(3)

typedef struct LaiueNetPeerInfo
{
    uint32_t structSize;
    LaiueNetPeerState state;
    LaiueNetAddress address;
    void *userData;
    /* Согласованные пределы; до установления соединения — нули. */
    uint32_t channelCount;
    uint32_t maxMessageBytes;
    uint32_t maxUnreliableBytes;
    /* Размер датаграмм сейчас — сколько пропускает путь: от 1200 байт до
     * согласованного предела, а при пределе меньше 1200 — сам предел. До
     * установления соединения — 0. */
    uint32_t datagramBytes;
    /* Сглаженное время оборота и его отклонение; 0 — ещё не измерено. */
    uint32_t roundTripMicroseconds;
    uint32_t roundTripVarianceMicroseconds;
    uint32_t congestionWindowBytes;
    uint32_t bytesInFlight;
    uint32_t queuedReliableBytes;
    uint32_t queuedUnreliableBytes;
    uint64_t packetsSent;
    uint64_t packetsReceived;
    uint64_t packetsLost;
    uint64_t bytesSent;
    uint64_t bytesReceived;
    /* Ненадёжные сообщения, выброшенные до отправки: устаревшие и вытесненные
     * из полной очереди. Рост значит, что приложение шлёт больше, чем
     * пропускает путь. */
    uint64_t unreliableDropped;
} LaiueNetPeerInfo;

typedef struct LaiueNetEndpointStats
{
    uint32_t structSize;
    /* Соединения, занимающие место в пределе maxPeers: установленные,
     * устанавливаемые и принятые, но ещё не подтвердившие ключи. */
    uint32_t peerCount;
    uint64_t datagramsSent;
    uint64_t datagramsReceived;
    /* Не прошедшие разбор или проверку подлинности, чужие и повторные. */
    uint64_t datagramsDropped;
    uint64_t bytesSent;
    uint64_t bytesReceived;
} LaiueNetEndpointStats;

LAIUE_NET_API LaiueNetStatus LaiueNetEndpointCreate(const LaiueNetEndpointDesc *desc,
                                                    LaiueNetEndpoint **outEndpoint);
/* Собеседникам уходит уведомление о закрытии; событий больше не будет. */
LAIUE_NET_API void LaiueNetEndpointDestroy(LaiueNetEndpoint *endpoint);

/* Начинает соединение и сразу возвращает описатель. Итог придёт событием:
 * CONNECTED либо DISCONNECTED с причиной. */
LAIUE_NET_API LaiueNetStatus LaiueNetConnect(LaiueNetEndpoint *endpoint,
                                             const LaiueNetConnectDesc *desc,
                                             LaiueNetPeer *outPeer);
/* Закрывает соединение; code получит собеседник. Событие DISCONNECTED с
 * причиной LOCAL приходит сразу, а с LAIUE_NET_DISCONNECT_FLUSH — когда
 * очередь надёжных сообщений доставлена. Повторный вызов без флага
 * обрывает ожидание. */
LAIUE_NET_API LaiueNetStatus LaiueNetDisconnect(LaiueNetEndpoint *endpoint, LaiueNetPeer peer,
                                                uint32_t code, uint32_t flags);

/* Ставит сообщение в очередь; байты копируются. В сеть оно уходит при
 * LaiueNetUpdate, мелкие сообщения собираются в общие датаграммы. */
LAIUE_NET_API LaiueNetStatus LaiueNetSend(LaiueNetEndpoint *endpoint, LaiueNetPeer peer,
                                          uint32_t channel, LaiueNetDelivery delivery,
                                          const void *bytes, uint32_t size);

/* Принимает датаграммы, двигает таймеры и отправляет накопленное. Вызывается
 * каждый кадр или такт сервера. nowMicroseconds не убывает.
 * LAIUE_NET_IO_ERROR — драйвер неисправен; соединения доживают до своих
 * тайм-аутов. */
LAIUE_NET_API LaiueNetStatus LaiueNetUpdate(LaiueNetEndpoint *endpoint, uint64_t nowMicroseconds);
/* Забирает очередное событие. outEvent->structSize задаёт вызывающий.
 * Неразобранные события ждут следующих вызовов. */
LAIUE_NET_API bool LaiueNetPollEvent(LaiueNetEndpoint *endpoint, LaiueNetEvent *outEvent);

LAIUE_NET_API LaiueNetStatus LaiueNetPeerGetInfo(const LaiueNetEndpoint *endpoint,
                                                 LaiueNetPeer peer, LaiueNetPeerInfo *outInfo);
LAIUE_NET_API LaiueNetStatus LaiueNetPeerSetUserData(LaiueNetEndpoint *endpoint, LaiueNetPeer peer,
                                                     void *userData);
LAIUE_NET_API LaiueNetStatus LaiueNetEndpointGetStats(const LaiueNetEndpoint *endpoint,
                                                      LaiueNetEndpointStats *outStats);

/* Создаёт пару ключей сервера системным генератором. */
LAIUE_NET_API LaiueNetStatus LaiueNetGenerateKeyPair(uint8_t privateKey[LAIUE_NET_KEY_BYTES],
                                                     uint8_t publicKey[LAIUE_NET_KEY_BYTES]);
/* Открытый ключ по закрытому. */
LAIUE_NET_API LaiueNetStatus LaiueNetPublicKey(const uint8_t privateKey[LAIUE_NET_KEY_BYTES],
                                               uint8_t publicKey[LAIUE_NET_KEY_BYTES]);
/* Монотонные часы платформы в микросекундах: годятся для LaiueNetUpdate. */
LAIUE_NET_API uint64_t LaiueNetClockMicroseconds(void);

/* ---- UDP ---------------------------------------------------------------------- */

typedef struct LaiueNetUdp LaiueNetUdp;

typedef struct LaiueNetUdpDesc
{
    uint32_t structSize;
    /* Семейство выбирает вид сокета; сокет IPv6 принимает и IPv4. Нулевой
     * адрес — все интерфейсы, нулевой порт — любой свободный. */
    LaiueNetAddress bindAddress;
    /* Пожелания к буферам ОС; 0 оставляет значения системы. */
    uint32_t sendBufferBytes;
    uint32_t receiveBufferBytes;
} LaiueNetUdpDesc;

/* Сокет запрещает системе дробить свои датаграммы. LAIUE_NET_UNSUPPORTED —
 * у платформы нет сокетов; транспорт при этом работает со своим драйвером. */
LAIUE_NET_API LaiueNetStatus LaiueNetUdpOpen(const LaiueNetUdpDesc *desc, LaiueNetUdp **outUdp);
/* Закрывать после конечной точки, которая пользуется сокетом. */
LAIUE_NET_API void LaiueNetUdpClose(LaiueNetUdp *udp);
LAIUE_NET_API LaiueNetStatus LaiueNetUdpGetDriver(LaiueNetUdp *udp, LaiueNetDriverV1 *outDriver);
/* Адрес, к которому сокет привязан на самом деле: так узнают выданный порт. */
LAIUE_NET_API LaiueNetStatus LaiueNetUdpGetLocalAddress(const LaiueNetUdp *udp,
                                                        LaiueNetAddress *outAddress);
/* Разрешает имя узла или текстовый адрес. Имя спрашивается у системного
 * резолвера, и вызов блокирует поток: его место — в фоновой задаче.
 * outCount — сколько адресов записано, каждому проставлен port. */
LAIUE_NET_API LaiueNetStatus LaiueNetResolve(const char *host, uint16_t port,
                                             LaiueNetAddress *outAddresses, uint32_t capacity,
                                             uint32_t *outCount);

/* ---- Симулятор сети ----------------------------------------------------------- */

typedef struct LaiueNetSimulator LaiueNetSimulator;

typedef struct LaiueNetSimulatorDesc
{
    uint32_t structSize;
    /* Предел датаграмм в пути; 0 — 65536. Лишние теряются. */
    uint32_t maxQueuedDatagrams;
    /* Зерно: одинаковое зерно и одинаковые вызовы дают одинаковую сеть. */
    uint64_t seed;
} LaiueNetSimulatorDesc;

typedef struct LaiueNetSimulatorConditions
{
    uint32_t structSize;
    /* Задержка каждой датаграммы и случайная добавка к ней от нуля до
     * jitter; добавка переставляет датаграммы. */
    uint32_t latencyMicroseconds;
    uint32_t jitterMicroseconds;
    /* Доли на миллион датаграмм. */
    uint32_t lossPerMillion;
    uint32_t duplicatePerMillion;
    /* Портит один случайный бит. */
    uint32_t corruptPerMillion;
    /* Датаграммы длиннее теряются; 0 — без предела. */
    uint32_t maxDatagramBytes;
    /* Пропускная способность узкого места на пути к каждому порту;
     * 0 — без предела. Очередь перед ним вмещает queueMicroseconds времени
     * передачи (0 — 200 мс), остальное теряется, как в маршрутизаторе. */
    uint32_t bytesPerSecond;
    uint32_t queueMicroseconds;
} LaiueNetSimulatorConditions;

/* Видит каждую отправленную датаграмму раньше условий сети и может менять
 * её байты на месте. false — потерять. */
typedef bool (*LaiueNetSimulatorFilter)(void *context, const LaiueNetAddress *from,
                                        const LaiueNetAddress *to, uint8_t *bytes, uint32_t size);

LAIUE_NET_API LaiueNetStatus LaiueNetSimulatorCreate(const LaiueNetSimulatorDesc *desc,
                                                     LaiueNetSimulator **outSimulator);
/* Уничтожать после конечных точек, которые пользуются его портами. */
LAIUE_NET_API void LaiueNetSimulatorDestroy(LaiueNetSimulator *simulator);
/* Открывает порт с данным адресом и возвращает его драйвер. Драйвер
 * действует, пока жив симулятор. */
LAIUE_NET_API LaiueNetStatus LaiueNetSimulatorOpen(LaiueNetSimulator *simulator,
                                                   const LaiueNetAddress *address,
                                                   LaiueNetDriverV1 *outDriver);
/* NULL возвращает идеальную сеть: без задержек и потерь. */
LAIUE_NET_API LaiueNetStatus LaiueNetSimulatorSetConditions(
    LaiueNetSimulator *simulator, const LaiueNetSimulatorConditions *conditions);
LAIUE_NET_API LaiueNetStatus LaiueNetSimulatorSetFilter(LaiueNetSimulator *simulator,
                                                        LaiueNetSimulatorFilter filter,
                                                        void *context);
/* Двигает часы сети: датаграммы, чьё время доставки наступило, становятся
 * видны получателям. Время не убывает. */
LAIUE_NET_API LaiueNetStatus LaiueNetSimulatorAdvance(LaiueNetSimulator *simulator,
                                                      uint64_t nowMicroseconds);
