# Аудит переносимости правок 46623fd..HEAD (parallel work)

Отчёт по проверке переносимости сегодняшних правок движка `laiue` на цели,
которые не собирались локально. Проверка выполнена в изолированном дереве
`C:\Users\landi\projects\laiue-portability`, ветка `peer/portability`.

- Диапазон: `46623fd` (родитель) … `2f2781d` (HEAD), 19 коммитов.
- Инструменты: clang 22.1.8 (`C:\Program Files\LLVM\bin\clang-cl.exe`,
  `clang.exe`), MSVC 14.44.35207 (штатная сборка `setup.bat`), CMake 4.4.
- Штатная MSVC x64/AVX2-сборка: `build/peer/setup.bat` → `SETUP-OK`.
- SSE2-сборка: `build/sse2` (`/arch:SSE2`), полный `ctest` — см. раздел 4.

## 1. Метод

Каждый из десяти изменённых файлов прогнан через `clang-cl` для трёх целей:

| Цель | Ключ | Активные макросы |
|---|---|---|
| `x86_64_avx2` | `--target=x86_64-pc-windows-msvc /clang:-march=x86-64-v3` | `_M_X64`, `__SSE2__`, `__AVX2__` |
| `x86_64_sse2` | `--target=x86_64-pc-windows-msvc /clang:-march=x86-64` | `_M_X64`, `__SSE2__` |
| `aarch64` | `--target=aarch64-pc-windows-msvc` | `_M_ARM64`, `__aarch64__`, `__ARM_NEON` |

Определения и пути включения взяты из штатной сборки
(`build/windows-msvc/CMakeFiles/impl-Release.ninja`); архитектурный флаг
clang-cl соответствует `LaiueToolchain.cmake` (`/clang:-march=x86-64-v3`
для AVX2, `/clang:-march=x86-64` для SSE2). Общий набор:

```
-DNOMINMAX -DUNICODE -DWIN32_LEAN_AND_MEAN -D_UNICODE
-DLAIUE_VERSION_MAJOR=0 -DLAIUE_VERSION_MINOR=7 -DLAIUE_VERSION_PATCH=0
-DLAIUE_VERSION_TEXT=L"0.7.0" -DCMAKE_INTDIR="Release" -Isrc -W4 -WX -std:c17
```

Плюс макрос сборки модуля на файл: `-DLAIUE_BUILD_MESHER` (chunk_mesher),
`-DLAIUE_BUILD_PHYSICS` (rigid_body, rigid_broadphase),
`-DLAIUE_BUILD_RENDER -Ibuild/windows-msvc/src` (renderer_d3d12),
`-DLAIUE_BUILD_SCENE` (chunk_streaming), `-DLAIUE_BUILD_WORLD`
(world_infinite). Для тестов — только общий набор.

Дополнительно, помимо `-fsyntax-only`, те же десять файлов собраны **до
объектного файла** (`-c`), чтобы поймать ошибки, видимые только на кодогенерации
(например, диапазоны непосредственных сдвигов SIMD).

Строгие предупреждения: `/W4 /WX` (штатная замена для clang-cl) дал ноль
предупреждений на всех целях; прогон с `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion -Wsign-conversion` для x86_64_avx2, x86_64_sse2 и aarch64 на
изменённых файлах тоже дал **ноль** предупреждений. Стилистических замечаний,
которые пришлось бы чинить, нет.

Для Linux-целей (`x86_64-linux-gnu`, `aarch64-linux-gnu`) применён
`clang.exe --target=… -fsyntax-only`; результат — в разделе 3.

## 2. Таблица «файл × цель × результат»

### Windows (clang-cl, `/W4 /WX`, `-fsyntax-only` и полная компиляция в объект)

| Файл | x86_64_avx2 | x86_64_sse2 | aarch64 |
|---|---|---|---|
| `src/mesh/chunk_mesher.c` | OK | OK | OK |
| `src/physics/rigid_body.c` | OK | OK | OK |
| `src/physics/rigid_broadphase.c` | OK | OK | OK |
| `src/render/renderer_d3d12.c` | OK | OK | OK |
| `src/scene/chunk_streaming.c` | OK | OK | OK |
| `src/world/world_infinite.c` | OK | OK | OK |
| `tests/chunk_mesher_test.c` | OK | OK | OK |
| `tests/rigid_body_test.c` | OK | OK | OK |
| `tests/rigid_parallel_test.c` | OK | OK | OK |
| `tests/voxel_raycast_test.c` | OK | OK | OK |

Все 30 комбинаций «файл × цель» прошли и `-fsyntax-only`, и полную
компиляцию в объект без предупреждений.

### Linux (`clang.exe -fsyntax-only`, заголовки glibc в среде отсутствуют)

| Файл | x86_64-linux-gnu | aarch64-linux-gnu |
|---|---|---|
| `src/mesh/chunk_mesher.c` | FAIL: нет `wchar.h` (через `platform/system.h`) | FAIL: нет `wchar.h` |
| `src/physics/rigid_body.c` | FAIL: нет `string.h` | FAIL: нет `string.h` |
| `src/physics/rigid_broadphase.c` | **OK** | **OK** |
| `src/render/renderer_d3d12.c` | FAIL: нет `wchar.h` (через `content/content_format.h`) | FAIL: нет `wchar.h` |
| `src/scene/chunk_streaming.c` | FAIL: нет `wchar.h` | FAIL: нет `wchar.h` |
| `src/world/world_infinite.c` | FAIL: нет `wchar.h` | FAIL: нет `wchar.h` |
| `tests/chunk_mesher_test.c` | FAIL: нет `wchar.h` | FAIL: нет `wchar.h` |
| `tests/rigid_body_test.c` | FAIL: нет `stdio.h` | FAIL: нет `stdio.h` |
| `tests/rigid_parallel_test.c` | FAIL: нет `stdlib.h` | FAIL: нет `stdio.h` |
| `tests/voxel_raycast_test.c` | FAIL: нет `stdio.h` | FAIL: нет `stdio.h` |

Это ограничение среды, а не дефекты кода: набора заголовков glibc для
Linux-цели на машине нет, `clang` берёт только свои встроенные заголовки
(`stdint.h`, `stddef.h`, `limits.h`, `stdbool.h`, `arm_neon.h`), а `wchar.h`,
`string.h`, `stdio.h`, `stdlib.h` — из glibc. `renderer_d3d12.c` тянет
`content_format.h` → `wchar.h` ещё до `windows.h`, поэтому в Linux-цель он
не собирается в принципе. Единственный файл, который не зависит от libc,
`rigid_broadphase.c`, успешно прошёл обе Linux-цели.

Важно: Linux-цели включают те же архитектурные ветки, что и Windows-цели
(это свойства целевого триплета, а не ОС). Проверено дампом макросов:
`x86_64-linux-gnu -march=x86-64-v3` даёт `__AVX2__`+`__SSE2__`,
`-march=x86-64` — только `__SSE2__`, `aarch64-linux-gnu` — `__aarch64__`+
`__ARM_NEON`. Отсутствие `_MSC_VER` меняет только выбор запасного
`(_MSC_VER && ...)` в `world_infinite.c`, который для Linux и не нужен.

## 3. Сегодняшние `#if`/`#elif`/`#else` и их активация

Список получен через `git diff -U0 46623fd..HEAD -- <файлы>` с фильтром
директив. Другие `#if` в этих файлах — более ранние (см. раздел 6).
«Активна» = ветвь реально компилировалась в одной из целей раздела 1.

### `src/mesh/chunk_mesher.c` — `TransposeBits64` (коммит `4f4e31e`)

| Ветвь | Условие | Где активна |
|---|---|---|
| NEON-транспонирование | `defined(_M_ARM64) || defined(__aarch64__)` | aarch64 (подтверждено препроцессором: `__builtin_neon_vld1q_v`, `vdupq_n_u64`) |
| SSE2-транспонирование | `#else` | x86_64_avx2 и x86_64_sse2 (`_mm_loadu_si128` после `-E`) |

### `src/world/world_infinite.c` — `ClassifyRegion` и include (коммит `9857474`)

| Ветвь | Условие | Где активна |
|---|---|---|
| `#include <immintrin.h>` | `defined(__AVX2__)` | x86_64_avx2 |
| `#include <emmintrin.h>` | `elif defined(__SSE2__) \|\| (_MSC_VER && (_M_X64 \|\| _M_IX86))` | x86_64_sse2 (через `__SSE2__`); sub-условие `_MSC_VER` — в штатной MSVC x64-сборке |
| `WordHasZeroByte` определена | `#if !(defined(__AVX2__) \|\| defined(__SSE2__) \|\| (_MSC_VER && …))` | aarch64 |
| AVX2-цикл (32 байта) | `defined(__AVX2__)` | x86_64_avx2 (`_mm256_loadu_si256`) |
| SSE2-цикл (16 байт) | `elif defined(__SSE2__) \|\| (_MSC_VER && …)` | x86_64_sse2 (`_mm_loadu_si128`) |
| Скалярный 8-байтный цикл + хвост | `#else` | aarch64 (`WordHasZeroByte`) |

### `src/scene/chunk_streaming.c` — проверка целостности (коммиты `2aa5cc7`, `249b4ed`)

| Ветвь | Условие | Где активна |
|---|---|---|
| `VerifyStreamingIntegrity` и три её вызова | `#ifndef NDEBUG` (×4) | в non-NDEBUG прогонах раздела 1 (Debug-путь); Release-путь (`NDEBUG`) — в сборках `build/windows-msvc` и `build/sse2` |

`src/physics/rigid_body.c`, `src/physics/rigid_broadphase.c`,
`src/render/renderer_d3d12.c` и все четыре теста сегодня директив
препроцессора не добавляли и не меняли — `#if` в них отсутствует вовсе.

**Находок «ветвь нигде не активировалась» среди сегодняшних правок нет.**

## 4. Парность SIMD и семантика ветвей

Пары присутствуют полностью, ни одной односторонней ветви:

| Функция | SSE2 | NEON | Запасной путь |
|---|---|---|---|
| `chunk_mesher.c: EqualMask16` (ранее) | `_mm_movemask_epi8(_mm_cmpeq_epi8(...))` | `vceqq_u8` + `laneBitTable` + `vaddv_u8` | — (архитектуры только x86_64/ARM64) |
| `chunk_mesher.c: ColumnSolidMask` (ранее) | `~movemask(cmpeq)` | `vmvnq_u8(vceqq_u8)` + `vaddv_u8` | — |
| `chunk_mesher.c: TransposeBits64` (**сегодня**) | `TRANSPOSE_ROUND_SSE2` | `TRANSPOSE_ROUND_NEON` | общий скалярный раунд stride 1 |
| `world_infinite.c: ClassifyRegion` (**сегодня**) | 16 байт | — | 8 байт (`WordHasZeroByte`) |
| `world_infinite.c: ClassifyRegion` (**сегодня**) | — | — | AVX2 32 байта + SSE2 + скаляр |

Что проверено чтением и сверкой с исходным скалярным кодом:

1. **Транспонирование 64×64.** Макросы `TRANSPOSE_ROUND_NEON` и
   `TRANSPOSE_ROUND_SSE2` идентичны по структуре: страйды 32, 16, 8, 4, 2
   обрабатывают по два соседних 64-битных слова одной 128-битной командой,
   последний страйд 1 общий и скалярный. Разбор индексов:
   `base_` шагает `2S`, `index_` идёт `base_..base_+S` с шагом 2, поэтому
   пара `(index_, index_+S)` покрывает ровно те слова, которые старый цикл
   обходил по `index = ((index | stride) + 1) & ~stride`. Маски
   `0x00000000FFFFFFFF`, `0x0000FFFF0000FFFF`, `0x00FF00FF00FF00FF`,
   `0x0F0F…`, `0x3333…`, `0x5555…` совпадают с последовательным
   `mask ^= mask << stride` исходного кода. Побитовая эквивалентность
   подтверждена исполнением: тест `column_bits` (нерегулярные Z-колонны,
   добавлен сегодня) даёт один и тот же хеш на AVX2- и SSE2-сборках
   (раздел 5).
2. **`ClassifyRegion`.** Ширина шага разная (AVX2 32, SSE2 16, скаляр 8,
   хвост 1), но результат не зависит от ширины: накопители
   `anyAir`/`anySolid` только «накапливают ИЛИ», а досрочный выход по
   `anyAir && anySolid` эквивалентен. Маски `0xFFFFFFFF` (AVX2) и `0xFFFF`
   (SSE2) — полные для своей ширины; скалярный `word != 0` +
   `WordHasZeroByte` дают те же два признака. Идентичность подтверждена
   хешами физики/мира на обеих сборках (раздел 5).
3. Различий в порядке байтов/битов между ветвями не найдено: обе
   NEON-маски строят номер бита по номеру байта (`laneBitTable`), это же
   даёт `movemask`. Цель aarch64 здесь little-endian.

## 5. `ctest` на SSE2-сборке и сверка золотых хешей

Сборка `build/sse2` настроена штатным пресетом с `-DLAIUE_X86_64_LEVEL=sse2`;
в объектных командах стоит `/arch:SSE2` (проверено в
`build/sse2/CMakeFiles/impl-Release.ninja`), без `/arch:AVX2`.

```
ctest --test-dir build/sse2 -C Release -j4   → 100% tests passed, 29 из 29
ctest --test-dir build/windows-msvc -C Release -j4 → 100% tests passed, 29 из 29
```

Сверка выходных хешей AVX2- и SSE2-сборок (запуск исполняемых тестов
напрямую):

| Тест / величина | AVX2 (`windows-msvc`) | SSE2 (`build/sse2`) |
|---|---|---|
| `physics-determinism-hash` | `0x4486debc2d00fab7` | `0x4486debc2d00fab7` |
| `physics-rebased-hash` | `0x4486debc2d00fab7` | `0x4486debc2d00fab7` |
| `rigid-cached-replay-hash` | `0x58c622ddfd840a45` | `0x58c622ddfd840a45` |
| `rigid-parallel-canonical-replay-hash` | `0xb8651ce52897d51d` | `0xb8651ce52897d51d` |
| `rigid-parallel-colored-replay-hash` | `0xcbc65ea4b2f4de5e` | `0xcbc65ea4b2f4de5e` |
| `chunk-mesher checks` | `156777368 failures=0` | `156777368 failures=0` |
| `mesh column_bits` (новый) | `0x23217a5fc6f604c2` (и после rebase) | `0x23217a5fc6f604c2` (и после rebase) |

Все совпадают побитово. Это подтверждает эквивалентность ветвей SSE2 и
AVX2 (включая транспонирование NEON/SSE2-формы через `column_bits`).

## 6. Что не удалось проверить и почему

1. **Исполнение на ARM64.** Запуска ARM64-кода нет: машина x64, ARM64-эмулятора
   в среде нет. Кодогенерация под `aarch64-pc-windows-msvc` и
   `aarch64-linux-gnu` (синтаксис и объект) прошла, но исполнялись только
   x86_64-сборки (AVX2 и SSE2). Это согласуется со статусом в
   `docs/portability.md` («Windows ARM64: собирается, не запускался»).
2. **Штатная MSVC ARM64-сборка.** Компонент ARM64 у MSVC не установлен
   (в `VC\Tools\MSVC\14.44.35207\lib` есть только `x64`, `x86`, `onecore`),
   поэтому ветвь `chunk_mesher.c` с `<arm64_neon.h>` под
   `_MSC_VER && !__clang__` не собиралась нигде. Это **единственная**
   неактивированная ветвь среди просмотренных; она не сегодняшняя (пришла
   раньше `46623fd`) и не входит в список дня. Кодогенерацию NEON проверил
   clang-cl (ветвь `<arm_neon.h>`), семантика та же.
3. **Полная сборка/запуск Linux-целей.** Локально glibc-заголовков нет,
   см. раздел 2. Linux-ветки проверены только на уровне активации
   архитектурных макросов; единственный не зависящий от libc файл
   (`rigid_broadphase.c`) собран целиком.
4. **Полная сборка пресета `windows-clang`.** Отдельно не запускал полный
   build пресета, но все изменённые файлы собраны `clang-cl` в объект для
   x86_64 (AVX2 и SSE2) — этого достаточно для проверки компиляции.
   Интеграционная линковка под clang не является предметом этой задачи.

## 7. Что починено

Ничего. Ни на одной чужой цели не возникло ошибки компиляции или провала
теста, поэтому правок не требовалось. Стилистических предупреждений,
которые стоило бы зафиксировать, тоже не найдено (ноль при `/W4 /WX` и при
расширенном наборе `-Wconversion`/`-Wsign-conversion`/`-Wshadow`).

## 8. Воспроизведение

Внутри дерева созданы вспомогательные артефакты (не влияют на CMake и API):

- `build/peer/setup_sse2.bat` — настройка и сборка `build/sse2`;
- `build/portability_audit/check_win.sh` — `-fsyntax-only` × 3 цели × 10 файлов;
- `build/portability_audit/check_win_obj.sh` — полная компиляция в объект × 3 цели × 10 файлов;
- логи и объекты — в `build/portability_audit/`.

Значения флагов взяты из `build/windows-msvc/CMakeFiles/impl-Release.ninja`
(см. раздел 1); архитектурные флаги — из `cmake/LaiueToolchain.cmake`.
