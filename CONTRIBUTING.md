# Разработка laiue

## Локальная проверка

Канонические границы — Windows x86_64 с полным графическим набором,
Linux x86_64/ARM64 core и macOS arm64/x86_64 core. Нужны CMake 3.28+,
MSVC/clang-cl на Windows, GCC/Clang на Linux либо AppleClang на macOS.
Матрица и зависимости описаны в
[docs/portability.md](docs/portability.md).

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug

cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release

pwsh -NoProfile -File tools/check_architecture.ps1
git diff --check
```

Для `clang-cl` используйте `windows-clang` из x64 VS Developer shell. Linux
core проверяется реальной Linux-сборкой:

```sh
cmake --preset linux-gcc
cmake --build --preset linux-gcc-release --parallel
ctest --preset linux-gcc-release
```

Официальный x86_64 Linux CI выполняет те же команды внутри Debian 13 и Alpine
3.23 Docker containers. Linux ARM64 фактически прошёл полный CTest в Docker и
дополнительно назначен нативному GitHub-hosted ARM64 runner. macOS workflow
назначает отдельные jobs Apple Silicon и Intel:

```sh
cmake --preset macos-clang-arm64
cmake --build --preset macos-clang-arm64-release --parallel
ctest --preset macos-clang-arm64-release --no-tests=error

# На Intel host/runner:
cmake --preset macos-clang-x86_64
cmake --build --preset macos-clang-x86_64-release --parallel
ctest --preset macos-clang-x86_64-release --no-tests=error
```

Диагностический preset `linux-gcc-asan` добавляет ASan/UBSan. Configure
выполняется один раз на toolchain; Debug и Release выбираются build/test
preset с соответствующим суффиксом.

Публичная проверка внешней статической core-границы запускается отдельно:

```sh
cmake --preset linux-external-port-smoke
cmake --build --preset linux-external-port-smoke-release --parallel
ctest --preset linux-external-port-smoke-release --no-tests=error
```

Она проверяет контракт superbuild, но не заменяет закрытый toolchain и запуск
на консольном hardware.

Тесты живут в `tests/` и регистрируются в CTest. Аудио-тест может вернуть
125 и быть отмечен пропущенным, если в Windows отсутствует Media Foundation.
Новый platform boundary нельзя считать подтверждённым только по syntax
check: нужен реальный link и запуск релевантных тестов.

## Архитектурные правила

- Новая зависимость объявляется в `src/<module>/CMakeLists.txt`.
- Production `.c` компилируется ровно в одном модуле; общие исходники не
  копируются между targets.
- Нижний модуль не включает заголовки верхнего. Допустимый граф проверяет
  `tools/check_architecture.ps1`.
- Win32/POSIX API разрешены только в `src/platform` и специализированных
  backend-файлах. Переносимые модули (`content`, `mesh`, `mod`, `physics`,
  `runtime`, `world`) включают лишь стандартные заголовки C и интринсики
  процессора; это тоже проверяет `tools/check_architecture.ps1`, потому что
  проверка графа модулей видит только `"..."`-включения и обращение прямо
  в системный API через угловые скобки пропускает.
- Графический слой собирается по выбранному бэкенду рендера, а не по
  платформе: `LAIUE_RENDER_BACKEND` даёт `D3D12` либо `VULKAN`, и состав
  модулей у них разный. Заголовки в SDK ставятся по собранным целям, а не
  по флагу `LAIUE_BUILD_GRAPHICS`.
- `UNIX` не используется как синоним Linux: Darwin и внешние консольные
  adapters имеют собственные реализации и capability checks.
- `world` и `physics` не выполняют platform I/O и не выбирают ОС через
  `#ifdef`.
- `mod` не включает игровые, сетевые, scene или render interfaces: прикладные
  возможности публикуются узкими versioned service tables.
- Крупную библиотеку делят на внутренние `.c/.h`, если новой подсистеме не
  нужен самостоятельный жизненный цикл и отдельный публичный C API.
- CMake options, include paths, definitions и linker flags target-scoped.
- Configure/build не меняют source tree.
- Каждый буфер и handle имеет одного владельца и явную release-функцию.
  Потребитель не освобождает память allocator-ом из другой DLL/SO.
- Fixed-step и другие горячие пути не содержат allocation, файлового I/O и
  покадрового logging.
- Оптимизация требует повторяемого измерения до и после.

## Mobile

- Android ARM64 проверяется NDK r29 build-only job; iOS ARM64 — Xcode 26
  unsigned link job с deployment target 15.0. Ни один из них не заменяет
  устройство, APK/IPA, Vulkan/Metal и lifecycle tests.
- Mobile application обязан передать явный app-container root каталогу
  содержимого; executable directory там намеренно недоступен как default.
- Нативные моды на mobile отключены. Data-only packs проходят отдельную
  store policy review; произвольные shader scripts/source не обещаются.

## Закрытые консоли

- Публичный репозиторий содержит только platform-agnostic contracts и точку
  подключения внешнего adapter.
- SDK, toolchain files, proprietary headers, libraries, hardware commands и
  закрытая документация не добавляются в этот репозиторий или публичный CI.
- Консольную поддержку подтверждает только закрытая нативная сборка и запуск
  на официальном dev/test hardware; mock adapter подтверждает лишь границы.
- Политики native mods, shader packs, storage и пользовательского содержимого
  не переносятся с PC по предположению: их определяет документация платформы.
- Self-hosted console runner не исполняет код из pull request. В него попадает
  только одобренный commit защищённой ветки через закрытый integration repo.
- До появления закрытой нативной сборки и запуска Xbox, PlayStation и
  Nintendo считаются не заявленными платформами, даже если mock проходит.

## Контракт World

- `WorldCreate(NULL)` всегда создаёт пустой базовый слой.
- Непустой `WorldBaseProvider` обязан предоставить `getBlock`.
- `fillRegion`, если задан, заполняет каждый элемент документированной
  раскладки и возвращает корректную классификацию области.
- Provider `context` принадлежит приложению, живёт дольше `World` и не
  освобождается движком.
- Provider callbacks thread-safe: чтение и meshing могут идти параллельно.
- `BlockType` 0 — воздух; значения 1–255 непрозрачны для `world`.
- Правка, равная базовому значению provider, не должна оставлять лишний
  sparse override.
- `WorldApplyBlockBatch` либо публикует весь набор и новый revision, либо не
  меняет ничего.

`WorldRebase` вызывается только после остановки streaming, meshing и других
пользователей локальных координат. Сначала готовятся все allocations, затем
вызывается provider callback, и только после его успеха переключается
состояние. Ошибка callback сохраняет прежние origin, blocks и revision.

World origin не заменяет render origin. До преобразования во `float` позиция
чанка вычитается из близкого к камере локального начала. В шейдеры не
передаются `InfiniteCoord` и большие абсолютные значения.

## Контракт physics

- Интегратор приложения использует fixed step и стабильный порядок тел.
- Один simulation step читает неизменяемый snapshot world/collider callbacks.
- Динамические коллайдеры имеют уникальный ненулевой `stableId`; callback не
  возвращает усечённую выборку как успешную.
- Перед шагом приложение вызывает `VoxelBodyLocalRangeIsResolved` и при
  необходимости синхронно rebases `World` и все локальные тела.
- Изменение collision arithmetic проверяет общий reference hash, повторный
  прогон, rebased schedule, границы чанков и resting-contact drift.
- Linux ARM64 уже подтвердил тот же hash отдельным Docker-запуском. Каждая
  новая OS/architecture/toolchain комбинация всё равно требует собственного
  нативного запуска; результат Linux ARM64 не заменяет macOS ARM64.

## Контракт модов

- Нативный мод — доверенный in-process код; path validation не называется
  sandbox.
- Мод включает standalone `mod_api.h`, не линкуется с внутренними DLL/SO и
  запрашивает только versioned services.
- Registry сервисов не меняется, пока моды загружаются, выгружаются или
  активны. Service implementation живёт до завершения `unload`.
- Worker threads мода останавливаются и join-ятся внутри `unload` до закрытия
  dynamic library.
- `LoadMany` получает явный детерминированный порядок приложения; discovery
  никогда не означает автоматическое выполнение найденного кода.

## Публичный API и совместимость

- Публичные функции отмечаются правильным `LAIUE_<MODULE>_API` из
  `src/api.h`.
- Публичная структура получает несовместимое изменение только вместе с
  обновлением версии API; новые расширяемые структуры используют
  `structSize` либо отдельный versioned descriptor.
- Внутренние ABI одной сборки могут меняться синхронно, но установленные
  headers и libraries должны соответствовать друг другу.
- Дисковые форматы little-endian, versioned и bounded. Новая несовместимая
  раскладка получает новую версию.
- Reader проверяет magic, version, count, offset, overflow и точный размер до
  allocation или индексирования.
- Неизвестная версия отклоняется явно; частично разобранный файл не меняет
  активное состояние.

## Переносимость и безопасность

- Внутренние пути используют UTF-8 и `/`; platform boundary отвечает за
  native representation.
- Указатели, `wchar_t` и native structs не сериализуются.
- Имя из внешнего файла проверяется до построения пути. Запрещены traversal,
  symlink/reparse escape и ASCII case-collision.
- Windows no-CRT изменение проверяется по imports готового бинарника, а не
  только успешной компиляцией.
- Linux ABI проверяется отдельно для используемой libc; glibc и musl
  artifacts не смешиваются.
- Любой внешний размер и offset проверяется до арифметики, allocation и
  обращения к памяти.
- Исправление ошибки получает минимальный regression test, который падал до
  исправления.

## Изменения и generated-файлы

- Сначала запускайте самый узкий релевантный тест, затем platform build и
  полный CTest.
- Не маскируйте предупреждения глобальным отключением.
- Для затронутых C/H-файлов применяйте `.clang-format`; не форматируйте весь
  проект вместе с функциональным изменением.
- `src/render/generated/*.h` — checked-in shader fallback. Обычная сборка
  создаёт headers в binary tree; fallback обновляется только явной командой
  и проверяется diff.
- Изменение `shaders/*.hlsl` проверяется компиляцией всех затронутых entry/
  profile и сравнением generated fallback.
- Workflow Actions закрепляются полным commit SHA; floating action refs в
  release workflow запрещены.
- Новая внешняя зависимость требует закреплённой версии, проверки hash,
  лицензии, notices и сборки на каждой затронутой ABI.
- Обычный configure не загружает файлы из интернета.

## Документация

При изменении контракта обновляйте ближайший документ вместо дублирования
деталей в нескольких местах:

- [архитектура](docs/architecture.md)
- [переносимость](docs/portability.md)
- [архитектура содержимого](docs/content_architecture.md)
- [форматы содержимого](docs/content_formats.md)
- [шейдерпаки](docs/shaderpacks.md)
- [текстурпаки](docs/texturepacks.md)
- [моды](docs/modding.md)
- [физика](docs/physics.md)
- [аудио](docs/audio.md)

`CONTRIBUTING.md` — канонический набор правил разработки. Editor/agent-
specific файлы ссылаются сюда и не копируют правила.
