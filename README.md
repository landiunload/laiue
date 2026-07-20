# laiue

Воксельный движок 0.5.0 для Windows: C17 без CRT, Direct3D 12,
потоковое аудио, DLL-модули, нативные моды и отдельный authoritative server.

Сейчас готовы одиночные миры, сохранения, креатив/выживание, инвентарь,
дропы и локальный мультиплеер. Сетевой transport привязан к
`127.0.0.1`; открывать его в LAN или Internet пока нельзя.

## Сборка

Требуются CMake 3.28+ и Visual Studio с MSVC. Для Ninja нужны Ninja и,
в зависимости от preset, Developer Command Prompt MSVC или LLVM
(`clang-cl` + `lld-link`).

```powershell
# Visual Studio, Release
cmake --preset visual-studio
cmake --build --preset visual-studio-release --parallel

# Ninja + clang-cl, Release
cmake --preset ninja-clang-release
cmake --build --preset ninja-clang-release --parallel
```

Результат находится в `build/<preset>/bin/<Configuration>`. Перед
пересборкой закройте клиент и сервер из этого каталога: Windows блокирует
загруженные EXE/DLL. Сборка проверяет блокировку заранее и выводит PID.
Подробности — в [CONTRIBUTING.md](CONTRIBUTING.md).

## Запуск

Из runtime-каталога:

```powershell
./laiue.exe

# Для локальной сетевой игры — в другом терминале
./laiue_server.exe
```

Клиент открывает главное меню без мира и фоновых meshing-задач. Мир,
игровые GPU-ресурсы и DLL-хост модов создаются только после выбора сессии
и освобождаются при возврате в меню.

## Управление

| Ввод | Действие |
|---|---|
| `W A S D` | движение; в креативе — полёт |
| `Space` | прыжок / вверх в креативе |
| `Ctrl` | бег |
| `Shift` | приседание |
| `ЛКМ` | ломать блок |
| `ПКМ` | поставить выбранный блок |
| `E` | инвентарь, 36 слотов |
| `1`–`9`, колесо | слот хотбара |
| `G` | креатив/выживание, только в одиночной игре |
| `Esc` | назад, пауза, возврат в меню или выход |
| `F3` | статистика streaming/renderer |
| `F7`, `Shift+F7` | включить / выключить mouse look |
| `V` | вертикальная синхронизация |

В выживании ломание занимает время, блок выпадает предметом, появляются
частицы, а установка расходует инвентарь. В креативе ломание мгновенное,
дропов и частиц нет, предметы не расходуются.

## Архитектура

```text
laiue.exe -> laiue_core.dll
             ├─ window + input + audio
             ├─ world + mesher + render
             ├─ physics + gameplay + interaction
             └─ content + mod + network

laiue_server.exe -> network + content + mod + world
                    + physics + gameplay + interaction
```

`core` компонует клиент. Сервер не зависит от `core`, окна, input,
renderer или mesher. Направления include-зависимостей проверяются при
сборке. Полное описание — в [docs/architecture.md](docs/architecture.md).

## Содержимое

Поддерживаются три категории:

- моды: `mods/<name>.lmp` с `mod.lm` и DLL;
- шейдерпаки: `shaders/<name>.lsp` с DXBC-стадиями `.ls`;
- текстурпаки: `textures/<name>.ltp`.

`.lr/.lrp` и `.ld/.ldp` удалены. Нативные моды не изолированы и должны
быть доверенными. Сетевой набор `server`/`both` модов сверяется по
порядку, id, версии и SHA-256; `client`-моды в сравнении не участвуют.

## Сохранения

Одиночные миры лежат в `saves/<slot>`: метаданные, правки чанков, игрок,
инвентарь, `mods.lock` и `moddata/`. Основные файлы записываются через
временный файл с атомарной заменой. Dedicated server сохраняет только
`saves/default/chunks.dat` и данные модов; состояние игроков пока временное.

## Документация

- [архитектура](docs/architecture.md)
- [аудио](docs/audio.md)
- [актуальный план](docs/improvement_plan.md)
- [мультиплеер и безопасность](docs/multiplayer.md)
- [моддинг и SDK](docs/modding.md)
- [форматы содержимого](docs/content_formats.md)
- [шейдерпаки](docs/shaderpacks.md)
- [текстурпаки](docs/texturepacks.md)
- [сохранения](docs/world_format.md)
- [физика игрока](docs/player_physics.md)

## Лицензия

MIT
