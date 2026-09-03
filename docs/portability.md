# Переносимость и сборка

## Матрица поддержки

| Платформа | Core | Graphics | ABI |
|---|---:|---:|---|
| Windows x86_64 | Tier 1 | Tier 1 | MSVC или clang-cl, no-CRT runtime |
| Windows ARM64 | clang-cl собран локально | собирается, не запускался | MSVC или clang-cl, no-CRT runtime |
| Debian x86_64 | Tier 1, Docker CI | — | glibc, GCC или Clang |
| Alpine x86_64 | проверено в Docker | — | musl, GCC |
| Alpine ARM64 | проверено в Docker | — | musl, GCC |
| Debian ARM64 | Docker-tested; native CI job | — | glibc, GCC |
| Steam Deck / SteamOS | Linux x86_64 core | — | glibc, нужен будущий Vulkan client |
| macOS arm64 | macOS 11+, native CI job, не проверено локально | — | AppleClang, native slice |
| macOS x86_64 | macOS 11+, native CI job, не проверено локально | — | AppleClang, native slice |
| Android ARM64 | NDK r29: собрано и слинковано локально, CI настроен | — | API 28+, static external core |
| iOS/iPadOS ARM64 | Xcode 26 build/link CI настроен | — | iOS 15+, static external core |
| tvOS/visionOS | adapter contract | — | без preset и native validation |
| другие Linux x86_64 | source-compatible | — | совместимый glibc/musl toolchain |
| Xbox / PlayStation / Nintendo | external seam | не заявлен | закрытый SDK и dev/test hardware |
| WebAssembly/WebGPU | не заявлен | не заявлен | нужен отдельный web adapter |

Core включает `platform_support`, `world`, `physics`, `content` и `mod`. Graphics
добавляет `window`, `input`, `audio`, `mesh`, `render`, `scene` и `ui`.
Linux- и macOS-графические backends пока отсутствуют; CI на этих системах
подтверждает core/headless, но не окно, presentation, input, audio или
пригодность игрового bundle. Попытка запросить отсутствующий backend должна
завершаться понятной ошибкой configure.

## Опции CMake

| Опция | Default | Назначение |
|---|---:|---|
| `LAIUE_PLATFORM_BACKEND` | `AUTO` | встроенный `WINDOWS`/`POSIX` либо явно подключённый `EXTERNAL` core adapter |
| `LAIUE_EXTERNAL_PLATFORM_FILE` | пусто | CMake-файл superbuild, создающий platform adapter и strict-FP targets |
| `LAIUE_MODULE_LIBRARY_TYPE` | desktop: `SHARED`, external: `STATIC` | тип внутренних модулей; первый external-контракт допускает только static |
| `LAIUE_NATIVE_MOD_MODE` | desktop: `DYNAMIC`, external: `OFF` | политика загружаемого native-кода; data/content packs этим не запрещаются |
| `LAIUE_ENABLE_SDK_INSTALL` | desktop: `ON`, external: `OFF` | install/export SDK; внешний порт линкуется из родительского superbuild |
| `LAIUE_BUILD_GRAPHICS` | Windows: `ON`, Linux/macOS: `OFF` | доступный платформенный графический набор |
| `LAIUE_WARNINGS_AS_ERRORS` | `ON` | считать предупреждения ошибками |
| `LAIUE_ENABLE_LTO` | `ON` | link-time optimization в Release |
| `LAIUE_CLANG_LTO_MODE` | `full` | режим clang-cl: `thin` либо `full` |
| `LAIUE_AGGRESSIVE_INLINING` | `ON` | MSVC `/Ob3`; отключаемый максимальный inline profile |
| `LAIUE_X86_64_LEVEL` | x86_64: `avx2` | ISA Release x86_64: `sse2`, `avx2` либо экспериментальный `avx512`; к ARM64 не применяется |
| `LAIUE_X86_64_TUNE` | x86_64: `generic` | планирование x86_64: `generic` либо opt-in `amd_zen4`; к ARM64 не применяется |
| `LAIUE_ENABLE_SANITIZERS` | `OFF` | ASan и UBSan в поддерживаемом Linux toolchain |
| `LAIUE_LINUX_LIBC` | `gnu` | `gnu` либо `musl` для ABI-меток |
| `LAIUE_EXPECTED_ARCHITECTURE` | `auto` | fail-fast проверка `x86_64`, `arm64` или macOS `universal2` |
| `BUILD_TESTING` | `ON` | зарегистрировать CTest targets |

Все определения, include paths и linker flags target-scoped. CMake не
скачивает зависимости и не записывает generated-файлы в source tree.

Стандартный Release preset является speed-first профилем и требует AVX2 у
MSVC либо полного x86-64-v3 у Clang/GCC:
MSVC использует `/O2 /Ot /Oi /GF /Gy /Gw /volatile:iso /Ob3 /GL`, полный
`/LTCG` и десять проходов ICF; clang-cl — compile `-O3`, `/Qvec`, loop/SLP
vectorization, `-fno-math-errno`, Full LTO и LLD LTO/codegen level 3. Linux
использует `-O3`, full LTO, section GC и прямое связывание внутренних вызовов
shared libraries. Специальный no-CRT runtime object намеренно остаётся без LTO,
чтобы изолировать linker helpers; он получает тот же ISA baseline и у clang-cl
также компилируется с `-O3`.
Строгая математика physics не ослабляется. Для старого x86_64 CPU
можно сконфигурировать `-DLAIUE_X86_64_LEVEL=sse2`; это отдельный artifact и
его нельзя смешивать с AVX2 bundle. `amd_zen4` оставлен opt-in: на текущем
координатном workload vendor tuning оказался медленнее generic AVX2.
Apple Silicon использует ISA baseline AppleClang для выбранного arm64 slice и
никогда не наследует x86-флаги. x86_64 и arm64 проходят детерминизм отдельно.
Linux ARM64 в Docker дал тот же reference hash, включая сценарий rebasing;
macOS ARM64 должен подтвердить его собственным нативным CI-запуском.

## Presets

Каждый toolchain использует отдельный configure tree, а Debug и Release
выбираются build/test preset без повторного configure.

| Configure preset | Generator | Build/test presets |
|---|---|---|
| `windows-msvc` | Visual Studio | `windows-msvc-debug`, `windows-msvc-release` |
| `windows-clang` | Ninja Multi-Config | `windows-clang-debug`, `windows-clang-release` |
| `windows-msvc-arm64` | Visual Studio ARM64 | `windows-msvc-arm64-debug`, `windows-msvc-arm64-release` |
| `windows-clang-arm64` | Ninja Multi-Config | `windows-clang-arm64-debug`, `windows-clang-arm64-release` |
| `linux-gcc` | Ninja Multi-Config | `linux-gcc-debug`, `linux-gcc-release` |
| `linux-clang` | Ninja Multi-Config | `linux-clang-debug`, `linux-clang-release` |
| `android-arm64-core` | Ninja + NDK r29 | `android-arm64-core-release` (build-only) |
| `ios-arm64-core` | Xcode 26 | `ios-arm64-core-release` (build/link-only) |
| `linux-gcc-arm64` | Ninja Multi-Config | `linux-gcc-arm64-debug`, `linux-gcc-arm64-release` |
| `linux-clang-arm64` | Ninja Multi-Config | `linux-clang-arm64-debug`, `linux-clang-arm64-release` |
| `linux-musl` | Ninja Multi-Config | `linux-musl-debug`, `linux-musl-release` |
| `linux-gcc-asan` | Ninja Multi-Config | `linux-gcc-asan-debug` |
| `linux-external-port-smoke` | Ninja Multi-Config | `linux-external-port-smoke-release` |
| `macos-clang-arm64` | Ninja Multi-Config | `macos-clang-arm64-debug`, `macos-clang-arm64-release` |
| `macos-clang-x86_64` | Ninja Multi-Config | `macos-clang-x86_64-debug`, `macos-clang-x86_64-release` |

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

## Windows on ARM

`windows-msvc-arm64` и `windows-clang-arm64` собирают тот же полный набор,
включая D3D12-клиент: Direct3D 12 доступен на ARM64, а архитектурно-зависимые
места имеют собственные реализации. Маска колонны в `mesh` использует NEON
вместо SSE2, `scene` берёт аппаратный `fsqrt`, а no-CRT runtime заменяет
`rep stosb`/`rep movsb` явными циклами: у ARM64 нет строковых инструкций.
Обе реализации маски проверены против скалярного эталона на своей
архитектуре, а loop-based `memset`/`memcpy`/`memcmp`/`memmove` — запуском на
реальном aarch64.

Запускайте из ARM64 VS Developer PowerShell:

```powershell
cmake --preset windows-clang-arm64
cmake --build --preset windows-clang-arm64-release --parallel
ctest --preset windows-clang-arm64-release
```

Кросс-сборка с x64 host требует компонента «MSVC v143 — VS 2022 C++ ARM64
build tools»; без его CRT libraries линкуются только no-CRT цели движка.
Локально проверены configure, компиляция и линковка всех ARM64 DLL и тестов;
запуск на ARM64-устройстве выполняет отдельный native CI job.

## Linux core

Нужны CMake 3.28+, Ninja, GCC или Clang и pthreads. SHA-256 реализован внутри
`platform_support`, поэтому внешняя crypto-библиотека для core не требуется.

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
повторно использовать с другим compiler, architecture или libc. musl-профиль
фактически прошёл полный CTest в Alpine-контейнерах x86_64 и aarch64, и оба
дали тот же эталонный хеш физики, что и glibc.

Linux ARM64 фактически проверен отдельной Docker-сборкой и полным CTest.
GitHub Actions использует нативный `ubuntu-24.04-arm` runner и отдельное
дерево `build/linux-gcc-arm64`; x86_64 presets для этой цели не используются.

## macOS core

Нужны CMake 3.28+, Ninja и AppleClang. Каждый configure tree содержит ровно
один native slice:

```sh
cmake --preset macos-clang-arm64
cmake --build --preset macos-clang-arm64-release --parallel
ctest --preset macos-clang-arm64-release --no-tests=error
```

На Intel host/runner используется `macos-clang-x86_64`. Workflow назначает
оба варианта нативным GitHub-hosted runners; локально в текущей Windows-среде
они не исполнялись. Universal package допустим только как отдельный packaging
шаг после успешных тестов обоих slices и всех их зависимостей; он сам по себе
не заменяет два тестовых запуска.

## Установка и bundle

Установочный компонент `Engine` содержит созданные библиотеки, публичные
заголовки и документацию. Windows graphics bundle дополнительно включает
HLSL-исходники renderer contracts; Linux core bundle их не устанавливает.

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
  reparse traversal и ASCII case-collision.
- ELF и Mach-O symbols по умолчанию hidden; наружу выходят только API exports.
- Windows no-CRT target не должен получать скрытую зависимость от CRT через
  новую библиотеку или compiler helper.
- Кадр стека функции в no-CRT сборке остаётся меньше страницы: за границей
  4 КиБ MSVC вставляет вызов `__chkstk`, которого без CRT нет. Крупные
  структуры передаются по указателю, копируются явным `memcpy` и живут в
  куче: на ARM64 при `/Od` MSVC заводит временную копию на каждое
  присваивание структуры целиком, поэтому два присваивания уже переполняют
  страницу, хотя на x86_64 тот же код обходится десятками байт.
- ARM64 под MSVC линкует `arm64rt.lib` из Windows SDK. `winnt.h` просит
  развернуть `_Interlocked*` только в ветках `_M_AMD64` и `_M_IX86`, поэтому
  на ARM64 при `/Od` компилятор оставляет вызовы помощников; ни `/Oi`, ни
  собственная `#pragma intrinsic` этого не меняют, а нужные члены
  `arm64rt.lib` не определяют `memset`, `memcpy` и прочий CRT и линкуются
  без импортов. clang-cl разворачивает интринсики сам.
- Авторитетная физика собирается без fast-math и FMA contraction.

Native mod artifact обязан точно совпадать с OS, CPU architecture и libc/ABI
host-процесса. Наличие core-сборки само по себе не обещает совместимость
старого native-мода. ARM64 получает собственный проверяемый контракт и не
наследует x86_64 битовый hash. Подробнее: [modding.md](modding.md) и
[physics.md](physics.md).

## Mobile и закрытые SDK

Android и Apple mobile family используют публичный
`cmake/platform/MobileCoreAdapter.cmake`, static core и
`LAIUE_NATIVE_MOD_MODE=OFF`. Android preset фиксирует NDK r29, ARM64 и API 28;
iOS preset фиксирует iOS 15, ARM64 и отключённую подпись build-only цели. CI
компилирует Android targets и линкует game `.so`; Apple runner линкует
минимальный unsigned app bundle. Android-профиль дополнительно собран
локально в контейнере с NDK r29: получившийся `libsimulation_of_sins.so` —
AArch64 ELF, экспортирует свою точку входа, а среди неопределённых символов у
него только Android libc. Debug/no-LTO цели whole-archive включают
каждый object game core и модулей `world`, `physics`, `content`, `mod` без
section GC, чтобы статический архив или optimizer не мог скрыть unresolved
symbol. Отдельная Release-цель проверяет финальную LTO/dead-strip линковку.
Это не запуск на телефоне и не готовый store package.

Mobile shell получает resource и writable application directories от ОС,
при необходимости копирует data-only packs в app container и передаёт явный
root в `LaiueContentCatalogCreate`. Mobile adapter намеренно не подменяет его
путём к process executable. Полноценному клиенту ещё нужны Vulkan/Metal
renderer, platform window/lifecycle, input, audio, suspend, thermal/memory
policy и device tests.

Публичный CMake-проект предоставляет compile-tested точку подключения
platform-agnostic core через `LAIUE_PLATFORM_BACKEND=EXTERNAL`. Указанный
`LAIUE_EXTERNAL_PLATFORM_FILE` обязан создать target из
`LAIUE_PLATFORM_ADAPTER_TARGET`, реализующий весь внутренний контракт
`src/platform/system.h`, и interface target из `LAIUE_PRECISE_FP_TARGET` с
точными FP-флагами закрытого компилятора. В этом режиме graphics выключен,
модули статические, динамический native-код модов выключен, а SDK export
принадлежит родительскому superbuild.

Публичный `linux-external-port-smoke` проверяет именно эту границу, static
link и core-тесты. Он использует тестовый Linux adapter и не является
эмуляцией консоли. Реальный toolchain file, backend, SDK,
заголовки, библиотеки, packaging/signing и команды dev/test hardware живут в
закрытом integration repository зарегистрированного разработчика.

Публичный GitHub Actions workflow не запускает консольный job и не содержит
NDA-названий API. После получения доступа отдельный защищённый workflow берёт
только одобренный commit `main`, работает на выделенном self-hosted runner и
не выполняет код из pull requests. До нативной сборки и запуска на hardware
статус каждой из Xbox, PlayStation и Nintendo остаётся «не заявлен», а не
«supported». Nintendo Switch 2 отдельно нельзя считать целью, пока Nintendo
публично не принимает заявки на доступ к её development environment.

Публичные основания для этой границы: [Android NDK r29](https://developer.android.com/ndk/downloads/),
[Apple App Review 2.5.2](https://developer.apple.com/app-store/review/guidelines/),
[Xbox XR-018](https://learn.microsoft.com/en-us/gaming/gdk/docs/store/policies/xr/xr018),
[Nintendo Developer registration](https://developer.nintendo.com/register) и
[PlayStation partner process](https://sonyinteractive.com/en/news/blog/showing-your-game-to-playstation/).
Закрытая partner documentation всегда имеет приоритет для конкретного порта.

Новая platform-specific операция сначала получает единый контракт владения
и ошибок, затем отдельные реализации для поддерживаемых платформ. Нельзя
размещать Win32/POSIX ветвление внутри `world` или `physics`.

## Проверка изменений

Для затронутого toolchain выполняются configure, build и CTest. Изменение
Windows no-CRT boundary дополнительно проверяется по imports готовых DLL;
изменение Linux boundary — реальной Docker-сборкой, изменение Darwin boundary
— нативными arm64 и x86_64 runs. Mock/external adapter не заменяет закрытую
консольную сборку и hardware test.

```powershell
pwsh -NoProfile -File tools/check_architecture.ps1
git diff --check
```

Полный набор правил разработки находится в
[CONTRIBUTING.md](../CONTRIBUTING.md).
