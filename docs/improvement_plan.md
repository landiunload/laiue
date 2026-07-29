# План улучшений

Актуально для laiue 0.5.0. Выполненные возможности описаны в тематических
документах; здесь только незакрытые задачи. Каждый пункт опирается на
измерение из раздела ниже или на правило [CONTRIBUTING.md](../CONTRIBUTING.md).

## Измеренное состояние (29.07.2026)

| Проверка | Результат |
|---|---|
| `windows-msvc` Debug + Release | сборка OK, CTest 21/21 |
| `windows-clang` Debug + Release | сборка OK, CTest 21/21 |
| `windows-msvc-server` Debug + Release | сборка OK, CTest 19/19 |
| Lean MsQuic glibc/musl | pinned source, ≤4 MiB, без XDP/BPF/NUMA dependencies |
| Debian GCC/Clang Debug + Release | сборка OK, CTest 20/20 |
| Debian GCC ASan/UBSan Debug | сборка OK, CTest 20/20 |
| Alpine/musl GCC Debug + Release | сборка OK, CTest 20/20 |
| glibc/musl release archive smoke | права, `$ORIGIN`, IPv4/IPv6/dual/custom port OK |
| Клиент `laiue.exe` | стартует, закрывается штатно |
| `laiue_server` без credentials | выход с кодом 5 и строкой `startup failed: configuration has no usable certificate or private key` |
| Сервер moskva, IPv4 и IPv6 | QUIC + pin + полный initial sync подтверждены probe |

Release/CI больше не используют общий upstream runtime с XDP/BPF/NUMA.
`tools/build_lean_msquic.sh` строит закреплённый профиль с системной OpenSSL
3, а configure/package smoke перепроверяют его metadata, SHA-256, ELF
hardening и допустимый список `NEEDED`.

## P0 — зелёные сборки и запуск

1. **Сквозной тест на Windows.** `laiue.network.quic.integration`
   регистрируется под `if(UNIX AND TARGET laiue_msquic)`, поэтому Windows
   идёт без единой проверки client/server поверх loopback — а это
   основная платформа клиента. Сделать сценарий переносимым (PEM или
   certificate store) либо добавить эквивалентный Windows-случай.

## P0 — воспроизводимый деплой moskva

Сервер работает в dual-stack на UDP 27180: pinned-TLS probes по публичным
IPv4 и IPv6 доходят до `READY`. Release установлен атомарным переключением
`current` с автоматическим rollback. Незакрыта эксплуатационная
автоматизация, а не игровой transport.

1. **Установочный скрипт в репозиторий.** Реальная установка выполнена
   ad-hoc скриптом во временном каталоге сборки: он создаёт сертификат с
   SAN на IPv4/IPv6, раскладывает release, ставит unit, открывает UDP в
   UFW. Такой скрипт обязан лежать в `packaging/linux` параметризованным
   (каталог релиза, список SAN, порт), иначе следующий деплой снова
   станет ручным.
2. **Ротация сертификата.** Self-signed выдан на 365 дней, а его pin
   зашит в `assets/servers.txt`. Нужен план смены без простоя: два
   допустимых pin на время перехода либо переход на доверенный CA с DNS
   именем. Клиент по правилу не «обновляет» pin после mismatch.
3. **Наблюдаемость.** Нет ни логов подключений, ни метрик, ни health
   check. Минимум: журналируемые события peer connect/disconnect и отказ
   handshake, счётчик активных peers, расширение
   `tools/audit_linux_server_host.sh` проверкой слушателя UDP и статуса
   unit.
4. **Гигиена секретов.** Приватный SSH-ключ и файл состояния с паролями
   лежат в одном каталоге с релизными архивами. Правило запрещает
   попадание ключей в build artifacts: развести секреты и артефакты по
   разным каталогам, ограничить права, исключить каталог из любых
   упаковок.
5. **Диагностика IPv6 у клиента.** Запись `Москва IPv6` неотличима для
   игрока от «сервер лежит», если у него нет глобального IPv6. Нужны
   разные сообщения для «нет маршрута» и «таймаут», а также отметка в
   [secure_server.md](secure_server.md).
6. **Автоматизация выката.** Проверка SHA-256, атомарный `current` и rollback
   уже проверены разовым deploy runner. Следующий шаг — перенести этот
   алгоритм в versioned operator tool и запускать его из release pipeline,
   не сохраняя SSH credentials в CI artifact.

## P0 — эксплуатационное усиление multiplayer

Secure QUIC/TLS boundary, IPv4/IPv6 endpoint parsing и protocol v5 snapshot
реализованы. Следующий слой для долгоживущего публичного сервера:

1. Identity/session layer с короткоживущими токенами и отзывом сессий.
2. Полные серверные profiles игроков/инвентарей, метрики и
   административный аудит.
3. Fuzzing decoder, soak/load, DDoS/perimeter limits и проверки ошибочного
   QUIC-потока.
4. Rotation сертификатов/pins без простоя и документированная recovery
   процедура.

Текущие гарантии и ограничения: [multiplayer.md](multiplayer.md).

## P1 — упрощение крупных подсистем

Разделять нужно внутренние файлы существующих DLL, сохраняя публичные C ABI:

- `renderer_d3d12.c`: device/swap chain, ресурсы, chunk, panorama и UI pass;
- `world_infinite.c`: хранение, генерация и фасад `world.h`;
- `chunk_streaming.c`: планировщик, очередь работ и GPU-меши;
- `server/main.c`: конфигурация и запуск, обработка сетевых
  событий, симуляция тика и интерес игроков;
- `pause_menu.c`: состояние/команды и представление;
- Сетевой стек: вынести общий framing/send ownership в
  `network_channel.*`, client/server negotiation/snapshot/READY — в session
  files, а MsQuic callbacks/DNS/bind/handles оставить в
  `network_transport_msquic.c`.

Рефакторинг выполняется после профиля или перед существенным изменением
подсистемы. Новая DLL оправдана только отдельным временем жизни, владельцем
состояния или стабильной границей API.

## P1 — экспериментальный OpenSSL QUIC backend

Linux server на OpenSSL 3.5 QUIC реален и должен быть совместим с Windows
MsQuic client через QUIC v1 и ALPN `laiue/5`, но прямой второй вариант
текущего высокоуровневого backend продублирует session state machines.
Последовательность:

1. Завершить общий channel/session/transport split и fake-transport tests
   для fragmentation/coalescing, delayed completion, overflow, extra
   streams и shutdown generation race.
2. Добавить compile-time `LAIUE_QUIC_BACKEND=msquic|openssl`; OpenSSL
   разрешать только для Linux server-only и версии 3.5+.
3. Проверить listener, полный 1-RTT, точный ALPN, один bidi control stream,
   server uni snapshot/content streams, отсутствие tickets/resumption и
   внешний handshake timeout.
4. Прогнать Windows-client ↔ Debian-server для IPv4/IPv6, default/custom
   port, system/pin, wrong pin, expired/SAN mismatch, wrong ALPN и plaintext.
5. Сравнить MsQuic/OpenSSL под 0/1/5% loss: handshake p95, CPU, RSS,
   player-state age/jitter и snapshot throughput.

OpenSSL 3.5 не предоставляет RFC 9221 application DATAGRAM, поэтому
`PLAYER_STATE` использует reliable fallback. Backend остаётся experimental,
пока этот путь при loss не укладывается в interpolation budget. Выигрыш в
размере существует только при политике системных `libssl.so.3` и
`libcrypto.so.3`; bundling обеих библиотек может оказаться тяжелее lean
MsQuic.

## P1 — производительность и размер

1. Снять CPU/GPU-профиль повторяемых сцен: новый мир, мир с дельтами,
   быстрое движение и локальный сервер с несколькими peer.
2. По измерениям уменьшать remeshing, upload и draw calls; не добавлять
   аллокации и файловый ввод-вывод в frame/fixed-tick paths.
3. Измерить cold start, размер DLL/EXE и рабочий набор до изменения
   модульности или сжатия ресурсов. Текущая точка отсчёта Release
   (`windows-clang`): `laiue_core.dll` 128 КиБ, `laiue.exe` и все
   модульные DLL вместе 367 КиБ без `msquic.dll` (525 КиБ), рабочий набор
   в главном меню около 80 МиБ.
4. Сохранять лёгкое главное меню: без мира, стриминга, физики, ModHost и
   игровых GPU-ресурсов до запуска сессии.

У каждой оптимизации должны быть исходное измерение, целевой показатель и
повторяемая сцена. Счётчики CPU доступны через HUD `F3`.

## P2 — совместимость

- Зафиксировать размеры и смещения `sdk/laiue_mod_api.h`; проверить старый
  мод при расширении таблицы API.
- Версионировать бинарные форматы только при несовместимых изменениях и
  сохранять чтение предыдущей версии, где это возможно.
- Не ослаблять exact-size, bounds, state, sequence и rate проверки сети:
  сборка с `/GS-` переносит больше ответственности на код.
- `protocol.c` собирается с `/fp:precise` (см. `src/network/CMakeLists.txt`).
  Глобальный `/fp:fast` на clang-cl означает `-ffinite-math-only`, и тогда
  проверки `IsFiniteFloat`/`IsFinitePosition` теряют силу — `EncodeInput`
  доказанно принимал NaN во все поля. Флаг обязан оставаться; тест
  `TestNonFiniteRejected` это стережёт. Битовая проверка не помогает:
  finite-math-only — аксиома, а не пооперационное преобразование.

## Тесты

Решение «автоматические тесты не добавляются» отменено: одной успешной сборки
недостаточно. Проверка диапазона типов сообщений была прибита к
`SERVER_CONTENT_END`, поэтому добавленные позже дроп, инвентарь и выбор слота
не проходили `WriteHeader`; вместо отправки вызывающий код рвал соединение как
OVERFLOW. Сборка при этом была чистой, а обработчики приёма — недостижимы.

Сейчас регистрируется 21 тест на Windows и 20 на Linux: направления
include-зависимостей, контракт аудио-API, кодек протокола и проверка
сертификата, DATAGRAM queue/freshness/fallback, отображение команд игрока,
конфигурация сервера, source-tree guard, очередь
вводов и планировщик снапшотов, арифметика `InfiniteCoord`, снапшот мира,
побитовый детерминизм физики, репликация игрока, форматы контента и
правила имён паков и модов. Разница между платформами — сквозной
QUIC-сценарий, он существует только под UNIX.
Незакрытое:

- сквозной client/server сценарий на Windows (см. P0);
- fuzzing декодера и ошибочный сетевой поток;
- проверки формата сохранений при изменении раскладки;
- покрытие мешера и сохранений.

Перед изменением SDK, дисковых или сетевых форматов тест на новую раскладку
пишется вместе с изменением.

## Порядок работ

Пункты P0 упорядочены так, чтобы каждый следующий опирался на уже
проверяемый результат: сначала переносимый Windows QUIC test, затем
наблюдаемость/ротация и только после этого versioned operator tool для
повторяемого деплоя.

## Почему сейчас не ECS

Основные объекты уникальны (`World`, `Renderer`, игрок, стриминг), а воксели
живут в чанках. ECS сейчас добавит косвенный доступ, таблицы идентификаторов и
сложную границу владения между DLL без измеримой пользы.

Вернуться к ECS стоит, когда появятся тысячи однотипных сущностей и несколько
систем будут регулярно обходить одинаковые наборы компонентов. Даже тогда
мир чанков, renderer и UI не обязаны становиться частью ECS.
