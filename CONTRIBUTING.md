# Разработка laiue

## Локальная проверка

Канонические границы — Windows x86_64 с полным графическим набором и Linux
x86_64 core. Нужны CMake 3.28+ и MSVC/clang-cl на Windows либо GCC/Clang на
Linux. Матрица и зависимости описаны в
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

Диагностический preset `linux-gcc-asan` добавляет ASan/UBSan. Configure
выполняется один раз на toolchain; Debug и Release выбираются build/test
preset с соответствующим суффиксом.

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
  backend-файлах.
- `world` и `physics` не выполняют platform I/O и не выбирают ОС через
  `#ifdef`.
- Крупную библиотеку делят на внутренние `.c/.h`, если новой подсистеме не
  нужен самостоятельный жизненный цикл и отдельный публичный C API.
- CMake options, include paths, definitions и linker flags target-scoped.
- Configure/build не меняют source tree.
- Каждый буфер и handle имеет одного владельца и явную release-функцию.
  Потребитель не освобождает память allocator-ом из другой DLL/SO.
- Fixed-step и другие горячие пути не содержат allocation, файлового I/O и
  покадрового logging.
- Оптимизация требует повторяемого измерения до и после.

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
  symlink/reparse escape и case-collision.
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
- [аудио](docs/audio.md)

`CONTRIBUTING.md` — канонический набор правил разработки. Editor/agent-
specific файлы ссылаются сюда и не копируют правила.
