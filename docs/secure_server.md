# Secure remote server

Удалённая игра использует QUIC поверх UDP с TLS 1.3 и ALPN `laiue/5`.
Production API не откатывается на plaintext TCP: если MsQuic, сертификат
или ключ недоступны, сервер не начинает слушать порт.

## Адрес и порт

Настройки `server.cfg`:

```text
port = 27180
maximum_peers = 16
world_seed = 42
allow_content_downloads = false

address_family = dual
listen_address = *

# Linux:
certificate_file = certs/server-cert.pem
private_key_file = certs/server-key.pem

# Windows вместо PEM:
# certificate_thumbprint = <SHA-1 thumbprint из certificate store>
```

`address_family` принимает:

| Значение | Listener |
|---|---|
| `dual` | IPv4 и IPv6 одновременно; только с `listen_address = *` |
| `ipv4` | IPv4 wildcard или конкретный IPv4 |
| `ipv6` | IPv6 wildcard или конкретный IPv6 |

По умолчанию используются `dual`, `*` и UDP 27180. Конкретный bind address
обязан соответствовать выбранному семейству. Например:

```text
address_family = ipv4
listen_address = 192.0.2.10
port = 30000
```

```text
address_family = ipv6
listen_address = 2001:db8::10
port = 30000
```

Все удалённые параметры можно переопределить окружением:

- `LAIUE_SERVER_PORT`;
- `LAIUE_SERVER_ALLOW_CONTENT_DOWNLOADS`;
- `LAIUE_SERVER_ADDRESS_FAMILY`;
- `LAIUE_SERVER_LISTEN_ADDRESS`;
- `LAIUE_SERVER_CERTIFICATE_FILE`;
- `LAIUE_SERVER_PRIVATE_KEY_FILE`;
- `LAIUE_SERVER_CERTIFICATE_THUMBPRINT`.

В firewall и NAT открывается именно UDP. Для стандартного порта, например:

```sh
sudo ufw allow 27180/udp
```

## Сертификат Linux

Для публичного сервера предпочтителен сертификат от доверенного CA с SAN для
каждого DNS-имени или IP, по которому подключаются игроки. Для закрытого
сервера можно создать self-signed сертификат и передать fingerprint игрокам
по независимому каналу:

```sh
install -d -m 700 certs
openssl req -x509 -newkey rsa:3072 -sha256 -days 365 -nodes \
  -keyout certs/server-key.pem \
  -out certs/server-cert.pem \
  -subj "/CN=laiue server" \
  -addext "subjectAltName=IP:192.0.2.10,IP:2001:db8::10,DNS:game.example.net"
chmod 600 certs/server-key.pem
```

Замените примерные SAN своими адресами. Сертификат для `192.0.2.10` не
подтверждает другой IPv4, IPv6 или DNS name.

Сервер принимает private key только как обычный файл, не symlink, и на
Linux отклоняет права group/other. Ключ нельзя хранить в репозитории,
пакете игры, логах или пересылать игрокам.

Fingerprint leaf certificate вычисляется по DER:

```sh
openssl x509 -in certs/server-cert.pem -outform DER |
  openssl dgst -sha256
```

## Подключение клиента

В `servers.txt` одна запись имеет вид:

```text
name|endpoint|trust
```

Endpoint поддерживает:

```text
IPv4 default|192.0.2.10|system
IPv4 custom|192.0.2.10:30000|system
IPv6 default|2001:db8::10|sha256:<64 hex>
IPv6 custom|[2001:db8::10]:30000|sha256:<64 hex>
DNS|game.example.net:30000|system
```

У IPv6 нестандартный порт всегда записывается в квадратных скобках.
Отсутствующий порт означает 27180. URL, userinfo, пути, port 0 и значения
выше 65535 отклоняются. Старый формат `name|address|port` читается для
миграции как запись с числовым третьим полем.

Trust policy:

- `system` проверяет системную цепочку доверия, срок сертификата и точный
  DNS/IP SAN;
- `sha256:<64 hex>` дополнительно требует точного SHA-256 fingerprint leaf
  DER certificate; self-signed chain допускается только при правильном pin,
  непросроченном сертификате и совпавшем SAN.

TOFU, автоматического принятия первого сертификата и режима
`skip verification` нет. Mismatch завершает handshake до отправки mod list,
snapshot или игровых команд. Session tickets и gameplay в 0-RTT в первой
версии отключены.

## Диагностика

- `secure transport unavailable` — сборка не нашла MsQuic 2.5.9 либо backend
  не включён; plaintext fallback намеренно отсутствует.
- `configuration` — неправильное сочетание address family/bind address,
  отсутствующие credentials или недопустимый порт.
- `certificate` — SAN, срок, системная цепочка или pin не совпали.
- `timeout` — проверьте UDP firewall/NAT, адрес, порт и обратный маршрут.
- При смене сертификата заранее распространите новый pin. Клиент не
  «обновляет» pin после mismatch.

Ограничения сетевого протокола и модель авторитетности описаны в
[multiplayer.md](multiplayer.md).
