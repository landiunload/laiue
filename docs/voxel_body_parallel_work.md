# VoxelBody: вынос инвариантов из цикла свипа по плоскостям

Изменён файл [src/physics/voxel_body.c](../src/physics/voxel_body.c) и добавлены
проверки в [tests/physics_test.c](../tests/physics_test.c) и
[tests/physics_determinism_test.c](../tests/physics_determinism_test.c).
Заголовок, публичный API/ABI, CMake и остальной движок не трогались. Новых
экспортов, глобальных и потоко-локальных кэшей нет, только C17.

Суть: `MoveAxisAgainstBlocks` на каждой проверяемой плоскости звала
`BlockPlaneCollides`, а та заново вычисляла целочисленный диапазон блоков по
**всем трём** осям `newBounds` и только потом подменяла ось движения на
плоскость. Диапазон по двум неподвижным осям не зависит ни от номера
плоскости, ни от того, сколько плоскостей проходит свип, — он считался
заново десятки раз на одну операцию. Диапазон считается один раз, а сканер
среза вынесен в `ScanSolidBlocks`. Плюс динамический путь
`VoxelBodyMoveAxis` переиспользует уже посчитанные `oldBounds` /
`requestedBounds` вместо повторного `CalculateValidBodyBounds` на той же
позиции.

## 1. Стенд

`build/peer/voxel_body_profile.c` (собирается
`build/peer/build_voxel_body_profile.bat`). Исходники физики подключаются в
стенд как единица трансляции напрямую (`#include "physics/voxel_body.c"`), с
теми же флагами, что уходят в `laiue_physics.dll`: `/O2 /fp:strict`. Это даёт
доступ к статической `MoveAxisAgainstBlocks` для замера её отдельно от
`VoxelBodyCollides`.

Мир — заглушка `VoxelCollisionSource`. Режимы:

- **sparse** — пол `z == 0` и решётка стен `z ∈ [1, 3]` каждые 16 блоков;
- **dense** — сплошной массив `z <= 3`.

Тело: `radius = 0.3`, `height = 1.8`, `eyeHeight = 1.75`, `epsilon = 0.001`.
Стенд замеряет:

- `VoxelBodyCollides` — на блуждающей позиции над полом;
- `MoveAxisAgainstBlocks` — свип по одной оси, позиция сбрасывается перед
  каждым вызовом, дистанции от 0.05 до 64 (0…64 плоскости);
- `VoxelBodyMoveAxis` с ненулевым `queryDynamicColliders` — динамический путь.

Заглушка `queryBlockPhysics` считает обращения; проверенные плоскости
считаются по смене координаты оси движения в череде запросов одного вызова
(внутри `BlockPlaneCollides` ось движения зафиксирована, поэтому число
различных значений и есть число срезов). Каждый сценарий — 200 000 вызовов ×
7 раундов, приводится минимум раунда. Машина общая, время шумит, поэтому все
числа — «лучшее из серии», а не среднее.

## 2. Что измерено до правки

`1400000` вызовов на сценарий (200 000 × 7); `ns/call` — минимум из 7
раундов, ниже — минимум по 3 прогонам процесса.

| сценарий | ns/call | queryBlockPhysics / вызов | плоскостей / вызов |
| --- | --- | --- | --- |
| `collides_sparse` | 4,4 | 5,498 | — |
| `collides_dense` | 4,4 | 8,247 | — |
| `move_sparse_x_large` (d = 8) | 19,1 | 16,000 | 8,000 |
| `move_sparse_x_huge` (d = 13) | 28,4 | 26,000 | 13,000 |
| `move_sparse_x_small` (d = 0,05) | 4,3 | 0,000 | 0,000 |
| `move_dense_x_large` (d = 8) | 19,7 | 24,000 | 8,000 |
| `move_dense_x_huge` (d = 64) | 128,2 | 192,000 | 64,000 |
| `move_sparse_z_down` (d = −8) | 10,3 | 4,000 | 4,000 |

Видно, что цена свипа растёт линейно с числом плоскостей, а обращений к
`queryBlockPhysics` на сценарий не меняется. Это и есть цена лишних
`TryFloorToInt64` и повторного построения диапазона.

## 3. Что сделано

1. **Один диапазон на весь свип.** `BlockPlaneCollides` удалена. Её тело
   разбито на `ComputeBlockRange` (границы AABB → целочисленные блоки) и
   `ScanSolidBlocks` (обход `z, y, x` с `IsSolidBlock`). В `MoveAxisAgainstBlocks`
   диапазон `newBounds` считается один раз перед циклом, а в цикле
   переставляется только координата оси движения:

   ```c
   if (!ComputeBlockRange(newBounds, shape, minimumBlock, maximumBlock))
   {
       position[axis] = (double)firstPlane - positiveExtent - epsilon;
       return true;
   }
   for (int64_t plane = firstPlane; plane <= lastPlane; ++plane)
   {
       minimumBlock[axis] = plane;
       maximumBlock[axis] = plane;
       if (ScanSolidBlocks(collision, minimumBlock, maximumBlock)) { ... }
   }
   ```

   Было: `6 × K` вызовов `TryFloorToInt64` на `K` плоскостей (по два на
   ось, для оси движения результат выбрасывался). Стало: `6` на весь свип.

2. **Форма проверяется один раз.** `CalculateValidBodyBounds` разделена на
   валидацию формы (`BodyShapeIsValid`) и `CalculateValidBodyBoundsForShape`.
   В `MoveAxisAgainstBlocks` форма проверяется один раз, а не на каждой из
   двух позиций.

3. **Границы переиспользуются в динамическом пути.** Тело свипа вынесено в
   `MoveAxisAgainstBlocksBounded(position, distance, oldBounds, newBounds)`.
   `VoxelBodyMoveAxis` при наличии `queryDynamicColliders` уже посчитал
   `oldBounds` и `requestedBounds` (а `requestedBounds` — это ровно
   `newBounds`, потому что `requestedPosition = position + distance`), поэтому
   передаёт их напрямую, экономя два полных `CalculateValidBodyBounds` на
   одном и том же входе.

Порядок вещественных операций не менялся: `requestedPosition[axis] += distance`
и `targetPosition[axis] += distance` — одни и те же операнды; финальная
запись `position[axis] += distance` даёт тот же binary64, что и прежний
`position[axis] = targetPosition[axis]`, так как до неё `position[axis]` не
менялась (все ветки столкновения завершают функцию раньше).

### Почему результат тот же

- Результат `TryFloorToInt64` для неподвижных осей не зависит от плоскости,
  а по оси движения его значение затирается плоскостью. Вынос не меняет ни
  одну ветку и ни одну координату.
- Разбиение валидации формы не меняет исход: форма — один и тот же `const`
  указатель, результат `BodyShapeIsValid` на обеих позициях совпадает.
- Переиспользование границ в динамическом пути даёт те же значения, потому
  что это буквально те же `oldBounds` / `requestedBounds`, что посчитала бы
  `CalculateValidBodyBounds` на тех же аргументах.
- Fail-closed ветки сохранены посимвольно, включая запись `position[axis]` при
  отказе перевода в `int64` (сохранены и пустые диапазоны плоскостей, когда
  старая версия вообще не звала `BlockPlaneCollides`).

## 4. Счётчики: точное совпадение

Число обращений к `queryBlockPhysics` и число проверенных плоскостей **не
изменились ни на одном сценарии** ни в одном прогоне:

| сценарий | query до = после | плоскостей до = после |
| --- | --- | --- |
| `collides_sparse` | 5,498 | — |
| `collides_dense` | 8,247 | — |
| `move_sparse_x_large` | 16,000 | 8,000 |
| `move_sparse_x_huge` | 26,000 | 13,000 |
| `move_sparse_x_small` | 0,000 | 0,000 |
| `move_dense_x_large` | 24,000 | 8,000 |
| `move_dense_x_huge` | 192,000 | 64,000 |
| `move_sparse_z_down` | 4,000 | 4,000 |

Это и есть количественное подтверждение, что изменён только способ получить
тот же диапазон, а сама выборка блоков идентична.

## 5. Парные замеры до/после

Минимум раунда, лучший из серии прогонов; до — код `peer/voxelbody` до правки.

| сценарий | до, ns/call | после, ns/call | изменение |
| --- | --- | --- | --- |
| `collides_sparse` | 4,4 | 4,4 | 0 % |
| `collides_dense` | 4,4 | 4,4 | 0 % |
| `move_sparse_x_large` | 19,1 | **11,1** | **−42 %** |
| `move_sparse_x_huge` | 28,4 | **14,9** | **−48 %** |
| `move_sparse_x_small` | 4,3 | 4,1 | −5 % |
| `move_dense_x_large` | 19,7 | **12,1** | **−39 %** |
| `move_dense_x_huge` | 128,2 | **65,9** | **−49 %** |
| `move_sparse_z_down` | 10,3 | **6,7** | **−35 %** |

Разброс по 5 прогонам финального кода: `move_dense_x_huge` 65,9…71,1;
`move_sparse_x_huge` 14,9…15,3; `move_dense_x_large` 12,1…12,8; остальные —
в пределах ±0,4 нс. `collides_*` не затронуты правкой и не изменились.

### 5.1 Переиспользование границ в динамическом пути

Здесь плоскостная оптимизация уже применена, сравнивается только отказ от
повторного `CalculateValidBodyBounds` (старый вызов `MoveAxisAgainstBlocks`
против нового `MoveAxisAgainstBlocksBounded` с готовыми границами), минимум
из серии:

| сценарий | до, ns/call | после, ns/call | изменение |
| --- | --- | --- | --- |
| `move_dyn_dense_x_large` (24 q, 8 плоскостей) | 19,4 | **15,5** | **−20 %** |
| `move_dyn_dense_x_huge` (192 q, 64 плоскости) | 76,0 | **71,4** | −6 % |

Экономия примерно постоянная по абсолютной величине (~4 нс на вызов), поэтому
на дешёвой операции она видна сильнее. Счётчики (24/8 и 192/64) не изменились.

## 6. Проверка

| | значение |
| --- | --- |
| `physics-determinism-hash` | `0x4486debc2d00fab7` |
| `physics-rebased-hash` | `0x4486debc2d00fab7` |

Оба хеша совпали с эталоном до бита. Полный `ctest` — **30/30** в Release и
**30/30** в Debug (`windows-msvc-release`, `windows-msvc-debug`).

Добавлены проверки (без изменения публичного API):

- `tests/physics_test.c`: `TestMultiPlaneSweep` — длинный свободный свип
  (9 блоков) сверяется по битам (`SameBits`), удар в дальнюю стену `x = 40`
  даёт клип `39.699`, длинный свип вниз через пустые плоскости даёт `2.601`,
  и динамический путь с пустой выборкой коллайдеров обязан выдать те же биты,
  что статический. Заглушка расширена опциональной стеной (`useWall`/`wallX`).
- `tests/physics_determinism_test.c`: `TestMultiPlaneSweepExactness` —
  свободный проход `x: 0.5 → 40.5` по битам, удар в стену `x = 101` из-за
  границы чанка даёт `100.699`, свип вниз даёт ровно `2.751`.

### 6.1 Фальсификация

В вынесенный диапазон внесена намеренная поломка: в цикле положительного
свипа плоскость сдвинута на единицу (`minimumBlock[axis] = plane + 1`),
из-за чего проверяется соседний срез, и тело проходит сквозь дальнюю стену.

| поломка | чем поймана |
| --- | --- |
| сдвиг плоскости на +1 | `laiue.physics.voxel_body`: «far multi-plane wall clip is wrong»; `laiue.physics.determinism`: «positive chunk-edge clipping is inaccurate» |

Поломка откачена. Отдельно проверялась поломка сдвига клипа на `epsilon` в
разгрузочной ветке — тоже поймана обеими целями («downward movement was not
clipped at the floor», «negative chunk-edge clipping is inaccurate»). Дыр в
контроле не обнаружено.

## 7. Отклонено

- **Отсев `BlockPlaneCollides` до полного сканирования среза по диагонали.**
  Сканирование уже последовательное по `z, y, x`, и первое попадание
  возвращает `true`; сокращать нечего без изменения порядка обхода (а он
  фиксирует, какой блок считается первым).
- **Слияние двух `BlockPlaneCollides`-проходов в один.** Единственная
  возможная экономия — те самые `TryFloorToInt64`, которые уже устранены
  выносом; сам обход блоков остаётся линейным по плоскостям.
- **Отказ от `BoundsAreValid` / `CollisionMarginIsResolved`.** Не дублируются
  между позициями (значения разные) и нужны для fail-closed контракта —
  не трогалось.

## 8. Ограничения соблюдены

- Изменены только `src/physics/voxel_body.c` и два файла тестов; CMake,
  `rigid_body.c`, `rigid_broadphase.c`, остальной движок не затронуты.
- Публичный API/ABI и экспорты не менялись; `voxel_body.h` не менялся.
- Порядок вещественных операций сохранён (модуль собирается с `/fp:strict`).
- Глобальных и потоко-локальных кэшей нет; SIMD нет; C17.
- Стек функций не вырос: `MoveAxisAgainstBlocksBounded` держит два массива
  `int64_t[3]` в цикле, как и прежняя `BlockPlaneCollides`; предел 4 КиБ не
  задет. Предупреждения — ошибки (`/W4 /WX` у стенда, сборка движка чистая).

## 9. Проверено на

- Windows x64 Release, MSVC 2022 (`windows-msvc-release`), полный CTest 30/30.
- Windows x64 Debug, MSVC 2022 (`windows-msvc-debug`), полный CTest 30/30.
- Стенд `build/peer/voxel_body_profile.exe` (MSVC x64, `/O2 /fp:strict`).
