# Разработка laiue

## Локальная проверка

Проект собирается только под Windows: CMake 3.28+, MSVC или clang-cl.

```powershell
cmake --preset ninja-clang-debug
cmake --build --preset ninja-clang-debug --parallel

cmake --preset ninja-clang-release
cmake --build --preset ninja-clang-release --parallel

pwsh -NoProfile -File tools/check_architecture.ps1
git diff --check
```

CI собирает MSVC Debug и Release с warnings-as-errors и проверяет, что
сборка не изменила tracked-файлы. Автоматические тесты пока не подключены.

## Блокировка DLL при сборке

Перед пересборкой закройте `laiue.exe` и `laiue_server.exe` из того же
`build/.../bin/<Configuration>`. Иначе Windows не даст линкеру заменить
EXE/DLL (`permission denied`, `LNK1104`). Корневая сборка сама показывает
имя, PID и путь блокирующего процесса.

Штатно закрывайте клиент через меню, сервер — через `Ctrl+C`. Для
зависшего процесса сначала проверьте путь:

```powershell
Get-Process laiue,laiue_server -ErrorAction SilentlyContinue |
    Select-Object Id, ProcessName, Path
Stop-Process -Id <PID>
```

Разные build-каталоги друг другу не мешают. Ninja + MSVC запускайте из
Developer PowerShell/Command Prompt; иначе используйте preset
`visual-studio`.

## Архитектурные правила

- Новая зависимость DLL объявляется в `src/<module>/CMakeLists.txt`.
- Нижние модули не включают `core`; разрешённый граф проверяет
  `tools/check_architecture.ps1`.
- Крупную DLL делите на внутренние `.c/.h`, не создавая ABI без
  самостоятельного жизненного цикла и второго потребителя.
- `network` принимает только bounded wire-format. Клиент не задаёт
  авторитетную позицию, инвентарь или результат правки блока.
- Текущий TCP backend остаётся loopback-only. Remote backend обязан
  использовать TLS 1.3 с проверкой server identity и без plaintext fallback.
- Оптимизация горячего пути требует повторяемого замера до и после.

## Совместимость

- `sdk/laiue_mod_api.h` растёт только добавлением полей в хвост;
  потребитель проверяет `structSize`.
- Несовместимое изменение SDK требует новой версии API.
- Дисковые и сетевые форматы — little-endian, versioned и bounded.
- Новая несовместимая раскладка получает новую версию, а не тихо заменяет
  старую.

## Генерируемые файлы

- `src/render/generated/*.h` создаются из `shaders/*.hlsl`; после изменения
  HLSL пересоберите проект и commit-ьте обновлённые заголовки.
- Бинарные шейдеры встроенных `.lsp` пересобираются тем же профилем
  `*_5_0`, `/O3`, `/Qstrip_debug`, `/Qstrip_reflect`.
- `tools/generate_textures.ps1` создаёт исходные PNG, а
  `tools/build_texture_pack.ps1` собирает `.ltp`.

Не форматируйте весь проект вместе с функциональным изменением. Для
затронутых C/H-файлов используйте `.clang-format`; предупреждения считаются
ошибками.

## Документация и SDK

При изменении поведения обновляйте ближайший документ, а не добавляйте
второе описание в README. Основные контракты:

- [архитектура](docs/architecture.md)
- [мультиплеер](docs/multiplayer.md)
- [форматы](docs/content_formats.md)
- [шейдеры](docs/shaderpacks.md)
- [моды](docs/modding.md)
- [сохранения](docs/world_format.md)

Перед публикацией SDK соберите пять примеров из `sdk/examples/`.
