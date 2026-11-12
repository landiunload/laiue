#pragma once

/* Датаграммные сокеты UDP и системный резолвер имён: граница ОС для
 * laiue.net.
 *
 * Сокет неблокирующий. Сокет IPv6 принимает и IPv4: такие адреса видны как
 * обычные IPv4, а не как отображённые ::ffff:a.b.c.d. Описатель обнуляется
 * до открытия и принадлежит вызывающему; одним сокетом пользуется один
 * поток. Close безопасен для нулевого и уже закрытого описателя. */

#include <stdbool.h>
#include <stdint.h>

#define PLATFORM_SOCKET_IPV4 4u
#define PLATFORM_SOCKET_IPV6 6u

typedef struct PlatformSocketAddress
{
    /* Сетевой порядок байт; у IPv4 заняты первые четыре. */
    uint8_t ip[16];
    /* Зона IPv6-адреса канального уровня, иначе ноль. */
    uint32_t scope;
    /* Порядок хоста. */
    uint16_t port;
    uint8_t family;
    uint8_t reserved;
} PlatformSocketAddress;

typedef struct PlatformUdpSocket
{
    uintptr_t opaque[2];
} PlatformUdpSocket;

/* Открывает сокет семейства bindAddress и привязывает его. Нулевой адрес
 * означает все интерфейсы, нулевой порт — любой свободный. Размеры буферов
 * ОС — пожелание; ноль оставляет значения системы.
 *
 * Датаграммы сокета система не дробит, где умеет это запретить: слишком
 * большая для пути теряется целиком, и транспорт находит размер пути
 * пробами. Windows и Linux к тому же не сверяют датаграмму с размером пути,
 * который запомнили по ICMP: проверить такое сообщение транспорту нечем.
 * Darwin запрещает дробление только семейству самого сокета, и IPv4 через
 * сокет IPv6 там дробится. */
bool PlatformUdpOpen(const PlatformSocketAddress *bindAddress, uint32_t sendBufferBytes,
                     uint32_t receiveBufferBytes, PlatformUdpSocket *outSocket);
void PlatformUdpClose(PlatformUdpSocket *socket);
/* Адрес, к которому сокет привязан на самом деле: так узнают выданный порт. */
bool PlatformUdpLocalAddress(const PlatformUdpSocket *socket, PlatformSocketAddress *outAddress);

/* 1 — датаграмма передана системе; 0 — не ушла, но сокет исправен (буфер
 * полон, адресат недостижим, датаграмма велика для пути); -1 — сокет
 * неисправен. */
int32_t PlatformUdpSend(PlatformUdpSocket *socket, const PlatformSocketAddress *to,
                        const void *bytes, uint32_t size);
#define PLATFORM_UDP_NONE (-1)
#define PLATFORM_UDP_BROKEN (-2)

/* Забирает из очереди сокета не больше одной датаграммы и возвращает её
 * размер; PLATFORM_UDP_NONE — очередь пуста; PLATFORM_UDP_BROKEN — сокет
 * неисправен. Ноль — тоже взятая датаграмма: пустая либо отголосок чужой
 * ошибки (ответ ICMP на прежнюю отправку), и outFrom тогда не заполнен.
 * Вызов сам ничего не пропускает, поэтому работу приёма ограничивает
 * вызывающий: поток пустых датаграмм не дешевле для отправителя, чем любой
 * другой. Датаграмма длиннее capacity обрезается, и возвращается capacity:
 * буфер на байт больше наибольшей допустимой датаграммы позволяет такие
 * отличить. */
int32_t PlatformUdpReceive(PlatformUdpSocket *socket, PlatformSocketAddress *outFrom, void *buffer,
                           uint32_t capacity);

/* Разрешает имя узла (UTF-8) в адреса системным резолвером. Вызов блокирует
 * поток на время запроса, поэтому его место — вне кадра. Порт в ответе
 * нулевой. Возвращает число записанных адресов в порядке, выбранном
 * системой; 0 — имя не найдено или резолвер недоступен. */
uint32_t PlatformResolveHost(const char *host, PlatformSocketAddress *outAddresses,
                             uint32_t capacity);
