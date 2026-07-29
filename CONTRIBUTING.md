# Разработка laiue

## Локальная проверка

Канонические платформы: Windows x86_64 (клиент и сервер) и Linux x86_64
(сервер). Нужны CMake 3.28+ и MSVC/clang-cl на Windows либо GCC/Clang на
Linux. Подробная матрица и зависимости — в
[docs/portability.md](docs/portability.md).

```powershell
$env:LAIUE_MSQUIC_ROOT = 'C:\deps\msquic-2.5.9'
cmake --preset windows-clang
cmake --build --preset windows-clang-debug --parallel
ctest --preset windows-clang-debug

cmake --build --preset windows-clang-release --parallel
ctest --preset windows-clang-release

pwsh -NoProfile -File tools/check_architecture.ps1
git diff --check
```

Минимальная server-only проверка на Debian/Ubuntu после создания
проверенного lean-prefix по
[инструкции](docs/portability.md#debian-13):

```sh
export LAIUE_MSQUIC_ROOT=/opt/msquic-2.5.9
cmake --preset linux-gcc-asan
cmake --build --preset linux-gcc-asan-debug \
  --target laiue_server_bundle
ctest --preset linux-gcc-asan-debug
```

Configure выполняется один раз на toolchain; Debug и Release выбираются
одноимённым build/test-preset с суффиксом `-debug` или `-release`.
Sanitizer-проверка существует только как `linux-gcc-asan-debug`.
Все канонические configure-presets задают `LAIUE_REQUIRE_MSQUIC=ON`:
отсутствие secure transport является ошибкой configure, а не основанием
включить plaintext fallback. Prefix передаётся через `LAIUE_MSQUIC_ROOT`
или `-DLAIUE_MSQUIC_ROOT=...`.

Тесты живут в `tests/` и регистрируются в CTest; test-preset назван так же,
как build-preset. Тест аудио-API возвращает 125 и помечается пропущенным,
если в системе нет Media Foundation (Windows N/Server без Media Pack).
Тест протокола оборудования не требует и пропусков не имеет: он компилирует
`src/network/protocol.c` в себя, поэтому кодек можно проверять, не экспортируя
его из `laiue_network`.

CI собирает Windows MSVC Debug/Release, Debian GCC/Clang и Alpine/musl,
включает warnings-as-errors, запускает доступные CTest/smoke checks и
проверяет, что сборка не изменила tracked-файлы. Проверка source tree всегда
получает явный repository root и не зависит от текущего каталога runner.

## Блокировка DLL при сборке

Перед пересборкой закройте `laiue.exe` и `laiue_server.exe` из того же
`build/.../bin/<Configuration>`. Иначе Windows не даст линкеру заменить
EXE/DLL (`permission denied`, `LNK1104`). Корневая сборка сама показывает
имя, PID и путь блокирующего процесса.

Штатно закрывайте клиент через меню, сервер — через `Ctrl+C`. Для
зависшего процесса сначала проверьте путь:

```powershell
Get-Process laiue,laiue_server -ErrorAction SilentlyContinue |
    Select-Object Id, ProcessName, Path
Stop-Process -Id <PID>
```

Разные build-каталоги друг другу не мешают. Visual Studio preset
`windows-msvc` самодостаточен. Preset `windows-clang` требует `clang-cl` и
Ninja в `PATH` и запускается из x64 VS Developer PowerShell/Command Prompt.

## Архитектурные правила

- Новая зависимость DLL объявляется в `src/<module>/CMakeLists.txt`.
- Общий клиент-серверный код входит в `laiue::headless_stack`; нельзя
  компилировать те же production `.c` отдельно в клиент и сервер.
- Win32/POSIX API разрешены только в `src/platform` и специализированных
  backend-файлах. Simulation/world/gameplay не выбирают ОС через `#ifdef`.
- Общий simulation-код не выполняет файловый, сетевой или console I/O.
- Нижние модули не включают `core`; разрешённый граф проверяет
  `tools/check_architecture.ps1`.
- Крупную DLL делите на внутренние `.c/.h`, не создавая ABI без
  самостоятельного жизненного цикла и второго потребителя.
- Все CMake options, include paths, definitions и linker flags должны быть
  target-scoped. Configure/build не пишут в source tree.
- Каждый буфер и handle имеют одного владельца; публичный API явно указывает
  release-функцию. Нельзя требовать от потребителя освобождать память через
  allocator другой DLL/SO.
- `network` принимает только versioned и bounded wire-format. Клиент не задаёт
  авторитетную позицию, инвентарь или результат правки блока.
- MsQuic callbacks только копируют данные в bounded queues. Gameplay/world
  вызываются на main thread, send-buffer живёт до `SEND_COMPLETE`, а overflow
  закрывает только виновного peer.
- Reliable control/input/snapshot и transient state — разные контракты.
  QUIC DATAGRAM разрешён только для сообщений, которые можно безопасно
  потерять, продублировать и переупорядочить. Временное состояние игрока
  сравнивается wrap-safe по `serverTick` и имеет bounded reliable fallback;
  edge-input нельзя переносить в DATAGRAM без отдельного repeat/ack протокола.
- Публичный `Network*` ABI не зависит от конкретной QUIC-библиотеки.
  Transport backend не декодирует protocol и не вызывает gameplay; новый
  backend сначала подключается к общему bounded channel/session engine, а не
  копирует client/server state machines.
- Fixed tick не содержит allocation, I/O и покадрового logging.
- Network physics исполняется только как 60 Гц canonical input и четыре
  целых substep; render `deltaSeconds` не попадает в authoritative simulation
  или replay.
- Монотонное время и fixed-step accumulator остаются `double`; сужение до
  `float` допустимо только после simulation boundary. Изменение scheduler
  проверяется как минимум при 30/60/144 FPS.
- Reconciliation сначала атомарно восстанавливает полный server state, затем
  повторяет неподтверждённые команды. Visual correction не меняет collision
  position, а freshness и remote interpolation опираются на wrap-safe
  `serverTick`; новый tick нельзя отбрасывать только из-за неизменившегося
  input acknowledgement.
- Physics-changing моды сетевой сессии исполняются server-authoritative;
  remote client не получает mutable `PlayerController` и не может применять
  `setBlock` прямо к локальной копии мира. Серверная мутация блока обязана
  увеличить authoritative revision и реплицироваться заинтересованным peers.
- Оптимизация горячего пути требует повторяемого замера до и после.

## Безопасность

- Remote transport — только QUIC/TLS 1.3 с проверкой DNS/IP identity или
  точного pin. Plaintext fallback, TOFU и release-режим `skip certificate
  validation` запрещены.
- Private keys не попадают в репозиторий, build artifacts, crash dumps и
  логи. Linux проверяет regular-file/no-symlink и права не шире `0600`.
- Gameplay, mod negotiation и snapshots не отправляются в 0-RTT.
- Любой размер, count, offset, sequence и enum из сети/файла проверяются до
  allocation и индексирования. Доверенный локальный файл не считается
  автоматически корректным.
- Нативные моды не являются sandbox: hash подтверждает совместимость и
  целостность, но не безопасность кода.
- Исправление уязвимости сопровождается негативным regression test; секреты
  и рабочие exploit payloads в fixtures не добавляются.

## Совместимость

- `sdk/laiue_mod_api.h` растёт только добавлением полей в хвост;
  потребитель проверяет `structSize`.
- Несовместимое изменение SDK требует новой версии API.
- Дисковые и сетевые форматы — little-endian, versioned и bounded.
- Новая несовместимая раскладка получает новую версию, а не тихо заменяет
  старую.
- Новые поля wire/disk/SDK добавляются так, чтобы старый reader мог либо
  безопасно пропустить их, либо явно отклонить новую версию.
- Manifest v2 hash считается по одинаковым байтам и фиксированному порядку
  артефактов на всех ОС; нельзя подменять его hash только текущего бинарника.

## Изменения, тесты и генерируемые файлы

- Исправление ошибки получает минимальный regression test, который падает до
  исправления. Изменение wire/disk/SDK layout получает golden/round-trip
  проверку.
- Изменение physics formula, input quantization/sequence, authoritative state
  или tick/interpolation policy получает determinism и
  prediction/reconciliation regression test.
- Перед отправкой запускайте наиболее узкую релевантную проверку, затем
  platform build/CTest. Не маскируйте предупреждения глобальным отключением.
- Workflow Actions закрепляются полным commit SHA, runner image — явным
  поддерживаемым label. Обновления делает Dependabot и проверяет обычная
  матрица; floating `@main`, `@vN` и `*-latest` в release workflow запрещены.
- Загружаемый в CI dependency проверяется до распаковки: version/commit,
  SHA-256 и обязательные license/notices. Ошибка должна печатать ожидаемое и
  фактическое значение, чтобы drift отличался от сетевого сбоя.
- `src/render/generated/*.h` — checked-in fallback. Обычная сборка генерирует
  shader headers только в binary tree; обновление fallback выполняется
  отдельной явной командой и проверяется diff.
- Бинарные шейдеры встроенных `.lsp` пересобираются тем же профилем
  `*_5_0`, `/O3`, `/Qstrip_debug`, `/Qstrip_reflect`.
- `tools/generate_textures.ps1` создаёт исходные PNG, а
  `tools/build_texture_pack.ps1` собирает `.ltp`.

Не форматируйте весь проект вместе с функциональным изменением. Для
затронутых C/H-файлов используйте `.clang-format`; предупреждения считаются
ошибками.

## Документация, SDK и зависимости

При изменении поведения обновляйте ближайший документ, а не добавляйте
второе описание в README. Основные контракты:

- [архитектура](docs/architecture.md)
- [переносимость](docs/portability.md)
- [мультиплеер](docs/multiplayer.md)
- [физика и сетевая репликация](docs/physics.md)
- [secure server](docs/secure_server.md)
- [форматы](docs/content_formats.md)
- [шейдеры](docs/shaderpacks.md)
- [моды](docs/modding.md)
- [сохранения](docs/world_format.md)

Перед публикацией SDK соберите пять примеров из `sdk/examples/`.

Новая или обновлённая внешняя зависимость требует:

- закреплённой поддерживаемой версии и воспроизводимого источника пакета;
- проверки security advisories и release notes;
- проверки лицензии, redistribution условий и обновления notices;
- CI-сборки на каждой затронутой ABI/libc;
- отсутствия сетевой загрузки во время обычного CMake configure.

Linux release MsQuic собирается только через
`tools/build_lean_msquic.sh`: exact source/quictls commits, системная
OpenSSL 3, XDP/logging/tools/tests/perf выключены. Его
`BUILD-METADATA` и ELF проверяются повторно при configure, install smoke и
упаковке; менять профиль без проверки runtime dependencies, размера,
hardening, license/notices и обеих libc нельзя.

Linux release archive создаётся только GNU tar/gzip с отключёнными
name/mtime в gzip header. CI дважды упаковывает один bundle и сравнивает
SHA-256; изменение упаковщика обязано сохранить эту проверку.

`CONTRIBUTING.md` — единственный канонический набор правил разработки.
Editor/agent-specific файлы должны ссылаться сюда, а не копировать правила.
