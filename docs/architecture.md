# Архитектура laiue

## Границы модулей

- `launcher` — минимальный EXE: загружает `laiue_core.dll` и передаёт ему
  управление.
- `platform` — окно Win32 и монотонное время; `input` — raw input.
- `core/application.c` — composition root, цикл кадров и жизненный цикл.
- `core/application_config.*` — единый источник настроек.
- `game/camera.h` — нейтральная структура состояния камеры; операции и
  матрицы находятся в `core/camera.*`, потому что являются сервисом ядра.
- `core/save_game.*` — слоты миров, метаданные, игрок и атомарные сохранения.
- `mod/mod_host.*`, `mod/mods.*` — отдельная DLL загрузки нативных модов,
  стороны client/server и канонический набор сетевой совместимости.
- `content/*` — каталог форматов и bounded bundle со staging-установкой.
- `network/*` — bounded wire protocol, схемы сетевых игровых сообщений,
  соединения, очереди и transport; не зависит от реализаций мира, физики,
  renderer или UI.
- `server/main.c` — отдельный headless composition root: authoritative мир,
  игроки, fixed tick и семантическая проверка команд.
- `gameplay/player_controller.*` — orchestration fixed-step физики игрока.
- `gameplay/player_locomotion.*` — горизонтальная скорость, ускорение,
  торможение, air-control и sprint-jump impulse.
- `gameplay/player_jump.*` — вертикальный прыжок, buffer/coyote time.
- `gameplay/player_stance.*` — стойка и безопасная анимация приседания.
- `gameplay/inventory.*` — 36 слотов, stacking/consume/select без heap и
  без зависимости от мира или UI.
- `interaction/voxel_interaction.*` — трассировка луча и формирование команды редактирования мира.
- `core/game_hud.*`, `core/ui*`, `core/pause_menu.*`, `core/server_list.*` —
  HUD, UI-примитивы, главное/игровое меню и локальный список серверов;
  GPU-реализация UI остаётся в `render`.
- `core/chunk_streaming.*` — асинхронная загрузка и жизненный цикл мешей.
- `core/block_effects.*`, `core/inventory_ui.*` — дропы, частицы, подбор и
  UI поверх generic gameplay/render API.
- `world/*` — процедурный мир, изменения и свойства типов блоков.
- `physics/voxel_body.*` — геометрия AABB, столкновения и контакт с
  поверхностью без зависимости от конкретного мира.
- `render/*` — GPU и отрисовка без игровой логики.

Направление зависимостей задаётся в `src/*/CMakeLists.txt`: `core`
компонует подсистемы; `render` зависит только от `content`; `mesher` —
от `world`; `interaction` — от `world` и `physics`; `gameplay` — от
`physics`; `network` не зависит от игровых модулей; server компонует
`content`, `mod`, `network`, `world`, `physics`, `gameplay` и `interaction`.
Нижние модули не
должны включать заголовки `core`.

DLL-границы являются частью архитектуры. Крупную реализацию можно делить
на несколько `.c` внутри той же DLL, но объединять подсистемы или передавать
приватные структуры через ABI нельзя.

Допустимые направления include-зависимостей зафиксированы в
`tools/check_architecture.ps1`. Проверка запускается корневой целью `laiue`
и запрещает обратные зависимости на `core`, неизвестные модули и `../` в
include. Связь должна быть одновременно видна в CMake и в исходниках.

## Когда нужна новая DLL

Новая DLL оправдана, только если выполняются все условия:

1. подсистема имеет собственное состояние и жизненный цикл;
2. её контракт можно выразить узким C API с непрозрачным handle;
3. существует минимум один реальный потребитель помимо composition root;
4. граница не требует множества мелких вызовов на каждый блок или квад;
5. отказ подсистемы можно обработать на её границе.

Иначе добавляется новый `.c/.h` внутри существующей DLL. Поэтому камера,
HUD, меню, сохранения и стриминг остаются внутренними частями `core`, а
GPU-проходы — внутренними частями `render`.

## Владение и потоки

| Объект | Владелец | Поток | Освобождение |
|---|---|---|---|
| `World` | `ApplicationState` | главный + shared-read mesher | после остановки streaming |
| `ChunkStreaming` | `ApplicationState` | планирование — главный, meshing — workers | останавливает workers и освобождает результаты |
| `Renderer` и `RendererMesh` | `ApplicationState` / streaming | только главный | диапазоны мешей откладываются до GPU fence |
| `ModsState`, `ModHost` | `ApplicationState` | только главный | хуки останавливаются до сохранения и выгрузки DLL |
| списки content | вызывающая сторона | только главный | парный `*ListRelease` |
| server content bundle | `DedicatedServer` / client installer | server/client main | после network / после staging-установки |
| байткоды/текстуры паков | вызывающая сторона загрузчика | только главный | освобождение после копирования renderer'ом |
| `NetworkClient` | client `ApplicationState` | главный | до разрушения client world |
| `NetworkServer` | `DedicatedServer` | server main thread | до server world |
| server `World`/players | `DedicatedServer` | server main thread, 60 Гц | после остановки listener |

В титульном состоянии `World`, `ChunkStreaming` и `ModHost` равны `NULL`:
`RendererCreate` поднимает только shell/UI, `RendererPrepareWorld` — игровые
PSO, depth, block textures и upload rings, `RendererReleaseWorld` освобождает
их после остановки streaming. Статичный фон меню поэтому не скрывает
работающий мир и не оставляет фоновых meshing/physics задач.

Рабочие потоки не вызывают renderer, UI или моды. Нативные моды получают
хуки только главного потока. Изменение `World` выполняется главным потоком;
mesher держит только shared-read на время чтения.

В сетевой сессии server — единственный владелец истинного игрового состояния.
Клиентское движение является prediction и исправляется snapshot; client intent
не является командой установить позицию или блок. Подробный threat model и
этапы remote transport находятся в [multiplayer.md](multiplayer.md).

## Ошибки и fallback

- Ошибка пользовательского содержимого не превращается в ошибку GPU или
  падение: content проверяет имя/путь, загрузчик проверяет формат, renderer
  публикует обобщённый `RendererContentStatus`, core показывает его в UI.
- Публичный заголовок DLL не раскрывает приватную раскладку загрузчика:
  структуры LTP находятся в `texture_pack_internal.h`.
- Сохранения записываются атомарно; старый файл заменяется только после
  полной записи и `FlushFileBuffers`.
- Загруженное содержимое сначала целиком проверяется (границы, пути, SHA-256
  сетевого потока), распаковывается в `*.download`, а предыдущая версия
  сохраняется как `*.previous` до атомарного rename.
- Между внутренними DLL одной сборки ABI может развиваться синхронно.
  Стабильность назад обязательна только для `sdk/laiue_mod_api.h` и форматов
  на диске.

Публичные контракты модов находятся в `sdk/laiue_mod_api.h`. Изменения
этого заголовка должны быть обратно совместимыми в пределах версии API:
новые поля добавляются в хвост таблиц с проверкой `sizeBytes`.

## SOLID в C

- **SRP:** каждая подсистема имеет одну причину для изменения.
- **OCP:** внешние толчки добавляются через `PlayerControllerApplyImpulse`, не меняя ввод и коллизии.
- **LSP:** наследование не используется; контракты функций не требуют подмены типов.
- **ISP:** команды игрока, воксельное редактирование и overlay имеют отдельные узкие интерфейсы.
- **DIP:** контроллер получает `PlayerControllerCommand` и абстракцию
  `PlayerCollisionSource`; generic callback возвращает solidity/friction и не
  раскрывает `World` или `BlockType` модулям physics/gameplay.

`game_events.h` остаётся узким набором игровых событий. Расширять его
следует только под конкретные пары производителей и потребителей.
