# Переносимость и сборка

## Матрица поддержки

| Платформа | Core | Graphics | ABI |
|---|---:|---:|---|
| Windows x86_64 | Tier 1 | Tier 1: D3D12; при Vulkan SDK ещё и Vulkan (Win32 swapchain) | MSVC или clang-cl, no-CRT runtime |
| Windows ARM64 | native CI job, CTest Debug/Release | собирается и проходит CTest в CI на ARM64-раннере | MSVC или clang-cl, no-CRT runtime |
| Ubuntu/Debian x86_64 | Tier 1, native CI на Ubuntu 26.04 | Vulkan offscreen, CI на lavapipe | glibc, GCC или Clang |
| Alpine x86_64 | проверено в Docker | — | musl, GCC |
| Alpine ARM64 | проверено в Docker | — | musl, GCC |
| Ubuntu/Debian ARM64 | native CI job на Ubuntu 26.04 | — | glibc, GCC |
| Steam Deck / SteamOS | Linux x86_64 core | Vulkan offscreen и ALSA, на устройстве не запускался | glibc, нужны окно, ввод и UI |
| macOS arm64 | macOS 11+, native CI job, не проверено локально | — | AppleClang, native slice |
| macOS x86_64 | macOS 11+, native CI job, не проверено локально | — | AppleClang, native slice |
| Android ARM64 | NDK r30: core собирается в CI | Vulkan NativeActivity-пример и вывод звука AAudio собираются в CI; на устройстве в CI не запускаются | API 28+, static external core |
| Android x86_64 | Android 17 Google APIs AVD в CI, NativeActivity/Vulkan APK | — | NDK r30, API 28+, static external core |
| iOS/iPadOS ARM64 | Xcode 26 build/link CI настроен | — | iOS 15+, static external core |
| tvOS/visionOS | adapter contract | — | без preset и native validation |
| другие Linux x86_64 | source-compatible | — | совместимый glibc/musl toolchain |
| Xbox / PlayStation / Nintendo | external seam | не заявлен | закрытый SDK и dev/test hardware |
| WebAssembly/WebGPU | не заявлен | не заявлен | нужен отдельный web adapter |

Core включает `platform_support`, `world`, `physics`, `content` и `mod`.
Состав Graphics задаёт бэкенд рендера, а не платформа:

| Бэкенд | Графические модули | Чего нет |
|---|---|---|
| `D3D12` | `window`, `input`, `audio`, `mesh`, `render`, `scene`, `ui` | — |
| `VULKAN` на Windows | `window`, `input`, `audio`, `mesh`, `render`, `scene`, `ui` | — |
| `VULKAN` на POSIX | `audio`, `mesh`, `render`, `scene`, `ui`; X11 `window`/`input` when X11 is found | Wayland and evdev adapters |

На Windows `LAIUE_RENDER_BACKEND=AUTO` (умолчание) связывает все физически
доступные бэкенды: D3D12 и, если найден Vulkan SDK, Vulkan. `RendererCreate`
и `RENDERER_BACKEND_AUTO` при этом дают D3D12, а конкретный бэкенд выбирает
`RendererCreateWithBackend`. Явные `D3D12`/`VULKAN` линкуют ровно один
бэкенд.

Путь вывода зависит от платформы, а не от одного признака «Vulkan». На
Windows Vulkan с непустым `windowHandle` создаёт Win32 surface и swapchain
и показывает кадр в переданном HWND; те же самостоятельные `window`, `input`
и `ui` providers доступны независимо от выбранного backend-а. Vulkan без
`windowHandle`, а
также весь Vulkan вне Windows рисует кадр offscreen: результат читается
`RendererCaptureFrame` из `render/renderer_offscreen.h`. Этого достаточно,
чтобы проверять рендер по пикселям на программном драйвере; при наличии X11
тот же provider создаёт Xlib surface и input, а Wayland остаётся отдельным
adapter.

Список собираемых графических модулей печатается при configure, чтобы
неполнота профиля была видна, а не подразумевалась. Попытка запросить
отсутствующий backend завершается понятной ошибкой configure: `D3D12` вне
Windows отвергается сразу.

Микшер и сцена переносимы целиком: микшер не знает платформы, потоковый
стриминг чанков работает через контракт потоков платформенного слоя.
Системный вывод звука: Windows — WASAPI, Linux — ALSA, Android — AAudio
(обе загружаются в рантайме), macOS и iOS — выходной AudioUnit. Остальные
внешние профили собирают `audio_output` с заглушкой: запрос системного
устройства завершается понятной ошибкой, а микшер и offscreen-вывод
работают. На POSIX headless-профиле окно
и ввод можно подключить X11 provider-ом; `ui` остаётся доступным и использует
portable font fallback, а на Windows растеризует
шрифты через GDI.

## Опции CMake

| Опция | Default | Назначение |
|---|---:|---|
| `LAIUE_PLATFORM_BACKEND` | `AUTO` | встроенный `WINDOWS`/`POSIX` либо явно подключённый `EXTERNAL` core adapter |
| `LAIUE_EXTERNAL_PLATFORM_FILE` | пусто | CMake-файл superbuild, создающий platform adapter и strict-FP targets |
| `LAIUE_MODULE_LIBRARY_TYPE` | desktop: `SHARED`, external: `STATIC` | тип внутренних модулей; первый external-контракт допускает только static |
| `LAIUE_NATIVE_MOD_MODE` | desktop: `DYNAMIC`, external: `OFF` | политика загружаемого native-кода; data/content packs этим не запрещаются |
| `LAIUE_ENABLE_SDK_INSTALL` | desktop: `ON`, external: `OFF` | install/export SDK; внешний порт линкуется из родительского superbuild |
| `LAIUE_BUILD_GRAPHICS` | Windows: `ON`, Linux/macOS: `OFF` | доступный платформенный графический набор |
| `LAIUE_RENDER_BACKEND` | `AUTO` | `D3D12` (только Windows) либо переносимый `VULKAN`; `AUTO` связывает всё доступное и по умолчанию выбирает D3D12 на Windows, иначе Vulkan |
| `LAIUE_WARNINGS_AS_ERRORS` | `ON` | считать предупреждения ошибками |
| `LAIUE_ENABLE_LTO` | `ON` | link-time optimization в Release |
| `LAIUE_CLANG_LTO_MODE` | `full` | режим clang-cl: `thin` либо `full` |
| `LAIUE_AGGRESSIVE_INLINING` | `ON` | MSVC `/Ob3`; отключаемый максимальный inline profile |
| `LAIUE_X86_64_LEVEL` | x86_64: `avx2` | ISA Release x86_64: `sse2`, `avx2` либо экспериментальный `avx512`; к ARM64 не применяется |
| `LAIUE_X86_64_TUNE` | x86_64: `generic` | планирование x86_64: `generic` либо opt-in `amd_zen4`; к ARM64 не применяется |
| `LAIUE_ENABLE_SANITIZERS` | `OFF` | ASan и UBSan в поддерживаемом Linux toolchain |
| `LAIUE_LINUX_LIBC` | `gnu` | `gnu` либо `musl` для ABI-меток |
| `LAIUE_EXPECTED_ARCHITECTURE` | `auto` | fail-fast проверка `x86_64`, `arm64` или macOS `universal2` |
| `LAIUE_BUILD_TOOLS` | desktop: `ON`, external и кросс-сборка: `OFF` | офлайн-инструменты подготовки содержимого; в SDK не устанавливаются |
| `LAIUE_BUILD_BENCHMARKS` | `OFF` | ручные измерения производительности; в CTest не входят |
| `BUILD_TESTING` | `ON` | зарегистрировать CTest targets |
| `LAIUE_REQUIRE_VULKAN_TEST_DRIVER` | `OFF` | при `BUILD_TESTING=ON` считать отсутствие Vulkan-драйвера ошибкой offscreen/instance-ring тестов; включено в Linux Vulkan CI |

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
Готовый Windows ARM64 SDK требует Windows 11 и ARMv8.2. Умолчание
`LAIUE_WINDOWS_ARM64_BASELINE=armv8.2` разрешает LSE атомарные инструкции:
MSVC получает `/arch:armv8.2`, clang-cl — `-march=armv8.2-a`. Ускорение этого
профиля ещё не измерено. Для старых устройств выбирайте `armv8.0` в отдельном
дереве сборки: он сохраняет базовый набор инструкций компилятора; смешивать
его библиотеки с готовым ARMv8.2 SDK нельзя. Apple Silicon использует ISA
baseline AppleClang для выбранного arm64 slice и никогда не наследует
x86-флаги. x86_64 и arm64 проходят детерминизм отдельно.
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
| `android-arm64-core` | Ninja + NDK r30 | `android-arm64-core-release` (build-only) |
| `android-x86_64-core` | Ninja + NDK r30 | `android-x86_64-core-release` (build/link-only; AVD отдельно) |
| `ios-arm64-core` | Xcode 26 | `ios-arm64-core-release` (build/link-only) |
| `linux-gcc-arm64` | Ninja Multi-Config | `linux-gcc-arm64-debug`, `linux-gcc-arm64-release` |
| `linux-clang-arm64` | Ninja Multi-Config | `linux-clang-arm64-debug`, `linux-clang-arm64-release` |
| `linux-musl` | Ninja Multi-Config | `linux-musl-debug`, `linux-musl-release` |
| `linux-gcc-asan` | Ninja Multi-Config | `linux-gcc-asan-debug` |
| `linux-vulkan-offscreen` | Ninja Multi-Config | `linux-vulkan-offscreen-debug`, `linux-vulkan-offscreen-release` |
| `linux-vulkan-x11-walk` | Ninja Multi-Config | `linux-vulkan-x11-walk-release` |
| `linux-external-port-smoke` | Ninja Multi-Config | `linux-external-port-smoke-release` |
| `macos-clang-arm64` | Ninja Multi-Config | `macos-clang-arm64-debug`, `macos-clang-arm64-release` |
| `macos-clang-x86_64` | Ninja Multi-Config | `macos-clang-x86_64-debug`, `macos-clang-x86_64-release` |

Пример Windows/MSVC:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug --parallel

cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release --parallel
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
ctest --preset windows-clang-arm64-release --parallel
```

Совместимая с ARMv8.0 сборка для старых устройств:

```powershell
cmake --preset windows-msvc-arm64 -B build/windows-msvc-arm64-armv8.0 `
  "-DLAIUE_WINDOWS_ARM64_BASELINE=armv8.0"
cmake --build build/windows-msvc-arm64-armv8.0 --config Release --parallel
ctest --test-dir build/windows-msvc-arm64-armv8.0 -C Release --parallel --output-on-failure
```

CI собирает и запускает оба ISA-профиля MSVC на нативном ARM64-раннере;
готовый SDK публикуется только из обычного ARMv8.2 дерева. Такой запуск
проверяет профиль ARMv8.0 на ARM64, но не заменяет проверку на старом устройстве.

Кросс-сборка с x64 host требует компонента «MSVC Build Tools for ARM64»
из Visual Studio 2026; без его CRT libraries линкуются только no-CRT цели движка.
Локально проверены configure, компиляция и линковка всех ARM64 DLL и тестов;
запуск на ARM64-устройстве выполняет отдельный native CI job.

## Linux core

Нужны CMake 4.2+, Ninja, GCC или Clang и pthreads. SHA-256 реализован внутри
`platform_support`, поэтому внешняя crypto-библиотека для core не требуется.

```sh
cmake --preset linux-gcc
cmake --build --preset linux-gcc-release --parallel
ctest --preset linux-gcc-release --parallel
```

Диагностическая сборка:

```sh
cmake --preset linux-gcc-asan
cmake --build --preset linux-gcc-asan-debug --parallel
ctest --preset linux-gcc-asan-debug --parallel
```

Для musl используйте native Alpine builder либо корректный musl toolchain:

```sh
cmake --preset linux-musl
cmake --build --preset linux-musl-release --parallel
ctest --preset linux-musl-release --parallel
```

glibc и musl libraries не взаимозаменяемы. Один configure tree нельзя
повторно использовать с другим compiler, architecture или libc. musl-профиль
фактически прошёл полный CTest в Alpine-контейнерах x86_64 и aarch64, и оба
дали тот же эталонный хеш физики, что и glibc.

Linux ARM64 проверяется полным CTest на нативном `ubuntu-26.04-arm`
runner в отдельном дереве `build/linux-gcc-arm64`; x86_64 presets для этой
цели не используются.

Локальный Android smoke не требует Android Studio: достаточно
command-line tools, NDK r30 и AVD. `ANDROID_NDK_HOME` указывает на
установленный NDK, например `$ANDROID_HOME/ndk/<версия>`:

```powershell
cmake --preset android-x86_64-core
cmake --build --preset android-x86_64-core-release --parallel
```

`laiue_static_core_link_smoke` можно передать в запущенный
x86_64 AVD через `adb`; это проверяет native ABI и статическое
замыкание. Для NativeActivity-клиента используйте preset
`android-x86_64-walk`: он собирает Vulkan surface path и `.so`; APK
target автоматически получает build-tools и `android.jar` из
`ANDROID_SDK_ROOT`/`ANDROID_HOME`, если они установлены, и включается после
явного задания `-DLAIUE_ANDROID_KEYSTORE=<keystore>`. Движок не создаёт и не
хранит ключ подписи. Renderer выбирает dynamic rendering на Vulkan 1.3 и
совместимые render-pass/framebuffer на Vulkan 1.2; поэтому emulator с
SwiftShader 1.2 также проходит NativeActivity smoke, не требуя 1.3-only
расширений. Push descriptors Vulkan 1.4 (или `VK_KHR_push_descriptor`)
используются, когда устройство их даёт, и не требуются.

На Windows с динамическими модулями сборка также создаёт
`laiue_graphics_d3d12.dll`. При найденном Vulkan SDK Linux/Windows собирают
`laiue_graphics_vulkan.so|dll` отдельно. Оба provider-а используют один
`laiue.graphics` service ABI, но имеют разные descriptor ID; в поставку
включают только выбранный профилем artifact. Старый `laiue_render` намеренно
оставлен compatibility-мостом и может содержать оба backend-а для прежних
клиентов.

## Vulkan offscreen на Linux

Нужны CMake 4.2+, Ninja, GCC или Clang, заголовки Vulkan и любой драйвер.
GPU не обязателен: программный `lavapipe` из Mesa даёт устройство Vulkan 1.4
и позволяет проверять рендер там, где видеокарты нет вовсе — именно так
кадр проверяется в CI.

```sh
sudo apt-get install -y libvulkan-dev mesa-vulkan-drivers vulkan-validationlayers
cmake --preset linux-vulkan-offscreen
cmake --build --preset linux-vulkan-offscreen-release --parallel
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json     ctest --preset linux-vulkan-offscreen-release --parallel --no-tests=error
```

Тест `laiue.render.offscreen_frame` рисует кадр целиком и сверяет пиксели:
очистку небом, положение квада, инстансный путь, шесть граней панорамы с
резолвом, слой интерфейса и изменение размера. Прогонять его стоит со
слоями проверки Khronos — без них драйвер молча прощает часть нарушений
спецификации:

```sh
VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation     ctest --preset linux-vulkan-offscreen-debug --parallel --no-tests=error
```

### Шейдеры

Исходник шейдера один и тот же HLSL в `shaders/`, и один компилятор, DXC,
собирает его под каждый бэкенд в сборке: DXIL Shader Model 6.0 для D3D12 и
SPIR-V для Vulkan. Результаты лежат в `src/graphics/render/generated/d3d12/`
и `.../vulkan/` массивами `uint32_t` и хранятся в репозитории, поэтому
сборка без DXC работает на любой платформе.

CMake ищет `dxc` в `PATH`, Vulkan SDK и Windows SDK; явный путь задаёт
`-DLAIUE_DXC=<путь>`. Сборки DXC умеют разное: в DXC из Windows SDK нет
SPIR-V, а DXC без валидатора не подписывает DXIL, который D3D12 тогда не
примет. Поэтому для каждого бэкенда берётся первый кандидат, который
собирает пробный шейдер; если подходящего нет, используется checked-in
байткод, а `-DLAIUE_REQUIRE_SHADER_COMPILER=ON` делает это ошибкой
конфигурации.
Цель `laiue_verify_shaders` сверяет checked-in файлы с выводом компилятора
побайтово. Совпадение гарантирует релиз DXC, закреплённый в CI
(`DXC_RELEASE` в `.github/workflows/build.yml`).

Раскладка дескрипторов задаётся сдвигами регистров (`-fvk-*-shift`), а не
атрибутами `[[vk::binding]]`: общий исходник остаётся без Vulkan-разметки.
Сдвиги переводят регистры в один descriptor set без коллизий — `b0` в
binding 0, `t0`..`t3` в 1..4, `s0` в 5.

Смещения констант кадра, которые DXC даёт в SPIR-V, совпадают с упаковкой
HLSL на D3D12, поэтому структура констант в C общая для обоих бэкендов. Ось Y
возвращается к соглашению D3D отрицательной высотой viewport — иначе
кадр оказался бы перевёрнут, а обход граней инвертирован.

## macOS core

Нужны CMake 4.2+, Ninja и AppleClang. Каждый configure tree содержит ровно
один native slice:

```sh
cmake --preset macos-clang-arm64
cmake --build --preset macos-clang-arm64-release --parallel
ctest --preset macos-clang-arm64-release --parallel --no-tests=error
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
- Переносимые модули (`content`, `mesh`, `mod`, `physics`, `runtime`,
  `scene`, `world`)
  включают только стандартные заголовки C и интринсики процессора; память,
  время, файлы и потоки они получают через `platform/system.h`. Правило
  проверяется `tools/check_architecture.cmake` и существует потому, что
  проверка границ между модулями смотрит только на `"..."`-включения:
  обращение прямо в системный API через угловые скобки она не видит, и
  мешер именно так вызывал `HeapAlloc` в обход платформенного слоя.
- Модули `render`, `ui`, `audio` и `input` привязаны к платформе или
  бэкенду осознанно и в это правило не входят.
- Внутренние пути канонизируются как UTF-8 с `/`; platform boundary
  преобразует их в native representation.
- Указатели, `wchar_t` и native structs не записываются в переносимые файлы.
- Имена внутри packs отклоняют absolute path, `..`, separators, symlink или
  reparse traversal и ASCII case-collision.
- `PlatformReadFilePrefix` читает ограниченный префикс в буфер вызывающей
  стороны без выделения памяти размером с файл. Размер полного файла
  проверяется на том же открытом handle; каталоги, устройства, FIFO и
  symlink/reparse в последнем компоненте отклоняются. Проверку родительских
  каталогов по-прежнему выполняет вызывающая сторона. Префикс не является
  snapshot при конкурентной записи в файл; при ошибке оба счётчика
  обнуляются, но буфер мог быть частично изменён. `laiue.platform.file`
  проверяет этот контракт, включая размер sparse-файла больше 4 ГиБ.
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
`LAIUE_NATIVE_MOD_MODE=OFF`. Android core preset фиксирует NDK r30 и API 28,
а walk preset добавляет NativeActivity/Vulkan для ARM64 или x86_64;
iOS preset фиксирует iOS 15, ARM64 и отключённую подпись build-only цели. CI
компилирует Android targets и линкует game `.so`; Apple runner линкует
минимальный unsigned app bundle. Android-профиль дополнительно собран
локально в контейнере с NDK r29: получившийся `libsimulation_of_sins.so` —
AArch64 ELF, экспортирует свою точку входа, а среди неопределённых символов у
него только Android libc. Debug/no-LTO цели whole-archive включают
каждый object game core и модулей `world`, `physics`, `content`, `mod` без
section GC, чтобы статический архив или optimizer не мог скрыть unresolved
symbol. Отдельная Release-цель проверяет финальную LTO/dead-strip линковку.
Это не подписанный store package: подпись и zipalign выполняются только
явно заданными SDK build-tools и keystore. NativeActivity `.so` отдельно
проверяется ELF-импортами и может быть встроен в приложение владельца.

Mobile shell получает resource и writable application directories от ОС,
при необходимости копирует data-only packs в app container и передаёт явный
root в `LaiueContentCatalogCreate`. Android walk уже покрывает Vulkan
surface/lifecycle, touch и pause/resume; полноценный Apple-клиент с Metal,
audio-output и store-specific memory/thermal policy остаётся следующим
этапом.

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

Публичные основания для этой границы: [Android NDK r30](https://developer.android.com/ndk/downloads/),
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
