# Разработка laiue

## Сборка

Проект поддерживает Windows, CMake 3.28+, MSVC или clang-cl. Основная
локальная проверка:

```powershell
cmake --preset ninja-clang-debug
cmake --build --preset ninja-clang-debug --parallel
cmake --preset ninja-clang-release
cmake --build --preset ninja-clang-release --parallel
git diff --check
pwsh -NoProfile -File tools/check_architecture.ps1
```

CI собирает Debug и Release с MSVC. Предупреждения компилятора считаются
ошибками. Автоматические тесты пока сознательно не входят в workflow.

### Перед каждой пересборкой

Полностью закройте `laiue.exe` и `laiue_server.exe`, запущенные именно из
пересобираемого `build/.../bin/<Configuration>`. Windows блокирует загруженные
EXE/DLL, поэтому `lld-link: permission denied` или `LNK1104` означает не
ошибку CMake, а работающий процесс, удерживающий файл.

Корневая сборка проверяет это автоматически и печатает имя, PID и путь
блокирующего процесса. Штатно завершайте клиент через меню, сервер — через
`Ctrl+C`. Только для зависшего процесса:

```powershell
Get-Process laiue,laiue_server -ErrorAction SilentlyContinue |
    Select-Object Id, ProcessName, Path
Stop-Process -Id <PID>
```

Процессы из другого build-каталога не мешают сборке и проверкой не
блокируются. Не используйте один build-каталог одновременно для MSVC и
clang-cl. Ninja + MSVC запускайте из Developer PowerShell/Command Prompt;
обычный терминал может не содержать путей к стандартным заголовкам. Если это
неудобно, используйте preset `visual-studio`.

## Границы изменений

- DLL-модульность сохраняется: новая зависимость добавляется только в
  `src/<module>/CMakeLists.txt`; нижний модуль не включает заголовки `core`.
- Допустимые направления include-зависимостей зафиксированы в
  `tools/check_architecture.ps1`; корневая цель `laiue` запускает проверку.
- Внутренние файлы большой DLL можно делить без создания дополнительных
  DLL и без публикации внутренних структур.
- Публичный API модов меняется добавлением полей в хвост таблиц. Код обязан
  проверять `sizeBytes`; несовместимое изменение требует новой версии API.
- Бинарные форматы меняются обратно совместимо либо получают новую версию.
  Все числа на диске little-endian, входные размеры проверяются до аллокаций.
- Горячие пути оптимизируются после измерения на повторяемой сцене.
- `network` принимает только bounded wire-format, не C-структуры. Клиент не
  присылает авторитетные координаты или результат правки. Увеличение лимита
  packet/buffer/rate требует threat-model в `docs/multiplayer.md`.
- Текущий TCP transport остаётся loopback-only. Удалённый backend обязан
  использовать TLS 1.3 с проверкой server identity и не иметь plaintext
  fallback.

## Форматирование и анализ

Для затронутых C/H-файлов можно запустить `clang-format -i <files>`.
Конфигурации находятся в `.clang-format` и `.clang-tidy`. Не форматируйте
механически весь проект вместе с функциональным изменением.

## Генерируемые файлы

Заголовки в `src/render/generated/` создаются из `shaders/*.hlsl` при
сборке. После изменения шейдера соберите проект и включите обновлённые
заголовки в тот же commit. Текстуры генерирует
`tools/generate_textures.ps1`; исходные PNG лежат в
`assets/texturepacks_src/`.

## Содержимое и SDK

- Форматы содержимого: `docs/content_formats.md`.
- Шейдерный контракт: `docs/shaderpacks.md`.
- API и примеры модов: `docs/modding.md` и `sdk/examples/`.
- Перед публикацией SDK соберите все пять примеров из корневого CMake.
