# Архитектура laiue

## Границы модулей

- `core/application.c` — композиция приложения и жизненный цикл.
- `core/application_config.*` — единый источник настроек.
- `gameplay/player_controller.*` — orchestration fixed-step физики игрока.
- `gameplay/player_locomotion.*` — горизонтальная скорость, ускорение,
  торможение, air-control и sprint-jump impulse.
- `gameplay/player_jump.*` — вертикальный прыжок, buffer/coyote time.
- `gameplay/player_stance.*` — стойка и безопасная анимация приседания.
- `interaction/voxel_interaction.*` — трассировка луча и формирование команды редактирования мира.
- `core/debug_overlay.*` — только форматирование диагностического текста.
- `core/chunk_streaming.*` — асинхронная загрузка и жизненный цикл мешей.
- `world/*` — процедурный мир, изменения и свойства типов блоков.
- `physics/voxel_body.*` — геометрия AABB, столкновения и контакт с
  поверхностью без зависимости от конкретного мира.
- `render/*` — GPU и отрисовка без игровой логики.

## SOLID в C

- **SRP:** каждая подсистема имеет одну причину для изменения.
- **OCP:** внешние толчки добавляются через `PlayerControllerApplyImpulse`, не меняя ввод и коллизии.
- **LSP:** наследование не используется; контракты функций не требуют подмены типов.
- **ISP:** команды игрока, воксельное редактирование и overlay имеют отдельные узкие интерфейсы.
- **DIP:** контроллер получает `PlayerControllerCommand` и абстракцию
  `PlayerCollisionSource`; generic callback возвращает solidity/friction и не
  раскрывает `World` или `BlockType` модулям physics/gameplay.

`game_events.h` оставлен как точка будущего расширения, но API событий не придуман заранее: он появится только при наличии нескольких реальных производителей и потребителей.
