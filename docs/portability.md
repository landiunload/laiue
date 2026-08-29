# Переносимость и сборка

## Матрица поддержки

| Платформа | Core | Graphics | ABI |
|---|---:|---:|---|
| Windows x86_64 | Tier 1 | Tier 1 | MSVC или clang-cl, no-CRT runtime |
| Debian x86_64 | Tier 1 | — | glibc, GCC или Clang |
| Alpine x86_64 | CI | — | musl, GCC |
| другие Linux x86_64 | source-compatible | — | совместимый glibc/musl toolchain |

Core включает `platform_support`, `world`, `physics` и `content`. Graphics
добавляет `window`, `input`, `audio`, `mesh`, `render`, `scene` и `ui`.
Linux-графический backend пока отсутствует; попытка включить его завершается
ошибкой configure.

## Опции CMake

| Опция | Default | Назначение |
|---|---:|---|
| `LAIUE_BUILD_GRAPHICS` | Windows: `ON`, Linux: `OFF` | графические библиотеки Windows |
| `LAIUE_WARNINGS_AS_ERRORS` | `ON` | считать предупреждения ошибками |
| `LAIUE_ENABLE_LTO` | `ON` | link-time optimization в Release |
| `LAIUE_ENABLE_SANITIZERS` | `OFF` | ASan и UBSan в поддерживаемом Linux toolchain |
| `LAIUE_LINUX_LIBC` | `gnu` | `gnu` либо `musl` для ABI-меток |
| `BUILD_TESTING` | `ON` | зарегистрировать CTest targets |

Все определения, include paths и linker flags target-scoped. CMake не
скачивает зависимости и не записывает generated-файлы в source tree.

## Presets

Каждый toolchain использует отдельный configure tree, а Debug и Release
выбираются build/test preset без повторного configure.

| Configure preset | Generator | Build/test presets |
|---|---|---|
| `windows-msvc` | Visual Studio | `windows-msvc-debug`, `windows-msvc-release` |
| `windows-clang` | Ninja Multi-Config | `windows-clang-debug`, `windows-clang-release` |
| `linux-gcc` | Ninja Multi-Config | `linux-gcc-debug`, `linux-gcc-release` |
| `linux-clang` | Ninja Multi-Config | `linux-clang-debug`, `linux-clang-release` |
| `linux-musl` | Ninja Multi-Config | `linux-musl-debug`, `linux-musl-release` |
| `linux-gcc-asan` | Ninja Multi-Config | `linux-gcc-asan-debug` |

Пример Windows/MSVC:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug

cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release
```

`windows-msvc` сам находит установленный Visual Studio. Для
`windows-clang` нужны `clang-cl` и Ninja в `PATH`; запускайте его из x64 VS
Developer PowerShell/Command Prompt, чтобы были доступны Windows SDK и x64
libraries.

## Linux core

Нужны CMake 3.28+, Ninja, GCC или Clang, pthreads и development package
OpenSSL 3 для платформенных crypto-примитивов.

```sh
cmake --preset linux-gcc
cmake --build --preset linux-gcc-release --parallel
ctest --preset linux-gcc-release
```

Диагностическая сборка:

```sh
cmake --preset linux-gcc-asan
cmake --build --preset linux-gcc-asan-debug --parallel
ctest --preset linux-gcc-asan-debug
```

Для musl используйте native Alpine builder либо корректный musl toolchain:

```sh
cmake --preset linux-musl
cmake --build --preset linux-musl-release --parallel
ctest --preset linux-musl-release
```

glibc и musl libraries не взаимозаменяемы. Один configure tree нельзя
повторно использовать с другим compiler, architecture или libc.

## Установка и bundle

Установочный компонент `Engine` содержит созданные библиотеки, публичные
заголовки, шейдерные исходники/fallback и документацию.

```powershell
cmake --install build/windows-msvc --config Release `
  --prefix .\staging\engine --component Engine
```

```sh
cmake --install build/linux-gcc --config Release \
  --prefix staging/engine --component Engine
```

Цель `laiue_engine_bundle` очищает и заполняет
`build/<configure>/bundles/engine/<Configuration>`. Для multi-config дерева
`--config` обязателен. CPack складывает архивы в
`build/<configure>/packages`, не в source tree.

`laiue::engine` является удобной aggregate-целью при использовании
`add_subdirectory`. Установленный потребитель может линковать только нужные
библиотеки, если не требуется полный платформенный набор.

## Платформенный код

- Native API изолируется в `src/platform` и специализированных backend-файлах.
- Внутренние пути канонизируются как UTF-8 с `/`; platform boundary
  преобразует их в native representation.
- Указатели, `wchar_t` и native structs не записываются в переносимые файлы.
- Имена внутри packs отклоняют absolute path, `..`, separators, symlink или
  reparse traversal и case-collision.
- ELF symbols по умолчанию hidden; наружу выходят только API exports.
- Windows no-CRT target не должен получать скрытую зависимость от CRT через
  новую библиотеку или compiler helper.
- Переносимые вычисления собираются без fast-math и FMA contraction.

Новая platform-specific операция сначала получает единый контракт владения
и ошибок, затем отдельные реализации для поддерживаемых платформ. Нельзя
размещать Win32/POSIX ветвление внутри `world` или `physics`.

## Проверка изменений

Для затронутого toolchain выполняются configure, build и CTest. Изменение
Windows no-CRT boundary дополнительно проверяется по imports готовых DLL;
изменение Linux boundary — реальной Linux-сборкой, а не только syntax check.

```powershell
pwsh -NoProfile -File tools/check_architecture.ps1
git diff --check
```

Полный набор правил разработки находится в
[CONTRIBUTING.md](../CONTRIBUTING.md).
