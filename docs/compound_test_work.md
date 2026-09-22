# Составные вращающиеся тела: hardening независимых regression-тестов

Продолжение задачи `build/compound-20260913/tests-hardening.md`. Зона та же:
`tests/rigid_compound_test.c`, `tests/CMakeLists.txt` (только регистрация своего
target), этот отчёт. Production/игра/git не трогались; engine правился
параллельно. Тест остаётся отдельным и линкует `laiue::physics`,
`laiue::numeric`, `laiue::task`.

## Изменённый контракт, который проверяется

Header (`src/physics/rigid_body.h`) теперь публикует:

```c
uint32_t VoxelRigidBodyStepCompoundScratchBytes(uint32_t bodyCount, uint32_t primitiveCount);
```

`primitiveCount` = обычные box-тела (по 1) + сумма `boxCount` compound-форм,
`>= bodyCount`. Бюджет контактов шага = `primitiveCount * 16`. `StepCompoundEx`
вычисляет `primitiveCount` по `shapes` и требует не меньше этого scratch-размера.
`VoxelRigidBodyStepScratchBytes(bodyCount)` равен
`VoxelRigidBodyStepCompoundScratchBytes(bodyCount, bodyCount)` и остаётся
pазмером обычной сцены. `VoxelRigidBodyReadStepStats(scratch, bodyCount, ...)`
совместим после успешного compound-шага. Тензор обязан быть конечным,
симметричным и SPD, а `body.halfExtent` — покрывать всех детей; `boxes == NULL`
тогда и только тогда, когда `boxCount == 0`. Cache с `bodyCapacity`, не
покрывающей нагрузку, даёт явный false, а не усечение.

## Что теперь проверяет тест

### Массовые свойства и FP
- Независимый `ReferenceMassProperties` считает COM, envelope и полный
  row-major 3x3 обратный тензор; сравнение всех 9 элементов, симметрии и
  недиагональных членов. Отдельно: envelope покрывает каждый угол ребёнка;
  касание гранями — не перекрытие.
- `VoxelRigidCompoundMassProperties` отвергает `NULL`/0/count>MAX, mass
  `<=0`/NaN/Inf, нулевые/отрицательные/NaN/Inf полуребра, NaN центр,
  перекрытие, `NULL` out. Out-массивы остаются побитово неизменными.
- `TestMassPropertiesHostileFp`: hostile MXCSR ставится **до** вызова, а не
  после `Initialize`; результат побитово равен нормальному, окружение
  нормализовано (`MassProperties` вызывает `VoxelPhysicsConfigureThread`).

### Масштабируемый scratch и бюджет контактов
- `VoxelRigidBodyStepCompoundScratchBytes(n, n) ==
  VoxelRigidBodyStepScratchBytes(n)` для нескольких n — обычная сцена и её
  layout не изменились.
- Растёт с `primitiveCount`; `bodyCount == 0`, `primitiveCount < bodyCount`,
  `UINT32_MAX`-переполнение дают 0.
- Replay-сцена имеет явный `primitiveCount == 32`; её scratch больше
  legacy-размера и помещается в статический буфер.
- `scratchBytes == 0` и `размер − 1` отвергаются до мутаций.
- Плоская плита из 16 блоков: сначала шаг с legacy-размером отвергается без
  изменений, затем с новым размером проходит. После успешного шага
  `VoxelRigidBodyReadStepStats` читается по **legacy** размеру и показывает
  `contactCount > 16`, то есть плита реально отдыхает, а не отбрасывает опору.
- Cache `bodyCapacity == 1` на плите даёт явный false; capacity, покрывающая
  `primitiveCount`, проходит.
- Реально переплотнённая сцена (12 совпадающих plain-тел через
  `boxCount == 0`) по-прежнему даёт явный false, а не усечение.

### Тензор, envelope, пробуждение
- `StepCompoundEx` до мутаций отвергает: count>MAX, `boxes==NULL` при
  count>0, `boxes!=NULL` при count==0, NaN/Inf, отрицательную диагональ,
  отрицательно определённый, сингулярный и асимметричный конечный тензор,
  под-заниженный `body.halfExtent` и нефинитный envelope. Тело побитово
  неизменно.
- `TestWakeChildrenAware`: спящая L-форма не просыпается от бодрствующего
  куба в пустой нише без контакта и просыпается от такого же куба у сплошного
  плеча.
- L3 и L9 (5 столбиком + 4 в ногу): щуп в пустом квадранте даёт
  `contactCount == 0` при широкой паре, щуп у плеча — контакт.

### Сохранённые проверки
- `boxCount == 0` побитово совпадает с `VoxelRigidBodyStep` 256 тиков;
  одиночный ребёнок близок к legacy; cached-legacy и compound count0 дают
  одинаковые тела и cache-счётчики каждый тик.
- Взаимный импульс, вращение от внецентренного удара, покой плиты на полу.
- Точный replay 512 тиков (L9 и плоская 16-плита в сцене, rebase и импульс),
  три свежих прогона совпадают; warm start суммируется по тикам.
- Executor: serial/split/`LaiueTaskPool` дают одинаковый hash; 383 вызова у
  split и у pool — executor реально работает.
- Rebase `±2^28` и `2^32` через `InfiniteCoord`; hostile FP replay.
- Эталонные hash-и старых тестов не трогались.

### Крупная повёрнутая составная форма (256 детей)
Root нашёл дыру корректности: `CollectWorldContacts` пропускала всё тело,
если его envelope покрывал больше `VOXEL_RIGID_MAX_WORLD_CELLS` (4096) клеток.
Длинный стержень из 256 единичных блоков, повёрнутый на 45° вокруг Z, имеет
повёрнутый AABB примерно `181 x 181 x 1 ≈ 33 000` клеток, поэтому мировые
контакты не собирались и стержень проваливался сквозь пол.

- `TestLargeRotatedRodWorldContacts` строит 256 смежных единичных детей
  (отдельные static-массивы `largeRod*`, не задевая 32-детские фикстуры,
  чтобы прежний replay-hash не менялся), COM в начале координат, ориентация
  45° вокруг Z, старт чуть утоплен в пол (`z = 0.49`).
- Негативный контроль: `LargeRodEnvelopeCells() >
  VOXEL_RIGID_MAX_WORLD_CELLS` — сцена действительно превышает прежний лимит.
  До правки root этот тест падал на `large rotated compound has world
  contacts` (`EXIT=1`), то есть подтверждал дыру; после правки проходит.
- После правки: первый же шаг даёт `contactCount > 0` и
  `contactCount <= 256 * 16` (ограничен бюджетом примитивов); после 512
  тиков стержень лежит на полу (`z ≈ 0.5`, скорости почти нулевые); два
  свежих прогона дают одинаковый hash (exact repeat replay) с cache
  capacity 256.
- Бюджет: `VoxelRigidBodyStepCompoundScratchBytes(1, 256) ≈ 1.69 МиБ`,
  больше legacy `StepScratchBytes(1)`; статический буфер 4 МиБ, cache 1 МиБ,
  всё в BSS — кадр стека не растёт.

## Команды и результаты

```powershell
cmd /c "call \"...\vcvars64.bat\" && cmake --preset windows-msvc -B build/compound-tests-agent -DLAIUE_BUILD_GRAPHICS=OFF"
cmd /c "call \"...\vcvars64.bat\" && cmake --build build/compound-tests-agent --config Debug   --target laiue_rigid_compound_test --parallel 1"
cmd /c "call \"...\vcvars64.bat\" && cmake --build build/compound-tests-agent --config Release --target laiue_rigid_compound_test --parallel 1"
```

- Debug: `EXIT=0`, `compound executor split dispatches: 383`,
  `pool dispatches: 383`, hash `0x6b06a6294d01137d`.
- Release: `EXIT=0`, тот же hash `0x6b06a6294d01137d`.
- `ctest -C Release -R laiue.physics` — 9/9 passed (включая
  `rigid_body`, `rigid_solver`, `rigid_cache`, `rigid_broadphase`,
  `broadphase_scale`, `rigid_parallel`, `determinism`); старые golden-hash-и
  сохранились.
- no-CRT: системные импорты Release-теста ограничены `KERNEL32.dll`; также
  импортируются модули движка physics, numeric и task. Зависимостей от CRT нет.
- Крупный повёрнутый стержень: до правки engine тест падал на
  `large rotated compound has world contacts`, после — проходит и даёт тот
  повторяемый результат отдельного replay стержня в Debug и Release.
  Напечатанный `0x6b06a6294d01137d` относится к основному compound replay.

## Оставшиеся замечания (не падения)

1. Проверка cache-ёмкости в engine стоит **после** сбора контактов и до
   solve/интеграции, поэтому при отказе гравитация текущего тика уже
   применена. Тест не требует all-or-nothing для этого случая — только явный
   false без усечения.
2. `StepOptionsValid` по-прежнему требует `executor->context != NULL`;
   собственный executor с `NULL` context отвергает шаг. Это неочевидно и не
   описано в header.
3. Составная сцена форсирует последовательную узкую фазу; executor проверен
   на стадиях подготовки контактов/кэша, а не на узкой фазе. Игра дополнительно
   проверяет compound replay для canonical и colored solver; отдельный engine-
   тест narrowphase всё ещё последовательный.
4. Реально плотное переполнение теперь требует переплотнённой сцены
   (12 совпадающих тел), потому что бюджет масштабируется по примитивам и
   обычный отдых 16-плиты легален.
5. При итоговой интеграции root дополнительно проверил Linux GCC Debug
   с ASan/UBSan и Release с LTO, Windows clang-cl Debug/Release и игровой
   colored replay. ARM64 Debug/Release проверены только компиляцией,
   линковкой и импортами; нативные ARM64/macOS/mobile не запускались.

## Файлы

- `tests/rigid_compound_test.c` — тест, включая hardening-критерии и крупный
  повёрнутый стержень (256 детей).
- `tests/CMakeLists.txt` — только регистрация `laiue_rigid_compound_test`.
- `docs/compound_test_work.md` — этот отчёт.
