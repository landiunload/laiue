# Составные вращающиеся тела: работа исполнителя

Эта заметка принадлежит ограниченной параллельной задаче `compound-20260913`
(зонa записи: `src/physics/rigid_body.c`, `src/physics/rigid_body.h`,
`docs/compound_engine_work.md`). Root читает её как отчёт о сделанном и
проверенном. Ниже — что добавлено, как устроено и что реально подтверждено.

## Что сделано

Additive API без изменения существующих `VoxelRigidBody`,
`VoxelRigidStepOptions` и старого ABI. В `rigid_body.h` добавлены
`VoxelRigidCompoundBox`, `VoxelRigidCompoundShape`,
`VOXEL_RIGID_COMPOUND_MAX_BOXES`, `VoxelRigidCompoundMassProperties`,
`VoxelRigidBodyStepCompoundScratchBytes` и `VoxelRigidBodyStepCompoundEx`;
старые entry points не тронуты.

### Изменённый/добавленный API (для root и игры)

- `uint32_t VoxelRigidBodyStepCompoundScratchBytes(uint32_t bodyCount,
  uint32_t primitiveCount)` — размер scratch для compound-шага.
  `primitiveCount` = число обычных box-тел (включая inactive, как 1 каждое) +
  сумма `boxCount` составных. Значения `< bodyCount` и переполнение
  `UINT32` дают `0`. При `primitiveCount == bodyCount` результат **точно**
  равен прежнему `VoxelRigidBodyStepScratchBytes(bodyCount)`.
- `VoxelRigidBodyStepCompoundEx` сам считает `primitiveCount` по `shapes` и
  требует scratch не меньше результата sizing-функции; иначе явный `false`
  до первой мутации. Игра должна резервировать scratch этим вызовом, а
  `VoxelRigidBodyReadStepStats(scratch, bodyCount, ...)` продолжает работать
  без изменений: compound шаг копирует stats в legacy-адрес в конце.
- Cache с `bodyCapacity >= primitiveCount` рекомендуется для compound.
  При меньшей ёмкости шаг откажет, если реальных контактов больше
  `bodyCapacity * 16`; контакты не усекаются. Cache storage по-прежнему
  выделяется по `VoxelRigidContactCacheBytes(bodyCapacity)`.
- `boxes == NULL` тогда и только тогда, когда `boxCount == 0`; иначе шаг
  отклоняется.

`VoxelRigidCompoundMassProperties` считает COM, консервативный envelope
относительно COM и полный обратный тензор инерции для неперекрывающихся
коробок при однородной плотности. Масса каждой коробки пропорциональна
объёму; тензор — собственный вклад плюс теорема Гюйгенса—Штейнера. Тензор
симметричный, row-major 3x3. Отказ (неверный вход, перекрытие, вырожденный
или нефинитный тензор) не меняет out-параметры: запись идёт только после
успешного расчёта. Функция вызывает `VoxelPhysicsConfigureThread()` перед
арифметикой, поэтому COM/envelope/тензор не зависят от враждебного MXCSR/FPCR.
Pairwise overlap — это build-time контракт этой функции; Step его не
перепроверяет на каждом тике.

## Как устроен compound step

1. **Dispatch.** `RigidBodyStepInternal` получил внутренний параметр
   `shapes`. Если ни у одного тела нет дочерних коробок, весь прежний
   конвейер — включая параллельные grid/tree коллекторы — работает
   побитово как раньше. Только при `compoundScene` narrowphase уходит на
   детерминированный последовательный путь.
2. **Проверка до мутаций.** `CompoundShapesValid` требует `boxes == NULL`
   ровно при `boxCount == 0`, конечность центров/полурёбер, конечный
   **симметричный положительно определённый** `inverseInertia` (Сильвестр:
   положительные диагональ, минор 2x2 и определитель) и чтобы
   `body->halfExtent` действительно покрывал каждого ребёнка в координатах
   тела. Битый или занижённый envelope теперь отклоняется, а не теряет
   broadphase-контакты. `CompoundShapesAliasFree` запрещает пересечение
   описаний форм с bodies, scratch, settings, collision, contact cache и
   broadphase. Обе проверки выполняются до гравитации/bounds. Размер
   scratch, `primitiveCount` и бюджет контактов проверяются ещё раньше —
   весь отказ происходит до первой мутации.
3. **Широкая фаза не меняется.** Envelope (`body->halfExtent`) задаёт AABB,
   сетку и дерево. Шаг проверяет, что envelope действительно покрывает всех
   детей, поэтому пара с пересекающимися детьми всегда находится. O(N^2) не
   появляется. Пробуждение (`WakePairTouches`) для compound тоже считает
   фактическое child-child касание: спящая L-форма не просыпается от
   объекта в её пустой нише; обычные коробки сохраняют прежний SAT по
   envelope.
4. **World narrowphase.** Клетки мира по-прежнему склеиваются жадным
   разбиением, затем каждая дочерняя коробка строит свой
   `BuildBlockManifold` против слитой коробки. Пустоты между детьми не
   заполняются: L-форма не превращается в объемлющий параллелепипед.
5. **Body narrowphase.** Для пары тел перебираются `child_i x child_j`,
   порядок фиксирован `(firstChild, secondChild)`, поэтому контакты пары
   остаются непрерывным run-ом — это нужно `ContactRunEnd`, warm start и
   colored solver. Дети одного тела не проверяются между собой никогда.
6. **Инерция.** `PrepareImpulseResponse` получил флаг полного тензора.
   Обычная коробка идёт прежним диагональным путём (биты не меняются);
   составное тело использует `R * I_inv * R^T * v` по девяти компонентам.
   `ApplyInverseInertiaFull` — отдельная функция, старый
   `ApplyInverseInertia` не изменён.
7. **Плечи/моменты.** Точка контакта мировая, `lever = point - COM` тела;
   скорости, угловая скорость, интеграция и ориентация остаются
   body-level. Поэтому момент относительно настоящего COM корректен и
   BigInt позиции/скорости не трогаются.
8. **Параллельный и цветной решатель.** При `compoundScene` отключаются
   только параллельные коллекторы пар (они собрали бы contacts по
   envelope). Подготовка, warm start, colored scheduling, парный решатель и
   интеграция работают как раньше: они оперируют уже подготовленными
   контактами и не знают о форме. `VOXEL_RIGID_SOLVER_COLORED`,
   broadphase-дерево и contact cache проверены с compound (см. ниже).
9. **Масштабируемый бюджет.** Общий бюджет контактов шага —
   `primitiveCount * 16`, где `primitiveCount` учитывает каждого ребёнка.
   Внутренняя раскладка scratch расширяет только `contacts`,
   `solverContacts`, `contactColors` и `solveOrder` до `primitiveCount`;
   всё остальное остаётся по `bodyCount`, а обычная сцена сохраняет прежний
   размер байт-в-байт и прежний быстрый путь. Поэтому честная форма из
   `VOXEL_RIGID_COMPOUND_MAX_BOXES` примитивов пригодна для обычного отдыха:
   16-блочная плита и L9 получают сотни точек, а не 16 на тело.
10. **Legacy stats.** Compound собирает `VoxelRigidStepStats` в маленькой
   локальной структуре и копирует их в legacy `StepStatsPointer(scratch,
   bodyCount)` только после успешного завершения (адрес перекрыт рабочими
   массивами до конца шага). `VoxelRigidBodyReadStepStats` остаётся
   совместимым.
11. **Переполнение.** `AppendContact` при исчерпании бюджета ставит флаг и
   не пишет контакт; шаг возвращает `false`. Contact cache проверяет, что
   `contactCount <= bodyCapacity * 16`, до записи — тоже явный `false`, без
   тихого усечения и без переполнения storage.

## Что проверено

Команды выполнялись в `build/compound-engine-agent` (Windows x64, MSVC
19.44, Ninja Multi-Config), `--parallel 1`.

- Debug и Release: `laiue_physics` собирается без предупреждений (W4/WX,
  `/GS-`, precise FP).
- `ctest -R "laiue.physics"` в Debug и Release — **9/9** пройдены, включая
  `rigid_body`, `rigid_solver`, `rigid_cache`, `rigid_broadphase`,
  `rigid_broadphase_scale`, `rigid_parallel` и
  `laiue.physics.rigid_compound` агента тестов. Значит, эталонные replay-хеши
  обычных коробок не изменились. Независимый compound-тест покрывает
  `MassProperties` против отдельного эталона, отказ по не-SPD/асимметрии/
  занижённому envelope, L-shape и L9 против щели, 16-блочную плиту с >16
  опорами, масштабируемый scratch и отказ старого размера до мутации,
  children-aware пробуждение спящей L и 512-тиковый replay.
- Отдельная временная программа (`compound_check`, вне репозитория)
  подтвердила:
  - аналитический `MassProperties` для двух симметричных коробок
    (COM=0, envelope, `invI = diag(1.5, 0.6, 0.6)`);
  - отказ при перекрытии/нулевом числе коробок/нулевой массе/плоской
    коробке и неизменность out-параметров при отказе;
  - составное тело из двух коробок ложится на пол (COM z ≈ 0.5), не
    проваливается и имеет world-контакты;
  - два составных тела складываются друг на друга с контактами тело-тело;
  - **дырка L/кольца не заполняется:** кольцо с пустым центром вокруг
    столба, торчащего сквозь него, падает по стенкам до пола, а не садится
    на верх столба, как сделал бы объемлющий box;
  - **бюджет масштабируется:** 8 детей на одном теле теперь стоят на полу
    (128 точек бюджета), а старое `VoxelRigidBodyStepScratchBytes(1)` меньше
    нового размера и отклоняется до мутации;
  - обычная коробка через `VoxelRigidBodyStepCompoundEx` (`boxCount = 0`)
    идёт бит-в-бит как `VoxelRigidBodyStep`;
  - indexed (`options.broadphase`) + `contactCache` +
    `VOXEL_RIGID_SOLVER_COLORED` вместе: тело ложится на пол, warm start
    реально сопоставляет контакты;
  - составное тело засыпает на полу и просыпается, когда на него падает
    обычный куб (пробуждение по envelope находит детей).

## Ограничения и что не проверено

- Compound можно направить только последовательным narrowphase: в таком
  вызове параллельные сборщики пар не работают, хотя подготовка, решатель и
  интеграция всё ещё могут использовать executor. Это принятое упрощение,
  разрешённое задачей.
- Для чрезмерно большого envelope составное тело разбирается по детям;
  каждый ребёнок обязан уложиться в `VOXEL_RIGID_MAX_WORLD_CELLS`, иначе шаг
  возвращает `false`. Обычная legacy-коробка сохраняет прежний предел выборки.
- Даже масштабированный бюджет конечен: сцена, где число контактов
  превышает `primitiveCount * 16`, отклоняется явным `false`, а не режется.
- Step не проверяет pairwise overlap детей (это build-time контракт
  `VoxelRigidCompoundMassProperties`): перекрывающиеся дети дадут удвоенный
  объём/инерцию, но не сломают память. Envelope и тензор при этом проверяются
  и fail-closed.
- Contact cache остаётся caller-owned: `bodyCapacity >= primitiveCount`
  рекомендуется для предсказуемого размера. При меньшей ёмкости шаг вернёт
  `false`, если фактических контактов больше `bodyCapacity * 16`; это не
  тихое усечение и не обещает отката уже применённой гравитации.
- Warm start сопоставляет локальные якоря с допуском 0.01 блока: контакты
  разных детей ближе 0.01 могут перепутаться по старому контракту кэша.
- Не реализованы CCD, persistent manifold и feature IDs; compound
  наследует те же ограничения, что и обычные коробки.
- Нативные ARM64/macOS/mobile не запускались. При итоговой интеграции root
  проверил ARM64 Debug/Release link-closure и импорты physics, numeric, task и
  compound test: зависимостей от CRT нет. Это не ARM64 runtime-проверка.
- Итоговый compound replay в Windows MSVC/clang-cl и Linux GCC совпал:
  `0x6b06a6294d01137d`; Linux также проверен ASan/UBSan. Полная матрица
  интеграции и ограничения записаны в игре, `docs/compound_recovery.md`.

## Файлы

- `src/physics/rigid_body.h` — контракт.
- `src/physics/rigid_body.c` — реализация.
- `docs/compound_engine_work.md` — эта заметка.

Чужие правки в `tests/`, `CMakeLists.txt` и остальных модулях не трогались.
