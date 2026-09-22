# Child AABB BVH составного тела: работа исполнителя

Эта заметка принадлежит ограниченной задаче по ускорению узкой фазы compound-
тел. Зона записи: `src/physics/compound_bvh.c`, `src/physics/compound_bvh.h`,
`tests/compound_bvh_test.c`, `docs/compound_bvh_work.md`. Root владеет
`src/physics/rigid_body.c` и CMake; существующие файлы не тронуты, коммит и
полная сборка не выполнялись (`rigid_body.c` уже содержит вызовы этого API —
их сделал root).

## Что сделано

Новый внутренний модульный контракт без экспортируемого ABI (как
`RigidBroadphaseQuery`): balanced AABB-BVH над детьми одной составной коробки.
Файл не зависит даже от других модулей: только `compound_bvh.h` и стандартные
заголовки. Ни heap, ни OS, ни `<string.h>`, ни глобального состояния —
поэтому Build можно звать из параллельных диапазонов для разных тел.

Точный API (`src/physics/compound_bvh.h`):

```c
typedef struct RigidCompoundBvhNode
{
    double minimum[3];
    double maximum[3];
    uint32_t first;
    uint32_t second;
} RigidCompoundBvhNode;

bool RigidCompoundBvhBuild(const void *bounds, size_t stride, uint32_t count,
                           RigidCompoundBvhNode *nodes, uint32_t nodeCapacity,
                           uint32_t *workspace, uint32_t *outRoot);

bool RigidCompoundBvhQuery(const RigidCompoundBvhNode *nodes, uint32_t root,
                           const double minimum[3], const double maximum[3],
                           uint32_t *outIndices, uint32_t capacity, uint32_t *outCount);
```

`sizeof(RigidCompoundBvhNode) == 56`, выравнивание 8.

### Как это ложится на scratch root

`rigid_body.c` уже резервирует `primitiveCount * 2` узлов и `primitiveCount`
элементов workspace. Вызов

```c
RigidCompoundBvhBuild(children[0].aabbMin, sizeof(*children), shape->boxCount,
                      scratch->compoundNodes + offset * 2u, shape->boxCount * 2u,
                      scratch->compoundWork + offset, &scratch->compoundRoots[index]);
```

удовлетворяет контракту: `aabbMin` лежит в `_Alignas(64)` `RigidBodyCache`
(сразу за ним `aabbMax`, затем padding до stride 256), stride кратен 8,
`nodeCapacity = 2 * count >= 2 * count - 1`. Query во время сбора контактов
пишет в `scratch->compoundWork` уже после барьера Build, поэтому workspace и
`outIndices` не пересекаются с узлами.

## Контракт и валидация Build

- `bounds`, `nodes`, `workspace`, `outRoot` не `NULL`; `count >= 1`.
- `stride >= 6 * sizeof(double)`, кратен `sizeof(double)`; `bounds` выровнен по
  `double`, `nodes` — по `RigidCompoundBvhNode`.
- `nodeCapacity >= 2 * count - 1`; произведение и сумма считаются в `uint64`,
  переполнение `UINT32` не проходит.
- Диапазон `[bounds, bounds + (count - 1) * stride + 48)` адресуем: сначала
  проверяется `(count - 1) * stride`, затем отсутствие переноса адреса; ни одна
  арифметика указателей не строится за пределами объекта.
- Каждый AABB конечен и упорядочен: `minimum[axis] <= maximum[axis]`. NaN и
  ±Inf отклоняются; вырожденный AABB (`minimum == maximum`) допустим.
- Любой отказ происходит **до первой записи**: `nodes`, `workspace` и
  `*outRoot` остаются побитово неизменными. Поэтому проверка полная, а затем
  идёт запись.

Раскладка узлов: лист с исходным индексом `i` лежит в `nodes[i]`
(`first == i`, `second == UINT32_MAX`); внутренние узлы дописываются с индекса
`count`; всего при успехе используется ровно `2 * count - 1` узлов. Внутренний
узел хранит номера детей в `first`/`second`, а его AABB — точное объединение
AABB детей. Пост-обход даёт `first < second` и индекс родителя больше индексов
обоих детей.

## Алгоритм

1. Превалидация указателей, `count`, `stride`, выравнивания, `nodeCapacity` и
   адресуемого диапазона — всё до первой записи.
2. Проверка каждого AABB на конечность и порядок.
3. Заполнение листьев `nodes[0..count-1]` и `workspace[i] = i`.
4. Рекурсивный BuildRange: агрегатный AABB диапазона → самая длинная ось
   (ничья у младшей оси) → heapsort поддиапазона по ключу-центру с тай-брейком
   по исходному индексу → деление по медиане (`middle = begin + size / 2`) →
   пост-обход детей → новый внутренний узел.
5. Центр интервала считается безопасно: при разных знаках `low * 0.5 +
   high * 0.5`, при одинаковых `low + (high - low) * 0.5`; переполнения нет,
   ключи конечны, поэтому порядок — строгий и детерминированный.
6. Сортировка — собственная heapsort без выделения памяти. Глубина рекурсии
   Build не превышает `floor(log2(count)) + 1 <= 31`.

## Контракт Query

- `nodes`, `minimum`, `maximum`, `outIndices`, `outCount` не `NULL`;
  `root != UINT32_MAX`; запрос конечен и упорядочен.
- Отказ поддерева ровно как первая строка `BuildBoxManifold`: если по любой
  оси `node.maximum <= query.minimum` или `query.maximum <= node.minimum`,
  касание не считается пересечением. Иначе спуск; на листе пишется исходный
  индекс.
- Обход идёт явным стеком на 32 кадра (сбалансированное дерево из Build не
  глубже 31); рекурсии и большого стека нет.
- Попадания возвращаются **строго по возрастанию исходного индекса** — это
  сохраняет прежний порядок `(firstChild, secondChild)`, `ContactRunEnd`,
  warm start и colored solver побитово. Для этого список сортируется repeats-
  `heapsort` перед записью `*outCount`.
- Если попаданий больше `capacity`, функция возвращает `false`, не переполняя
  буфер и **не трогая `*outCount`**. На успехе `*outCount` — число попаданий.

## Что проверяет тест

`tests/compound_bvh_test.c` — standalone no-CRT тест, все буферы static
(≈1.5 МиБ BSS), общий entry `LAIUE_TEST_ENTRY`, кадр стека не растёт:

- count 1: лист-корень, точные границы, попадание и промах;
- отказ Build: `NULL`-аргументы, `count == 0`, `stride` короче 48, `stride` не
  кратен 8, `nodeCapacity < 2*count-1`, `count == UINT32_MAX` с переполнением;
  при отказе `*outRoot`, узлы и workspace неизменны (канарейки);
- нефинитные и инвертированные AABB (NaN, ±Inf, `min > max`) отклоняются без
  записи;
- отказ Query: `NULL`-аргументы, отсутствующий root, NaN/инвертированный
  запрос, нехватка capacity (с неизменным `*outCount`), точная capacity;
- строгое исключение касания: общая грань не считается, лёгкое перекрытие
  считается; вырожденная плита нулевой толщины не задевается при касании и
  задевается только при пересечении насквозь;
- огромные конечные координаты (`±DBL_MAX`, вырожденный AABB) строятся и
  запрашиваются; NaN/Inf отклоняются;
- strided вход (stride 80 с padding) даёт побитово то же дерево и те же
  результаты Query, что плотный вход (stride 48);
- детерминизм дерева: два Build на 512 коробках совпадают покомпонентно и
  бит-в-бит;
- 4096 случайных коробок против собственного brute force на 200 случайных
  запросах (совпадение счёта и набора), плюс полное покрытие и явная проверка
  возрастающего порядка исходных индексов;
- минимальная вместимость `2*count-1` с канарейкой за последним используемым
  узлом, канарейка workspace за `count`, capacity ровно `hits` и `hits-1`.

## Проверено

- `clang -fsyntax-only -std=c17 -Wall -Wextra -Werror -I src` для
  `compound_bvh.c` (и с `-I tests` для `compound_bvh_test.c`) — без
  предупреждений.
- Ad-hoc (вне source tree, во временном каталоге) `cl /std:c17 /W4 /WX`
  собрал `compound_bvh.c` + `compound_bvh_test.c` + раннер, тест прошёл:
  `compound-bvh: ok`, exit 0. Это разовая проверка логики, а не сборка
  проекта: CMake/CTest и no-CRT импорты — зона root.

## Что должен сделать root

1. Добавить в `src/physics/CMakeLists.txt` в `SOURCES`:

   ```cmake
   compound_bvh.c
   compound_bvh.h
   ```

   Новых зависимостей не нужно: файл не включает ничего, кроме собственного
   заголовка и стандартных заголовков.

2. Зарегистрировать тест в `tests/CMakeLists.txt`. Так как BVH — внутренний
   символ, а модули по умолчанию `SHARED` с hidden-видимостью, тест должен
   компилировать `compound_bvh.c` прямо в себя (как
   `laiue_texture_pack_build_test`), а не линковать `laiue::physics`:

   ```cmake
   add_executable(laiue_compound_bvh_test
       compound_bvh_test.c
       "${PROJECT_SOURCE_DIR}/src/physics/compound_bvh.c")
   target_include_directories(laiue_compound_bvh_test PRIVATE
       "${PROJECT_SOURCE_DIR}/src")
   laiue_configure_standalone_executable(
       laiue_compound_bvh_test CompoundBvhTestEntryPoint)
   add_test(NAME laiue.physics.compound_bvh COMMAND laiue_compound_bvh_test)
   set_tests_properties(laiue.physics.compound_bvh PROPERTIES TIMEOUT 30)
   ```

   `laiue_configure_standalone_executable` сам подключает `laiue_common` и
   `laiue::platform_support`; тест не использует `memcpy`, поэтому no-CRT
   закрывается штатным `laiue_runtime`.

3. Сверить, что `RigidCompoundBvhBuild/Query` в `rigid_body.c` совпадают с
   заголовком (они уже совпадают) и что scratch-раскладка резервирует
   `2 * count` узлов и `count` workspace на тело — она уже это делает.

## Ограничения

- Дерево строится заново на каждый тик: persistent-кэш не добавляется, как и
  просилось. Это O(n log^2 n) на тело, но count детей ограничен scratch.
- Точная вместимость узлов — `2 * count - 1`; root передаёт `2 * count`, что
  допустимо и оставляет один неиспользуемый узел на тело.
- Сравнения Query не зависят от формы дерева: набор листьев совпадает с
  brute force при любой корректной раскладке, поэтому hostile FP меняет лишь
  форму, но не результат. Тай-брейк и heapsort всё равно фиксируют дерево
  детерминированно.
- Полная сборка, CTest, no-CRT импорты и линковка в модуль не выполнялись:
  это зона root.
