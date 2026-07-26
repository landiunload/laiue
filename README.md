# laiue

Воксельный движок 0.5.0: Windows-клиент на C17 без CRT и Direct3D 12,
а также authoritative dedicated server для Windows и Linux x86_64.
Клиент и сервер используют общий headless-стек мира, физики, gameplay,
content, network и модов.

Сейчас готовы одиночные миры, сохранения, креатив/выживание, инвентарь,
дропы и client/server multiplayer. Удалённый production transport —
QUIC/UDP с TLS 1.3, обязательной проверкой server identity и поддержкой
IPv4/IPv6. Если MsQuic или credentials недоступны, он завершается
fail-closed без plaintext fallback.

## Сборка

Требуется CMake 3.28+. Windows-клиент собирается MSVC или clang-cl; Linux
server — GCC/Clang с OpenSSL 3 и MsQuic 2.5.9.

```powershell
# Visual Studio, Release
cmake --preset visual-studio
cmake --build --preset visual-studio-release --parallel

# Ninja + clang-cl, Release
cmake --preset ninja-clang-release
cmake --build --preset ninja-clang-release --parallel
```

Linux server:

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release \
  --target laiue_server_bundle --parallel
```

Также доступны `linux-gcc-debug`, `linux-clang-release`,
`linux-gcc-asan` и `linux-musl-release`. Готовый server stage находится в
`build/<preset>/bundles/server/<Configuration>`, а отдельные build outputs —
в `build/<preset>/bin/<Configuration>`. Перед
пересборкой закройте клиент и сервер из этого каталога: Windows блокирует
загруженные EXE/DLL. Сборка проверяет блокировку заранее и выводит PID.
Зависимости, options, install и ABI-матрица описаны в
[docs/portability.md](docs/portability.md).

## Запуск

Из runtime-каталога:

```powershell
./laiue.exe

# Для локальной сетевой игры — в другом терминале
./laiue_server.exe
```

Перед удалённым запуском настройте certificate/key и bind policy в
`server.cfg`. По умолчанию порт — UDP 27180, режим `dual` слушает IPv4 и
IPv6 одновременно. Инструкции для Linux/Windows, SAN, pin и нестандартного
порта — в [docs/secure_server.md](docs/secure_server.md).

Клиент открывает главное меню без мира и фоновых meshing-задач. Мир,
игровые GPU-ресурсы и native host модов создаются только после выбора сессии
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
             ├─ mesher + render
             └─ laiue::headless_stack
                  └─ world + physics + gameplay + interaction
                     + content + mod + network

laiue_server(.exe) -> laiue::headless_stack
```

`core` компонует клиент. Сервер не зависит от `core`, окна, input,
renderer или mesher. Направления include-зависимостей проверяются при
сборке. Полное описание — в [docs/architecture.md](docs/architecture.md).

## Содержимое

Поддерживаются три категории:

- моды: `mods/<name>.lmp` с `mod.lm` v2 и платформенными DLL/SO;
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
- [secure remote server](docs/secure_server.md)
- [переносимость и Linux](docs/portability.md)
- [моддинг и SDK](docs/modding.md)
- [форматы содержимого](docs/content_formats.md)
- [шейдерпаки](docs/shaderpacks.md)
- [текстурпаки](docs/texturepacks.md)
- [сохранения](docs/world_format.md)
- [физика игрока](docs/player_physics.md)

## Лицензия

MIT
