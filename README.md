# laiue

`laiue` 0.7.0 — встраиваемый воксельный движок на C17. Репозиторий
содержит библиотечные runtime-модули: бесконечные координаты и разреженный
мир, физику, построение чанковых мешей, D3D12-рендер, окно, ввод, аудио,
сцену, UI, каталог визуального содержимого и host для нативных модов.

Прикладной проект из прежнего репозитория сохранён отдельно:
[landiunload/laiue-game](https://github.com/landiunload/laiue-game).

Движок не создаёт ландшафт самостоятельно. Приложение передаёт источник
базовых блоков через `WorldBaseProvider`, а `WorldCreate(NULL)` создаёт
пустой бесконечный мир. Значение `BlockType` 0 зарезервировано для воздуха;
материалы 1–255 определяет приложение.

В движке нет игрового сценария, процедурной генерации, сетевого протокола и
сервера. Эти политики принадлежат приложению; текущий этап намеренно не
добавляет серверную подсистему.

## Модули

| Модуль | Назначение |
|---|---|
| `platform_support` | внутренняя граница памяти, locks, файлов и времени |
| `world` | `InfiniteCoord`, rebasing, базовый provider и разреженные правки |
| `physics` | переносимые AABB и столкновения с вокселями |
| `content` | безопасные имена, категории и выбор паков |
| `mod` | discovery, ABI v1, versioned services и жизненный цикл нативных модов |
| `window`, `input`, `audio` | Windows-окно, Raw Input и Media Foundation |
| `mesh` | greedy meshing чанков `64³` |
| `render` | Direct3D 12, GPU-меши, текстуры и шейдеры |
| `scene` | камера, streaming, panorama и voxel raycast |
| `ui` | immediate-mode UI поверх renderer |

`laiue::engine` объединяет доступные для выбранной платформы модули.
Windows собирает полный графический набор. Linux и macOS экспортируют
headless-ядро: `world`, `physics`, `content` и `mod`; `platform_support`
остаётся внутренней реализацией этих библиотек. Наличие core-сборки не
означает наличие окна или рендера на этой платформе.

| Платформа | Core/headless | Полный клиент |
|---|---:|---:|
| Windows x86_64 | CI | D3D12, CI |
| Windows ARM64 | clang-cl собран и слинкован локально; native CI job | D3D12 собирается; на устройстве не запускался |
| Linux x86_64 | glibc и musl, проверено в Docker | ещё нет графического backend |
| Linux ARM64 | glibc и musl, проверено в Docker; native CI настроен | ещё нет графического backend |
| Steam Deck / SteamOS | Linux x86_64 core | нужен Vulkan/input/audio client |
| macOS arm64/x86_64 | macOS 11+, native CI настроен, локально не запускался | ещё нет Metal backend |
| Android ARM64 | NDK r29: static core и финальный `.so` собраны локально, CI настроен | ещё нет APK/Vulkan/input/audio shell |
| iOS/iPadOS ARM64 | iOS 15+ static core и unsigned link CI настроены | ещё нет приложения/Metal backend |
| tvOS/visionOS | mobile adapter contract | нет presets, client и device tests |
| Xbox / PlayStation / Nintendo | external static seam | нужны одобрение, закрытый SDK и hardware |
| WebAssembly/WebGPU | не заявлен | нужен отдельный web adapter |

## Сборка

Требуется CMake 3.28+ и компилятор C17. На Windows поддерживаются MSVC и
`clang-cl`; для графики нужен Windows SDK с Direct3D 12 и `fxc`. Если `fxc`
не найден, используются закоммиченные fallback-заголовки шейдеров.

```powershell
# Visual Studio/MSVC
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug

# clang-cl из x64 VS Developer PowerShell
cmake --preset windows-clang
cmake --build --preset windows-clang-release --parallel
ctest --preset windows-clang-release
```

Linux core:

```sh
cmake --preset linux-gcc
cmake --build --preset linux-gcc-release --parallel
ctest --preset linux-gcc-release
```

macOS core проверяется нативно на обеих архитектурах:

```sh
cmake --preset macos-clang-arm64
cmake --build --preset macos-clang-arm64-release --parallel
ctest --preset macos-clang-arm64-release

# На Intel runner/host используйте macos-clang-x86_64.
```

Android ARM64 core компилируется NDK r29. Это создаёт нативные static
libraries, а потребитель обязан выполнить финальную `.so`-линковку:

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r29
cmake --preset android-arm64-core
cmake --build --preset android-arm64-core-release --parallel
```

На macOS с Xcode 26 доступен build-only профиль iOS 15 ARM64:

```sh
cmake --preset ios-arm64-core
cmake --build --preset ios-arm64-core-release --parallel
```

Эти mobile-профили не являются APK/IPA и не подтверждают GPU, ввод, звук,
store packaging или запуск на устройстве.

Стандартный Release ориентирован на скорость: MSVC требует AVX2, а
Clang/GCC — полный уровень x86-64-v3. Для отдельного совместимого с более
старыми x86_64 CPU artifact укажите
`-DLAIUE_X86_64_LEVEL=sse2` при configure; смешивать библиотеки разных ISA
профилей в одном bundle нельзя.
Release SDK bundles на Unix-платформах дополнительно проходят `strip`; build
tree остаётся нетронутым, поэтому диагностика и тестирование не теряют символы.

Для Clang используйте `linux-clang`; диагностический preset —
`linux-gcc-asan`. Подробная матрица находится в
[docs/portability.md](docs/portability.md).

Linux CI выполняется внутри фиксированных Debian/Alpine Docker images.
Linux ARM64 core и детерминированная физика также фактически прошли полный
набор тестов в ARM64 Docker; workflow дополнительно запускает их на нативном
GitHub-hosted ARM64 runner. macOS workflow настроен на нативные Apple Silicon
и Intel runners, но из этой Windows-среды не запускался: один успешный slice
в любом случае не считается подтверждением второго.

## Установка

Установочный компонент `Engine` содержит библиотеки, публичные заголовки и
документацию; Windows graphics bundle дополнительно содержит HLSL-контракты:

```powershell
cmake --install build/windows-msvc --config Release `
  --prefix .\staging\laiue --component Engine
```

Цель `laiue_engine_bundle` собирает готовый stage в
`build/<configure>/bundles/engine/<Configuration>`. Потребитель, который
добавляет исходники через `add_subdirectory`, может линковаться с
`laiue::engine` либо с отдельными `laiue::<module>`. Установленный SDK
подключается так:

```cmake
find_package(laiue 0.7 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE laiue::engine)
```

Для узкой зависимости доступны цели `laiue::world`, `laiue::physics`,
`laiue::content`, `laiue::mod` и графические цели установленной
Windows-сборки.

## Mobile и закрытые платформенные адаптеры

Публичный репозиторий определяет только границы platform/render/input/audio
и не содержит консольных SDK, путей, заголовков либо NDA API. Внешний core
подключается через `LAIUE_PLATFORM_BACKEND=EXTERNAL`: закрытый CMake adapter
предоставляет реализацию `Platform*` и строгие FP-флаги, движок собирает
статические core-модули без динамических native-модов и без публичного SDK
export. Эту границу проверяет `linux-external-port-smoke`, но он не эмулирует
консоль.

Публичный `MobileCoreAdapter.cmake` применяет тот же контракт к Android и
Apple mobile family. Mobile application shell обязан передать явный каталог
ресурсов/данных в `LaiueContentCatalogCreate`: executable path на Android и
read-only app bundle на Apple не считаются допустимым writable content root.
Нативные динамические моды там отключены; data-only packs остаются отдельной
политикой приложения и магазина.

Xbox, PlayStation и Nintendo пока не являются поддерживаемыми целями. Их
реальные adapters собираются только в закрытом integration-проекте
официальным toolchain после одобрения разработчика. Без SDK и dev/test
hardware нельзя заявлять, что консольная сборка, рендер, ввод, packaging,
shader/texture packs или физический hash работают. Steam Deck относится к
Linux x86_64, но до Vulkan backend это только core, не игровой клиент.

## Мир приложения

Минимальный пустой мир:

```c
World* world = WorldCreate(NULL);
if (world == NULL)
{
    return false;
}

WorldSetBlock(world, 0, 0, 0, 1);
WorldDestroy(world);
```

Для внешнего источника заполните `WorldBaseProvider`:

```c
static BlockType ReadBase(void* context, int64_t x, int64_t y, int64_t z)
{
    const MyWorldSource* source = context;
    return MyWorldSourceRead(source, x, y, z);
}

WorldBaseProvider provider = {
    .context = &source,
    .getBlock = ReadBase,
    .fillRegion = NULL,
    .rebase = NULL,
};
World* world = WorldCreate(&provider);
```

`getBlock` обязателен для непустого provider. Опциональный `fillRegion`
ускоряет чтение прямоугольных областей, а `rebase` синхронизирует внешний
источник при сдвиге локального начала. Структура provider копируется, но
`context` принадлежит приложению и должен жить дольше `World`. Колбэки могут
вызываться параллельно, обязаны быть thread-safe и не должны повторно входить
в тот же `World`.

`World` хранит локальные `int64_t`-координаты рядом с активной областью, а
абсолютное начало — в `InfiniteCoord`. `WorldRebase` переносит локальное
начало на целое число чанков, сохраняя абсолютные позиции и разреженные
правки. Перед вызовом приложение останавливает streaming, meshing и другие
операции с локальными координатами. Если provider отклоняет сдвиг, состояние
мира не меняется; отклоняющий callback также оставляет свой `context` без
изменений.

Абсолютные координаты не передаются в GPU. Приложение выбирает близкую к
камере локальную точку `render origin`, вычитает её из позиций чанков и лишь
после этого преобразует результат во `float`. Поэтому точность рендера не
зависит от удаления от абсолютного нуля.

Подробные контракты описаны в
[docs/architecture.md](docs/architecture.md).

## Содержимое

Renderer поддерживает подключаемые шейдерпаки `.lsp` и GPU-ready
текстурпаки `.ltp`. Шейдерпак может независимо заменить любую из шести
текущих стадий chunk/panorama/UI; отсутствующие стадии берутся из встроенного
набора. Перезагрузка GPU-ресурсов транзакционна: ошибка оставляет прежний
рабочий набор. Проект приложения решает, какие паки поставлять и как
показывать выбор пользователю.

Нативные моды поставляются отдельными каталогами `.lmp`. Приложение задаёт
точный порядок включения и публикует модам только явно зарегистрированные
versioned services. Это доверенный in-process код, а не sandbox.

- [архитектура содержимого](docs/content_architecture.md)
- [форматы содержимого](docs/content_formats.md)
- [шейдерпаки](docs/shaderpacks.md)
- [текстурпаки](docs/texturepacks.md)
- [моды и ABI](docs/modding.md)
- [детерминированная физика](docs/physics.md)
- [аудио](docs/audio.md)

## Разработка

Правила зависимостей, тестирования и изменения публичного API находятся в
[CONTRIBUTING.md](CONTRIBUTING.md). Перед отправкой изменения запустите
релевантный build/CTest, архитектурную проверку и `git diff --check`.

## Лицензия

MIT
