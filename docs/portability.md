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

CMake не скачивает MsQuic. Release/CI используют MsQuic 2.5.9 из системного
пакета или проверенного `LAIUE_MSQUIC_ROOT` и включают
`LAIUE_REQUIRE_MSQUIC=ON`.

## Debian 13

Нужны CMake 3.28+, Ninja, GCC или Clang, pthread/dl, OpenSSL 3 development
files и MsQuic 2.5.9 development/runtime package. `libmsquic` устанавливается
из официального Microsoft repository для Debian либо передаётся через
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

## Артефакты

- `laiue_client_bundle` — Windows client и runtime assets;
- `laiue_server_bundle` — сервер и общий headless runtime;
- `laiue_distribution` — все включённые компоненты и SDK examples.

Установка и пакет:

```sh
cmake --install build/linux-gcc-release --prefix staging/laiue
cpack --config build/linux-gcc-release/CPackConfig.cmake
```

Windows package — ZIP, Linux package — TGZ. Shared libraries лежат рядом с
исполняемым файлом; ELF RUNPATH равен `$ORIGIN`. Не полагайтесь на
`LD_LIBRARY_PATH` в release package.

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
