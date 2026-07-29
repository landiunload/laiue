# Переносимость и сборка

## Матрица поддержки

| Платформа | Клиент | Dedicated server | ABI |
|---|---:|---:|---|
| Windows x86_64 | Tier 1 | Tier 1 | MSVC/clang-cl, без CRT |
| Debian 13 x86_64 | — | Tier 1 | glibc, GCC/Clang |
| Alpine x86_64 | — | проверяемый артефакт | musl |
| другие современные Linux x86_64 | — | source-compatible | glibc/musl соответствующего артефакта |
| ARM64 | — | пока нет | физика ещё закреплена за SSE2 |

Linux client, renderer, window, input и audio в текущую итерацию не входят.
Сервер использует libc/POSIX; Windows сохраняет отдельный no-CRT контракт.

## Компоненты CMake

- `laiue::build_options` — общие target-scoped warnings, версии и FP policy;
- `laiue::windows_no_crt` — только Windows entry/link/runtime contract;
- `laiue::platform_support` — allocator, locks, files, time, crypto, signals и
  dynamic libraries;
- `laiue::headless_stack` — world, physics, gameplay, interaction, content,
  network и mod.

Клиент и сервер линкуют один `laiue::headless_stack`. Server-only configure
не создаёт и не подтягивает window/input/audio/mesh/render/core.

Основные опции:

| Опция | Default | Назначение |
|---|---:|---|
| `LAIUE_BUILD_CLIENT` | Windows: ON, Linux: OFF | Windows-клиент |
| `LAIUE_BUILD_SERVER` | ON | dedicated server |
| `LAIUE_BUILD_EXAMPLE_MODS` | ON | native SDK examples |
| `LAIUE_WARNINGS_AS_ERRORS` | ON | warnings как ошибки |
| `LAIUE_ENABLE_LTO` | ON | LTO в Release |
| `LAIUE_ENABLE_SANITIZERS` | OFF | ASan+UBSan на Linux |
| `LAIUE_LINUX_LIBC` | `gnu` | `gnu` либо `musl` |
| `LAIUE_ENABLE_MSQUIC` | ON | искать secure transport |
| `LAIUE_REQUIRE_MSQUIC` | OFF | fail configure без MsQuic |
| `LAIUE_MSQUIC_ROOT` | пусто | prefix с `include/`, `lib/`, runtime |
| `LAIUE_MSQUIC_VERSION` | auto | подтверждённая версия при отсутствии metadata |

CMake не скачивает MsQuic. Release/CI используют проверенный
`LAIUE_MSQUIC_ROOT` и включают `LAIUE_REQUIRE_MSQUIC=ON`. Другая
обнаруженная версия отклоняется. Если package metadata недоступна,
`LAIUE_MSQUIC_VERSION=2.5.9` задаётся только после внешней проверки package
hash/commit.

Linux release собирает закреплённый lean-профиль MsQuic: без XDP, logging,
tools, tests и perf, с системной `libcrypto.so.3`. Prefix содержит точные
upstream commits, SHA-256 runtime, licenses и параметры сборки в
`BUILD-METADATA`; CMake и package smoke перепроверяют metadata и ELF.
Обычный system prefix остаётся допустим для разработки, но не получает
статус проверенного lean-артефакта.

## Presets

Каждый toolchain конфигурируется один раз в `build/<configure>`:

| Configure preset | Generator | Build/test presets |
|---|---|---|
| `windows-msvc` | Visual Studio | `windows-msvc-debug`, `windows-msvc-release` |
| `windows-clang` | Ninja Multi-Config | `windows-clang-debug`, `windows-clang-release` |
| `windows-msvc-server` | Visual Studio, server-only | `windows-msvc-server-debug`, `windows-msvc-server-release` |
| `linux-gcc` | Ninja Multi-Config | `linux-gcc-debug`, `linux-gcc-release` |
| `linux-clang` | Ninja Multi-Config | `linux-clang-debug`, `linux-clang-release` |
| `linux-musl` | Ninja Multi-Config | `linux-musl-debug`, `linux-musl-release` |
| `linux-gcc-asan` | Ninja Multi-Config | `linux-gcc-asan-debug` |

Все эти configure-presets требуют MsQuic 2.5.9. Перед configure задайте
`LAIUE_MSQUIC_ROOT` в окружении либо передайте
`-DLAIUE_MSQUIC_ROOT=<prefix>`.

`windows-msvc` сам находит установленный Visual Studio и не требует
Developer shell. Для `windows-clang` нужны `clang-cl` и Ninja в `PATH`;
запускайте его из x64 VS Developer PowerShell/Command Prompt, чтобы были
доступны Windows SDK и x64 libraries.

## Debian 13

Нужны CMake 3.28+, Ninja, GCC или Clang, Git, binutils, Perl и OpenSSL 3
development files. Официальный Microsoft runtime содержит ненужные серверу
XDP/BPF/NUMA зависимости, поэтому release/CI собирают меньший профиль из
закреплённых исходников. Получение исходников выполняется явно, вне CMake:

```sh
git clone --filter=blob:none --no-checkout \
  https://github.com/microsoft/msquic.git /tmp/msquic
git -C /tmp/msquic fetch --depth 1 origin \
  87b53085d76bd7920d490a6f226c9999b6614d14
git -C /tmp/msquic checkout --detach \
  87b53085d76bd7920d490a6f226c9999b6614d14
git -C /tmp/msquic submodule update --init --depth 1 \
  submodules/quictls
LAIUE_MSQUIC_LIBC=gnu sh tools/build_lean_msquic.sh \
  /tmp/msquic /tmp/msquic-build /tmp/msquic-install
```

Build directory и prefix должны быть пустыми и находиться вне source tree.
Скрипт запрещает сетевые загрузки во время configure, проверяет оба commit,
чистоту checkout, ABI, SONAME, hardening, exports, размер и runtime
dependencies. Результат передаётся проекту через
`LAIUE_MSQUIC_ROOT=/tmp/msquic-install`.

```sh
export LAIUE_MSQUIC_ROOT=/opt/msquic-2.5.9
cmake --preset linux-gcc
cmake --build --preset linux-gcc-release --target laiue_server_archive
ctest --preset linux-gcc-release
```

Для диагностической сборки:

```sh
cmake --preset linux-gcc-asan
cmake --build --preset linux-gcc-asan-debug
ctest --preset linux-gcc-asan-debug
```

GCC и Clang используют отдельные configure trees, но Debug и Release внутри
каждого дерева выбираются без повторного configure.

## Alpine/musl

Используйте native Alpine builder либо musl toolchain. Для release archive
установите GNU `tar` и `gzip` (BusyBox gzip не поддерживает `-n`). Тот же
`tools/build_lean_msquic.sh` запускается внутри Alpine с
`LAIUE_MSQUIC_LIBC=musl`; один prefix нельзя переносить между glibc и musl:

```sh
cmake --preset linux-musl \
  -DLAIUE_MSQUIC_ROOT=/opt/msquic-musl
cmake --build --preset linux-musl-release --target laiue_server_archive
ctest --preset linux-musl-release
```

glibc и musl server/mod binaries не взаимозаменяемы. Release names и manifest
keys всегда содержат ABI: `linux-x86_64-gnu` или `linux-x86_64-musl`.
Чистому Alpine 3.23 runtime нужны пакеты `libcrypto3`, `libssl3` и
`libgcc`; `openssl` требуется оператору для выпуска/диагностики
сертификата, но не самому серверному процессу. CI запускает musl bundle
в отдельном чистом Alpine container, без установленной system
`libmsquic`.

## Артефакты

- `laiue_client_bundle` — Windows client и runtime assets;
- `laiue_server_bundle` — сервер и общий headless runtime;
- `laiue_server_archive` — Linux tar.gz и соседний файл SHA-256;
- `laiue_distribution` — все включённые компоненты и SDK examples.

Bundle-цели очищают и заполняют независимые staging-каталоги
`build/<configure>/bundles/{client,server,mod-sdk}/<config>`.
Install-компоненты `Client`, `Server` и `ModSDK` можно устанавливать
отдельно. Для multi-config дерева `--config` обязателен; CPack получает ту
же конфигурацию через `-C`:

```sh
cmake --install build/linux-gcc --config Release \
  --prefix staging/server --component Server
cpack --config build/linux-gcc/CPackConfig.cmake -C Release
```

CPack складывает архивы в `build/<configure>/packages`, не в source tree.
Windows package — ZIP, Linux package — TGZ. Shared libraries лежат рядом с
исполняемым файлом; ELF RUNPATH равен `$ORIGIN`. Не полагайтесь на
`LD_LIBRARY_PATH` в release package. `tools/smoke_linux_server.sh` проверяет
RUNPATH/runtime dependencies и запускает IPv4, IPv6 и dual listeners с
временным сертификатом (IPv6 пропускается только когда его отключил runner).
`laiue_server_archive` дополнительно нормализует modes через Linux-native
временный каталог: каталоги и исполняемые файлы получают `0755`, обычные
файлы — `0644`. Поэтому archive безопасен даже при build tree на drvfs или
другой файловой системе без Unix permission metadata.

glibc bundle содержит проверенную `libmsquic.so` symlink chain, но не
копирует системную OpenSSL. На чистом Debian 13 нужен runtime package
`libssl3t64`; XDP, BPF, NUMA и netlink packages lean-профилю не нужны. CI
повторяет package smoke в чистом `debian:13-slim` без system `libmsquic`,
чтобы доказать использование runtime из bundle и отсутствие скрытых
зависимостей.

`tools/check_lean_msquic.sh` можно запускать отдельно для аудита
`libmsquic.so.2.5.9`; `tools/package_linux_server.sh` повторяет этот аудит
перед созданием архива. Release archive создаётся GNU tar с
детерминированными order/mode/mtime и соседним SHA-256.

## Переносимый код

- Внутренние path strings канонизируются как UTF-8 с `/`; platform boundary
  преобразует их в native representation.
- Raw `wchar_t`, указатели и native structs не записываются в wire/disk.
- Имена внутри packs запрещают absolute path, `..`, разделители, symlink/
  reparse traversal и case-collision; ограничения Windows сохраняются на
  Linux, чтобы один pack был переносим.
- Export macros определены в `src/api.h`; SDK модов использует
  `LAIUE_MOD_EXPORT` и `LAIUE_MOD_CALL`.
- ELF symbols по умолчанию hidden; публичными становятся только API exports.
- Детерминированные simulation sources собираются без fast-math/FMA
  contraction на каждой платформе.

Новая platform-specific функция сначала добавляется в `src/platform/system.h`
с одинаковым контрактом владения и ошибок, затем реализуется в Win32 и POSIX
backend. Прямые OS API в world/gameplay/protocol запрещены правилами
[CONTRIBUTING.md](../CONTRIBUTING.md).
