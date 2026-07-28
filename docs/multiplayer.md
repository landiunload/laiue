# Мультиплеер

Версия 0.5.0 использует общий authoritative client/server stack:

- `laiue_server.exe`/`laiue_server` — отдельный headless server для
  Windows и Linux;
- production transport — MsQuic/UDP с TLS 1.3 и ALPN `laiue/5`;
- IPv4 и IPv6 можно слушать вместе либо выбрать одно семейство;
- сервер считает физику с частотой 60 Гц и отправляет состояние с частотой
  20 Гц;
- клиент отправляет намерения, предсказывает своё движение и выполняет
  reconciliation;
- сервер владеет блоками, revisions чанков, инвентарём, дропами и подбором
  предметов.

Соединение проходит состояния `CONNECTING`, `VERIFYING_SERVER`,
`NEGOTIATING`, `SYNCING_WORLD`, `READY`. Gameplay input принимается только в
`READY`: сначала завершается TLS 1-RTT, затем сравниваются моды и передаются
seed, snapshot изменённых чанков, world time, inventory, drops и player
roster.

## Запуск

```powershell
$env:LAIUE_MSQUIC_ROOT = 'C:\deps\msquic-2.5.9'
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --parallel
cd build/windows-msvc/bin/Release
./laiue_server.exe
./laiue.exe
```

Linux server конфигурируется через `linux-gcc` или `linux-clang`, затем
собирается одноимённым build-preset с суффиксом `-debug`/`-release`.
Клиент подключается через «Сетевая игра»; список находится в `servers.txt`:

```text
Локальный IPv4|127.0.0.1|sha256:<64 hex>
Локальный IPv6|[::1]:27180|sha256:<64 hex>
Публичный|game.example.net:30000|system
```

Синтаксис — `name|endpoint|trust`; старый `name|address|port` читается для
миграции. Отсутствующий порт означает 27180. У bare IPv6 допустим только
default port, IPv6 с явным портом записывается как `[address]:port`.
Переключатель `Авто | IPv4 | IPv6` в меню ограничивает разрешение DNS-имён.
IPv4- и IPv6-literal всегда однозначно выбирают своё семейство независимо от
переключателя.

Настройки сервера — в `server.cfg`:

```text
port = 27180
maximum_peers = 16
world_seed = 42
allow_content_downloads = false
address_family = dual
listen_address = *
certificate_file = certs/server-cert.pem
private_key_file = certs/server-key.pem
```

На Windows вместо PEM указывается `certificate_thumbprint` из certificate
store. Все network-поля имеют `LAIUE_SERVER_*` env overrides. Полная
настройка сертификата, IPv4/IPv6 и firewall:
[secure_server.md](secure_server.md).

## Проверка модов

До создания игрока сервер отправляет упорядоченный список обязательных модов.
Клиент сравнивает `id`, `version` и SHA-256 fingerprint полного pack:
manifest v2 и все объявленные Windows/glibc/musl binaries.

- лишний или отсутствующий `server`/`both` мод отклоняет подключение;
- `client`-моды в сравнении не участвуют;
- установленные, но выключенные моды можно включить через подтверждение UI;
- порядок модов является частью совместимости.

Только после точного ответа клиента сервер отправляет `SERVER_WELCOME` и
начинает принимать input. Стороны модов описаны в
[modding.md](modding.md).

## Первоначальная синхронизация

Процедурная база мира восстанавливается по server seed. Сервер передаёт
только сохранённые правки чанков в authoritative interest window. Snapshot
имеет id, общую world revision и точное число bounded chunk records.

Каждая live block/chunk delta содержит `uint64_t revision`. События,
появившиеся во время snapshot, применяются после его конца; пропуск revision
вызывает bounded chunk resync. Клиент удалённой сессии не читает и не
перезаписывает локальный world save.

Roster синхронизируется событиями player joined/left, а player states
интерполируются между серверными обновлениями. Inventory и active drops
передаются до `READY` и далее меняются только server-authoritative событиями.

## Загрузка содержимого

Если `allow_content_downloads = true`, сервер может предложить `.lmp`, `.lsp`
и `.ltp`. Загрузка начинается только после подтверждения пользователя.

Bundle ограничен 256 МиБ и 4096 файлами. Проверяются SHA-256, размеры, типы,
относительные пути и отсутствие `..`/reparse points. Распаковка идёт в
staging `*.download`, прежний пак сохраняется как `*.previous`, затем каталог
переключается и handshake повторяется.

SHA-256 подтверждает целостность полученного потока, но серверный `.lmp` всё
равно содержит нативный код с правами процесса.

## Авторитетность

| Состояние | Владелец и проверка |
|---|---|
| позиция и скорость | сервер; клиент их не присылает |
| движение | серверная fixed-step физика; клиент присылает кнопки и взгляд |
| ломание/установка | серверный raycast, дистанция, таймер, выбранный слот |
| инвентарь и дропы | сервер; bounded снимок 36 слотов |
| моды | точный ordered fingerprint до допуска |
| wire-поток | magic, version, exact size, sequence, state и rate limits |

Обычный payload ограничен 1024 байтами, chunk snapshot — 128 правками, весь
snapshot — 4096 чанками. Ограничены streams, callback/event queues,
outstanding sends и число peer. Неверный размер, NaN/Infinity, повтор
sequence, сообщение не в той фазе или overflow закрывают виновное соединение.

## Безопасность сборки

Windows EXE/DLL собираются с `/GS-`, без CFG и CET compatibility; ASLR и
DEP/NX сохранены. Linux CI дополнительно использует ASan/UBSan. Поэтому
проверки входных данных нельзя ослаблять. Свойства «взломать невозможно» не
бывает: TLS аутентифицирует канал и сервер, но не устраняет ошибки памяти,
вредоносные моды, DDoS и уязвимости ОС.

## Ограничения первой версии

- TLS аутентифицирует сервер; клиенты пока анонимны, account/session tokens
  отсутствуют.
- Состояние игрока и inventory между независимыми подключениями пока не
  сохраняется как постоянный profile.
- QUIC datagrams и 0-RTT отключены; control/gameplay используют один
  bidirectional stream, snapshots/content — server-initiated streams.
- Для публичного сервера всё равно нужны обновления ОС/MsQuic, rate limiting
  на периметре, мониторинг и резервные копии.
