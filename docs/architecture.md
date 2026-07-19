# Архитектура

## Модули

| Модуль | Ответственность |
|---|---|
| `launcher` | минимальный `laiue.exe`: `LoadLibrary` и `Start` |
| `platform` | Win32-окно, message loop, mouse look, время |
| `input` | Raw Input клавиатуры и мыши |
| `world` | генерация, чанки, дельты, свойства блоков и сохранение `chunks.dat` |
| `mesher` | greedy meshing: `World` → упакованные квады |
| `render` | D3D12, GPU-меши, шейдеры, текстуры, panorama и UI pass |
| `physics` | AABB и столкновения без зависимости от `World` |
| `gameplay` | контроллер игрока, движение, прыжок, стойка и инвентарь |
| `interaction` | raycast и проверяемая команда правки блока |
| `content` | каталог форматов и безопасный сетевой bundle |
| `mod` | профили модов, DLL-host, стороны и межмодовые интерфейсы |
| `network` | protocol v4, bounded queues и loopback TCP transport |
| `core` | клиентский composition root, UI, streaming, сохранения и эффекты |
| `server` | headless authoritative composition root |

`game/camera.h` содержит только переносимое состояние камеры. Операции с
матрицами находятся в `core/camera.*`.

## Направление зависимостей

`core` компонует клиентские подсистемы. Сервер компонует `network`,
`content`, `mod`, `world`, `physics`, `gameplay` и `interaction`, но не
зависит от `core`, `window`, `input`, `render` или `mesher`.

Нижние модули не включают заголовки `core`. Допустимый граф задан в
`src/*/CMakeLists.txt` и проверяется `tools/check_architecture.ps1` при
сборке. Владеющие состоянием объекты (`World`, `Renderer`, network handles)
непрозрачны; через внутренний ABI передаются только объявленные публичные
value-структуры и указатели на них.

Новая DLL нужна только если подсистема имеет самостоятельный жизненный
цикл, узкий C API и реального второго потребителя. Иначе добавляется
внутренний `.c/.h` существующего модуля.

## Жизненный цикл клиента

Главное меню — лёгкая оболочка:

1. `RendererCreate` создаёт swapchain, UI pipeline и статичный PNG-фон.
2. `World`, `ChunkStreaming`, игровые PSO, block textures и `ModHost`
   отсутствуют.
3. При выборе мира `RendererPrepareWorld` инициализирует игровые GPU-ресурсы,
   затем создаются мир, streaming, эффекты и хост модов.
4. При возврате в меню сначала останавливаются моды/workers, затем
   уничтожается мир и вызывается `RendererReleaseWorld`.

Так меню не генерирует чанки и не держит игровую сессию в фоне.

## Потоки и владение

| Объект | Владелец | Потоки |
|---|---|---|
| клиентские `World`, `ChunkStreaming`, `Renderer` | `ApplicationState` | main + read-only meshing workers |
| `RendererMesh` | streaming/effects | создаётся и рисуется только main; освобождение после GPU fence |
| `ModsState`, `ModHost` | `ApplicationState` | только main |
| `NetworkClient` | `ApplicationState` | client main |
| server world, players, inventory, drops | `DedicatedServer` | server main, fixed tick 60 Гц |

Workers читают `World` и строят CPU-меши, но не вызывают renderer, UI или
моды. Мутации мира выполняются main thread. Нативные моды получают API и
callbacks только на main thread.

## Горячие пути

- Чанк `64³`; greedy meshing использует битовые маски и квады по 8 байт.
- Vertex shader разворачивает квады через `SV_VertexID`; отдельных vertex и
  index buffers нет.
- Меши субаллоцируются из общих DEFAULT-буферов, загрузка идёт через mapped
  upload-ring с бюджетом на кадр.
- Streaming строит ближние чанки раньше дальних, проверяет revision и
  отсекает невидимые меши.
- Дропы и частицы используют фиксированные пулы и instancing по материалу.
- Координаты мира хранятся с origin rebasing; камера использует `double`,
  renderer — значения относительно локального начала.

Статистика без GPU stall доступна по `F3`.

## Контракты и отказы

- Внутренние DLL одной сборки меняются синхронно.
- Стабильность назад обязательна для `sdk/laiue_mod_api.h` и versioned
  форматов на диске.
- Ошибка активного шейдерпака или текстурпака должна дать встроенный fallback,
  а не повредить игровое состояние.
- Сохранения пишутся во временный файл, сбрасываются и атомарно заменяются.
- Сетевой bundle полностью проверяется до установки и проходит через
  `*.download`/`*.previous`.
- В сетевой сессии сервер единолично владеет позицией, блоками, инвентарём и
  дропами; клиент отправляет только intent.

Сетевые границы и ограничения описаны в [multiplayer.md](multiplayer.md),
подключаемое содержимое — в
[content_architecture.md](content_architecture.md).
