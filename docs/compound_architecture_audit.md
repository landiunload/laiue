# Аудит архитектуры compound-пути rigid body

Read-only разбор по запросу root. Никакой код, сборка, замер, коммит и другие
файлы в этой задаче не менялись; создан только этот документ. Все выводы ниже
явно помечены как **измерено** (взято из уже лежащих в репозитории замеров,
не из этой сессии) или **выведено** (следствие кода, без профиля). Профиля
именно сцены 1000 × Г(9) не существует.

## 0. Что именно разбирается

- Движок: `C:/Users/landi/projects/laiue/src/physics/rigid_body.c`
  (6315 строк), call sites — [rigid_body.h](../src/physics/rigid_body.h).
- Игра: `C:/Users/landi/projects/simulation-of-sins`,
  `src/game/construct.c`, `src/game/construct_spawner.c`,
  `src/game/falling_cubes.c`, `src/app/application.c`.
- Сцена: 1000 тел-«Г» по 9 unit-детей, `pitch = 8`, 10×10, 10 слоёв,
  128 Гц, цветной решатель, executor, статичный пол. Сообщённый root
  порядок величины — **~39–40 мс/тик**; это не проверенное здесь число.

Все старые правки (compound API, compact grid, body/world contacts,
warm start, parallel solver) считаются намеренными и не трогаются.

## 1. Честная граница «измерено / выведено»

**Измерено ранее** (документы репозитория, не этот аудит):

- Плоская куча 4096 *обычных* тел: `body_contacts` — 39–41 % шага, из них
  запрос дерева 41–46 %, SAT 35–38 %, канонизация кандидатов 14 %
  ([body_contacts_parallel_work.md](body_contacts_parallel_work.md),
  [narrowphase_parallel_work.md](narrowphase_parallel_work.md)).
- 4096 обычных тел: ~42 021 обращений `queryBlockPhysics` за шаг, ~10.26 на
  тело; сплошное ядро нашлось лишь у 258 тел, т.е. 93 % обращений — плановый
  обход пустого ядра ([world_contacts_parallel_work.md](world_contacts_parallel_work.md)).
- Попарный `queryBlockPhysics`-кэш на 64 записи дал −41 % обращений, но
  `world_contacts` **вырос** (0.41 → 0.54 мс) на тривиальном стенде; откачен.
- Реальный прогон игры с кэшем импульсов: 4179 тел, 3199 бодрствовали,
  13 314 контактов, 12 397 переиспользованных импульсов, 24.50 мс/тик на
  железе того прогона ([rigid_physics.md](rigid_physics.md) §«Замеры»).

**Не измерено:** stage-профиль сцены Г(9), число кандидатов/контактов,
`queryBlockPhysics` на этого ребёнка, доля сна, вклад `wake`, `body_contacts`
против `world_contacts` именно здесь. Все проценты ниже — **выведены**.

Выводы о том, что compound-стадия вообще может быть дорогой, опираются на
размер работы в коде: для пары составных тел перебирается `9×9 = 81`
child-пара, а обычная пара — 1 SAT.

## 2. Карта текущего compound-пути

Порядок стадий — `RigidBodyStepInternal` (`rigid_body.c:5982`):

| стадия | функция | строка |
| --- | --- | --- |
| order | `BuildStableOrder` | 1376 |
| forces (гравитация) | inline | 6174 |
| bounds | `BuildCacheRange` → `BuildCache` | 5434 / 1809 |
| broadphase | `BuildBroadphase` | 3122 |
| wake | `WakeContactIslands` | 3394 |
| world contacts | `CollectWorldContacts` | 3019 |
| body contacts | `CollectBodyContacts` | 4114 |
| solve (parallel colored) | `SolveContacts` | 5641 |
| integrate | `IntegrateRange` | 5843 |
| sleep | `UpdateSleepIslands` | 5867 |

Ключевые факты о compound:

1. **Флаг сцены.** `CompoundSceneDetect` (1601) выставляет `compoundScene`,
   и `CollectBodyContacts` (4121) полностью отключает параллельные
   grid/tree-коллекторы: `parallel = !compoundScene && ...`. Мир-контакты
   (`CollectWorldContacts`, 3019) последовательны всегда. Итог: при 6
   рабочих потоках узкая фаза пар и мир идут **в один поток**; параллельны
   только подготовка, warm start, окрашенный решатель и интеграция.
2. **Bounds строит только родителя.** `BuildCache` (1809) считает
   `columns`, `position`, AABB и `radius` тела по envelope. Дочерняя
   геометрия — `CompoundChildCache` (1163) — считается **по требованию**,
   вне bounds.
3. **Body narrowphase.** `AppendCompoundPairContacts` (3553): внешний цикл по
   `firstChild`, внутренний по `secondChild`; внутри второго цикла
   `CompoundChildCache(secondCache, ...)` (3586) пересчитывается для одного и
   того же ребёнка `firstCount` раз. Для пары Г×Г это 81 вызов
   `CompoundChildCache` + 81 `BuildBoxManifold` (3584–3591). Пара
   повторяется для каждого кандидата тела; при ~7 кандидатах на тело за тик
   один и тот же ребёнок пересчитывается десятки раз.
4. **Wake.** `WakePairTouches` (3293) для составной пары — тот же двойной
   цикл 9×9 полного SAT (3308–3334), тоже с `CompoundChildCache` внутри.
5. **World narrowphase.** `CollectShapeWorldContacts` (2936) сэмплирует
   **родительский** envelope (`cache->aabbMin/Max`), затем
   `CollectSampleContacts` (2803) для каждой слитой коробки мира
   перебирает **все** дети (2913–2927) и вызывает `BuildBlockManifold`
   (2593). Ранний отсев внутри `BuildBlockManifold` есть только по
   world-осям (2618–2627) и body-осям (2608–2617) — но он уже после захода в
   функцию и пересчёта `BoxRadius`.
6. **Cache.** `VoxelRigidContactCacheReset` (1682) чистит весь кэш.
   Игра зовёт `SimulationCubeFieldInvalidateSolver`
   (`falling_cubes.c:438`) **после каждого** `ConstructSpawn`
   (`construct.c:850`), т.е. примерно раз в 1.28 тика в фазе спавна
   (`SIMULATION_CUBE_SPAWNS_PER_SECOND = 100`, тиков 128).
7. **Sleep.** `UpdateSleepIslands` (5867) — по контактным островам; поддержка
   определяется `hasWorldContact` тела, который ставит `AppendWorldManifold`
   (2757) по индексу родителя. Сцена с хотя бы одним бодрым телом всё равно
   проходит `BuildCacheRange` по **всем** активным (6197–6198) и
   `BuildBroadphase` по всем (3129–3143); компактного активного набора нет.

## 3. Где именно теряется работа

### 3.1. Подготовка child-трансформа, повторяемая на каждого кандидата — выведено

`CompoundChildCache` (1163) для одного ребёнка делает 9 копий `columns`,
9 умножений на `box->center`, 9 `AxisAligned`-радиусов. Для одного тела Г:
9 детей × стоимость; но вызов происходит:

- в body narrowphase — для каждого `(first,second)` кандидата
  (**не** между тиками, а внутри цикла);
- в `WakePairTouches` — для каждого спящего кандидата;
- в world narrowphase — для каждой слитой коробки и каждого ребёнка.

Итог по порядку: `O(candidates × children²)` вызовов на тело за тик вместо
`O(children)`.

### 3.2. Декартовы child-циклы без отсева по AABB — выведено

81 SAT на пару Г×Г. `BuildBoxManifold` (2080) отвергает несовпавшие AABB
первой же проверкой (2084–2091), но:
- второй ребёнок уже подготовлен `CompoundChildCache` (3586) до этой
  проверки;
- у `BuildBlockManifold` (2593) нет аналогичной предварительной проверки
  child-AABB против block-AABB до захода, а именно она — его первая
  world-осевая проверка (2622).

### 3.3. Мир опрашивается по родительскому envelope — выведено

Родительский envelope «Г» больше объединения детей, поэтому выборка
накрывает лишние клетки, а каждая слитая коробка гоняет все 9 детей. Это
ровно тот случай, о котором предупреждает
[world_contacts_parallel_work.md](world_contacts_parallel_work.md): сокращать
**число** клеток без хранения между телами нельзя, но удешевить разбор и
отсеять заведомо далёких детей можно, не меняя результат.

### 3.4. Узкая фаза compound последовательна — измерено по коду

См. §2.1. При 6 потоках это простаивающие потоки на самой тяжёлой стадии,
если она действительно тяжёлая.

### 3.5. Кэш импульсов обнуляется на каждом спавне — измерено по коду

См. §2.6. В фазе спавна warm start фактически не работает.

### 3.6. Нет компактного активного набора — выведено

Уже зафиксировано как следующий этап в
[rigid_physics.md](rigid_physics.md) §«Следующие этапы», п. 2. Для сцены
1000 одновременно бодрствующих тел выигрыша не даст.

## 4. Первичные источники: как это делают движки

- **Jolt.** У compound-формы дети — это `SubShape` с собственным локальным
  трансформом; есть `GetIntersectingSubShapes(AABox/…, …)`, которым
  коллизионный visitor отсеивает детей по коробке до точной проверки.
  <https://jrouwe.github.io/JoltPhysics/class_compound_shape.html>,
  <https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Collision/Shape/CompoundShape.h>
  (`GetIntersectingSubShapes`, `SubShape::mPosition/mRotation`, `mLocalBounds`).
- **Bullet.** У `btCompoundShape` есть *необязательное* динамическое
  AABB-дерево по детям «to accelerate early rejection tests»;
  `getAabb` — brute-force по AABB детей; локальный AABB — объединение.
  <https://github.com/bulletphysics/bullet3/blob/master/src/BulletCollision/CollisionShapes/btCompoundShape.h>,
  <https://github.com/bulletphysics/bullet3/blob/master/src/BulletCollision/CollisionShapes/btCompoundShape.cpp>
  (`createAabbTreeFromChildren`, `getAabb`, `recalculateLocalAabb`).
- **Box2D v3.** Каждая фигура получает собственный broadphase-прокси;
  фигуры одного тела никогда не сталкиваются между собой; AABB считается по
  фигуре под трансформом тела.
  <https://box2d.org/documentation/md_simulation.html>,
  <https://box2d.org/documentation/md__d_1__git_hub_box2d_docs_collision>.
- **Острова/параллелизм.** Islands Box2D и Jolt LargeIslandSplitter —
  <https://box2d.org/posts/2023/10/simulation-islands/>,
  <https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/LargeIslandSplitter.cpp>.

Разница с laiue: движок намеренно держит **один grid-прокси на тело** по
envelope и контракт побитового порядка контактов, поэтому вводить прокси на
ребёнка (как Box2D) — большое изменение ABI и порядка. Bullet-идея
«раннего отсева по AABB детей, посчитанных один раз» даёт тот же эффект без
смены порядка. Это и есть основа P1.

## 5. Приоритизированные проекты

### P1 (рекомендуется первым). Дочерние мировые AABB за тик + консервативный отсев

**Что.** В bounds-стадии один раз на бодрое collidable-тело вычислить
мировые AABB всех его детей (тот же `CompoundChildCache`, но однократно) и
положить их в scratch. Далее использовать:

- в `AppendCompoundPairContacts` (3553): до SAT отсеивать child-пару, если
  её AABB разъединены; отсев бит-в-бит равен первой проверке
  `BuildBoxManifold` (2084);
- в `WakePairTouches` (3293): то же;
- в `CollectSampleContacts` (2913): перед `BuildBlockManifold` отсеивать
  ребёнка, если его AABB не пересекает AABB слитой коробки; это ровно
  world-осевая проверка `BuildBlockManifold` (2622);
- при желании — необязательный ранний выход пары, если все детские AABB
  разъединены (счётчик `candidatePairCount` — диагностический, не replay).

Отдельная проверка «ребёнок против родителя» не нужна: контракт шага уже
требует, чтобы `body->halfExtent` покрывал каждого ребёнка в координатах тела
(`CompoundShapesValid`, 1558–1566), поэтому ребёнок заведомо внутри envelope.
Отсев нужен ровно там, где пересекаются **разные** тела/мир.

Опционально хранить не только AABB (48 Б), а полный child-cache
(`columns`+`position`+AABB, ~152 Б), тогда после отсева не нужен повторный
`CompoundChildCache` в SAT. Хранить по одному слоту на ребёнка (не на
primitive).

**Ожидаемый эффект — выведено.** Убирает `O(candidates × children²)`
подготовок и оставляет `O(children)` за тик; 81 SAT на пару превращаются в
81 сравнение AABB, и только пересекающиеся идут в SAT. Поскольку
`body_contacts` на плоских телах уже 39–41 % шага, на Г(9) эта стадия с
большой вероятностью главная. Точную долю должен дать профиль (см. §6).

**Риск: очень низкий.** Отсев только удаляет заведомо пустые child-пары;
условия совпадают с уже существующими необходимыми проверками, арифметика
SAT/решателя и порядок run-ов `(firstChild, secondChild, point)` не
меняются. Это и есть «первое изменение с побитово тем же порядком/арифметикой».

**Тесты same-output.**
- `tests/rigid_compound_test.c`: `TestCompoundReplayDeterminism` и
  `ReplayCompound()` (`rigid-compound-replay-hash`) — хеш обязан совпасть;
  `TestCompoundCacheMatchesLegacyCache`, `TestCompoundRestsOnFloor`,
  L9-в-щели, `BuildReplayScene` L9 — поведение не меняется.
- `RunExecutorHash` (serial == split == workers) — обязателен.
- `tests/rigid_body_test.c`: plain-box replay `0x493245ececa466b1` не
  меняется (P1 не трогает box-only путь).
- Новый тест эквивалентности отсева: на фиксированной сцене с Г(9)-парами
  сравнить `contactCount`, `candidatePairCount` (диагностика) и хеш тел с
  эталоном; и свойство «AABB-разъединённый child-пара не даёт точек» —
  brute-force по случайным локальным позам.
- `physics_determinism_test` reference hash `0x4486debc2d00fab7` — не
  меняется (другой контроллер, но проверяет общий rebuild).

**Scratch/ABI.** Новый регион в `RigidStepScratch` (1295) и
`RigidStepScratchBytesFor` (1738). Чтобы legacy-размер остался
байт-в-байт, выделять регион только при `primitiveCount > bodyCount`
(compound-сцена). Верхняя граница числа детей, выводимая из `primitiveCount`,
безопасна: каждый ребёнок даёт минимум один primitive. Публичная структура и
версия не меняются; для compound-вызовов scratch становится больше — игра
уже запрашивает размер через `VoxelRigidBodyStepCompoundScratchBytes` и
перевыделяет (`falling_cubes.c:559`), так что правок call-site не требует.
Память: на сцену 1000×9 при полном child-cache ~1.4 МБ к текущему scratch
(~63 МБ, из которых ~50 МБ — предвыделенный `solverContacts`), т.е. около
двух процентов.

### P2. Инкрементальная инвалидация contact-cache при спавне

**Что.** Не сбрасывать весь кэш на добавление тела. Кэш ключуется парой
`stableId` и локальными якорями, поэтому append не делает старые записи
невалидными. Достаточно убрать `VoxelRigidContactCacheReset` из ветки
`ConstructSpawn` → `SimulationCubeFieldInvalidateSolver`
(`construct.c:850`, `falling_cubes.c:438`) и оставить сброс на удаление
(`SimulationCubeFieldRemoveBody:666`), rebase и правку мира
(`application.c:1273`). При росте `bodyCapacity` кэш всё равно инициализируется
заново (`falling_cubes.c:625`), что сохраняет детерминированный график
ёмкости. Если нужен точечный инвалид для телепорта/rebuild одного тела —
добавить additive `VoxelRigidContactCacheInvalidateBody(cache, stableId)`.

**Ожидаемый эффект — выведено, но узкий.** В фазе спавна (~1280 тиков) warm
start перестанет теряться почти каждый тик; после 1000-го спавна
`InvalidateSolver` больше не вызывается, поэтому на измеряемые ~39–40 мс это
напрямую не влияет. Выигрыш — устойчивость кучи во время роста и меньше
работы warm-start на этих тиках.

**Риск: средний (replay).** График сброса кэша — часть replay-контракта
(комментарий `VoxelRigidBodyStepCached` в `src/physics/rigid_body.h:273–289` и
[rigid_physics.md](rigid_physics.md) §«Реализованный кэш импульсов»).
Изменение расписания надо принять как осознанную смену replay-конфигурации
игры; эталонные хеши движка (у них своё расписание) не затрагиваются.

**Тесты same-output.**
- Игровой replay по тикам: одинаковый seed → повторяемый хеш после смены
  расписания; сравнение с прежним поведением на тиках без спавна.
- Новый тест: append тела не меняет `matchedContactCount` и якоря уже
  существующих пар (счётчик совпадений до/после append на неподвижной сцене).
- `tests/rigid_cache_test.c`: все существующие хеши (`0x58c622…`) не
  меняются — движковый кэш не трогается.

**Scratch/ABI.** Нет для game-only варианта; для (b) — additive публичная
функция без изменения структур.

### P3. Параллельная body-narrowphase для compound

**Что.** Снять запрет `!scratch->compoundScene` (`CollectBodyContacts:4121`)
для **body**-контактов: в `GridNarrowphaseRange` (3674) и
`TreeNarrowphaseRange` (3899) для пары, где есть составное тело, выполнять
точный `firstChild × secondChild` в том же фиксированном порядке и считать
все точки в per-body счётчик. Двухпроходная префиксная сумма уже даёт
канонический порядок по `scratch->order`; `JoinContactIsland`/`hasBodyContact`
остаются последовательными после барьера (3872–3883). Мир-контакты остаются
последовательными: `queryBlockPhysics` по контракту зовётся на вызывающем
потоке.

**Ожидаемый эффект — выведено.** До `min(threads, доля body_contacts)`
ускорения этой стадии при 6 потоках; суммарный выигрыш зависит от того,
доминирует ли она. Сначала измерить.

**Риск: высокий по порядку/битам.** Нужно воспроизвести порядок контактов
`AppendCompoundPairContacts` точка-в-точку, включая min restitution/friction
и тот факт, что `RIGID_NARROWPHASE_POINTS_PER_BODY = 8` меньше числа точек
пары (9×4): второй проход и так пересчитывает весь список при >8, поэтому
эквивалентность достижима, но возможна двойная работа. Если P1 уже отсеял
пары, пересчёт дешёвый.

**Тесты same-output.** Уже существующий `rigid_compound_test::RunExecutorHash`
(serial == split == workers) — главный предохранитель; плюс
`TestCompoundReplayDeterminism`, `rigid-parallel-*-replay-hash`, и проверка
`candidatePairCount`/`contactCount` независимо от числа worker.

**Scratch/ABI.** Если переиспользовать child-cache из P1 — без изменений;
иначе может понадобиться расширить `narrowphasePoints` (1272) на
`primitiveCount`.

### P4 (отложенно). Компактный активный/спящий набор

**Что.** Постоянный список бодрых тел/островов, пропуск
`BuildCache`/`BuildBroadphase` для спящих с валидным AABB. Уже заявлено
следующим этапом в [rigid_physics.md](rigid_physics.md) §5.2.

**Ожидаемый эффект — выведено.** Только для сцен с большой долей спящих; в
измеряемой сцене 1000 одновременно бодрствующих выигрыша почти не даст.

**Риск: наивысший.** Устаревание кэша при rebase/правке мира/телепорте,
сохранение честного пробуждения (никакого «fake sleep»). Не делать до P1/P3
и до замера доли сна.

**Тесты.** Пробуждение одного тела в осевшей сцене даёт тот же набор
контактов, что и полный проход; rebase; правка мира; неизменность порогов
сна. Ссылки: [simulation-islands](https://box2d.org/posts/2023/10/simulation-islands/),
[LargeIslandSplitter](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/LargeIslandSplitter.cpp).

**Scratch/ABI.** Новые массивы по `bodyCount`; публичных структур не трогает.

## 6. Порядок и что мерить до правок

Root владеет изменениями и замерами. Предлагаемая последовательность:

1. Снять stage-профиль именно сцены 1000×Г по существующему
   `VoxelRigidStepProfile` ([rigid_body.h:138](../src/physics/rigid_body.h))
   и детерминированные счётчики: `activeBodyCount`, `awakeBodyCount`,
   `candidatePairCount`, `contactCount`, `queryBlockPhysics/тик`,
   число `CompoundChildCache` и `BuildBoxManifold` вызовов за тик.
   Существующий стенд `step_profile` из прошлых задач подходит; профиль
   должен быть на той же машине, чередующимися прогонами.
2. Если `body_contacts` доминирует — внедрять **P1**; он же снимет часть
   `world_contacts`.
3. P2 можно делать независимо и параллельно P1 (разные владельцы: игра vs
   движок).
4. P3 только после P1 и только если `body_contacts` остаётся бутылочным
   горлом; иначе потоки всё равно простаивают.
5. P4 — только если профиль покажет большую долю спящих.

## 7. Анти-цели (не предлагаются)

Замена детей одним объемлющим боксом; «fake sleep» и заморозка во сне;
отбрасывание контактов; увеличение `1/128` или уменьшение
`solverIterations`; `fastmath`/FMA; вынос физики в рендер. Все они меняют
физический результат либо нарушают replay-контракт и здесь не
рассматриваются. Памятка о том, почему «дырка» L/Г не заполняется, —
[compound_engine_work.md](compound_engine_work.md) §3.
