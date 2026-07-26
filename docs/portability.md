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

CMake не скачивает MsQuic. Release/CI используют MsQuic 2.5.9 из системного
пакета или проверенного `LAIUE_MSQUIC_ROOT` и включают
`LAIUE_REQUIRE_MSQUIC=ON`. Другая обнаруженная версия отклоняется. Если
package metadata недоступна, `LAIUE_MSQUIC_VERSION=2.5.9` задаётся только
после внешней проверки package hash/commit.

## Debian 13

Нужны CMake 3.28+, Ninja, GCC или Clang, pthread/dl и OpenSSL 3 development
files. Официальный Debian-пакет `libmsquic=2.5.9` является runtime-only:
в нём нет headers, pkg-config metadata и unversioned linker symlink.
Поэтому release build передаёт `LAIUE_MSQUIC_ROOT`, собранный из:

- `libmsquic.so.2.5.9` официального Microsoft-пакета;
- symlink chain `libmsquic.so -> libmsquic.so.2 -> libmsquic.so.2.5.9`;
- headers, `LICENSE` и `THIRD-PARTY-NOTICES` из закреплённого upstream commit,
  соответствующего 2.5.9;
- файла `VERSION` со значением `2.5.9`.

Обычный CMake configure ничего из сети не загружает. Воспроизводимая
bootstrap-последовательность находится в Debian job
`.github/workflows/build.yml`; prefix затем передаётся через
`LAIUE_MSQUIC_ROOT`.

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release --target laiue_server_bundle
ctest --preset linux-gcc-release
```

Для диагностической сборки:

```sh
cmake --preset linux-gcc-asan
cmake --build --preset linux-gcc-asan
ctest --preset linux-gcc-asan
```

Готовы presets `linux-gcc-debug`, `linux-gcc-release`,
`linux-clang-release` и `linux-gcc-asan`. Для release с обязательным secure
transport добавьте `-DLAIUE_REQUIRE_MSQUIC=ON` либо задайте эту cache option
в CI.

## Alpine/musl

Используйте native Alpine builder либо musl toolchain, а также совместимую
musl-сборку MsQuic/OpenSSL:

```sh
cmake --preset linux-musl-release \
  -DLAIUE_MSQUIC_ROOT=/opt/msquic-musl \
  -DLAIUE_REQUIRE_MSQUIC=ON
cmake --build --preset linux-musl-release --target laiue_server_bundle
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
- `laiue_distribution` — все включённые компоненты и SDK examples.

Bundle-цели очищают и заполняют независимые staging-каталоги
`build/<preset>/bundles/{client,server,mod-sdk}/<config>`. Install-компоненты
`Client`, `Server` и `ModSDK` можно устанавливать отдельно:

```sh
cmake --install build/linux-gcc-release --prefix staging/server \
  --component Server
cpack --config build/linux-gcc-release/CPackConfig.cmake
```

Windows package — ZIP, Linux package — TGZ. Shared libraries лежат рядом с
исполняемым файлом; ELF RUNPATH равен `$ORIGIN`. Не полагайтесь на
`LD_LIBRARY_PATH` в release package. `tools/smoke_linux_server.sh` проверяет
RUNPATH/runtime dependencies и запускает IPv4, IPv6 и dual listeners с
временным сертификатом (IPv6 пропускается только когда его отключил runner).

glibc bundle содержит выбранную `libmsquic.so` symlink chain, но не копирует
системные библиотеки Debian. На чистом Debian 13 до запуска установите
runtime packages `libssl3t64`, `libnuma1`, `libxdp1` и
`libnl-route-3-200`; apt подтянет их транзитивные зависимости. CI повторяет
package smoke в чистом `debian:13-slim` без установленного `libmsquic`
package, чтобы доказать использование runtime из bundle.

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
