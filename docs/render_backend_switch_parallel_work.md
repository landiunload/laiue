# Переключение бэкенда рендера (D3D12/Vulkan) на лету — параллельная работа

Отчёт о правке. Зона — `src/render/` и CMake модуля `render`. Публичные
имена и сигнатуры 30 функций `renderer.h` не менялись; их реализации
переехали в диспетчер, а сами бэкенды получили суффиксы. Логика внутри тел
функций бэкендов не менялась (кроме добавленных forward-объявлений, см.
раздел 3).

## 0. Как воспроизвести

```bat
build\peer\setup.bat
build\peer\x.bat cmake --build --preset windows-msvc-debug --parallel 4
build\peer\x.bat ctest --preset windows-msvc-debug --no-tests=error
build\peer\x.bat cmake --build --preset windows-msvc-release --parallel 4
build\peer\x.bat ctest --preset windows-msvc-release --no-tests=error
```

То же для `windows-clang`. `build\peer\x.bat` — обёртка, которая вызывает
`vcvars64.bat` и затем переданную команду (в `build/` и не входит в
репозиторий). Стенды:
`build/peer/drawbench/` (цена `RendererDrawMesh`),
`build/peer/registryprobe/` (реестр и фальсификация).

## 1. Что сделано по пунктам задания

### 1.1. Переименование 30 функций

Скрипт `build/peer/rename_backends.py` применил `re.sub(r'\b' + name +
r'\b', name + '_' + suffix, ...)` ко всем 30 именам раздела 2 в
`src/render/renderer_d3d12.c` (`_D3D12`) и `src/render/renderer_vulkan.c`
(`_Vulkan`). Границы слова не дали префиксным именам (`RendererCreate` /
`RendererCreateMesh`, `RendererReloadTexturePack` /
`RendererReloadTexturePackFrom`, `RendererReloadShaderPack` /
`RendererReloadShaderPackFrom`) задеть друг друга. Внутренние вызовы
(например, 16 вызовов `RendererDestroy`) переименованы вместе с
определениями; посторонние идентификаторы, `struct Renderer`,
`RendererMesh`, `RendererCaptureFrame` (Vulkan-only, живёт в
`renderer_offscreen.h`) не тронуты.

Проверка структурной полноты (скрипт-сверка имён):

```
d3d12 missing defs: []
vulkan missing defs: []
dispatch missing public defs: []
dispatch missing d3d12 extern refs: []
dispatch missing vulkan extern refs: []
```

### 1.2. `src/render/renderer_dispatch.c` (новый файл, 765 строк)

Содержит реестр «указатель → бэкенд» (`RENDERER_REGISTRY_CAPACITY = 8`,
`PlatformMutex`, ленивый one-time-init по образцу
`src/content/content_catalog.c`), `RendererBackendIsAvailable`,
`RendererCreateWithBackend`, `RendererGetBackend`, 60 `extern`-объявлений
обеих суффиксных реализаций под `LAIUE_RENDER_HAS_D3D12` /
`LAIUE_RENDER_HAS_VULKAN` и 30 функций-диспетчеров — ровно блоки из
разделов 1.2 и 3 задания, дословно.

Порядок блоков пришлось слегка переставить: `extern`-объявления из
раздела 3 стоят **до** `RendererCreateWithBackend`, иначе тот вызывает
`RendererCreate_D3D12`/`_Vulkan` без объявления. Оба блока при этом
вставлены целиком и без правок.

### 1.3. `renderer.h`

После `typedef struct RendererMesh` добавлены `RendererBackendKind`
(`AUTO=0`, `D3D12=1`, `VULKAN=2`) и три публичные функции:
`RendererBackendIsAvailable`, `RendererCreateWithBackend`,
`RendererGetBackend`. `RendererCreate` осталась объявленной как была.

### 1.4. CMake (`src/render/CMakeLists.txt`)

- `find_package(Vulkan QUIET)` вместо `REQUIRED` в ветке Vulkan.
- Собирается набор **физически доступного**: `WIN32` добавляет D3D12 и
  `ui_image_wic.*`, `Vulkan_FOUND` добавляет `renderer_vulkan.c` и
  `renderer_offscreen.h`.
- Явный `LAIUE_RENDER_BACKEND=D3D12`/`VULKAN` линкует ровно один бэкенд
  (как раньше); только `AUTO` линкует всё доступное.
- `LAIUE_RENDER_DEFAULT_BACKEND_D3D12`/`_VULKAN` выбирается по
  `LAIUE_RENDER_BACKEND_RESOLVED` (на Windows AUTO — D3D12, сегодняшнее
  поведение).
- `target_compile_definitions(laiue_render PRIVATE
  ${backend_compile_definitions})` после `laiue_add_module`.

**Отклонение от буквы задания (осознанное).** В задании предложено
обернуть логику в `if(LAIUE_RENDER_BACKEND_RESOLVED STREQUAL "D3D12") ...
elseif(... "VULKAN") ... else()`. Но `LaiuePlatform.cmake` уже разрешает
`AUTO` в конкретный бэкенд (`AUTO`+Windows → `D3D12`), и по
`LAIUE_RENDER_BACKEND_RESOLVED` AUTO неотличим от явного D3D12 — тогда
AUTO никогда бы не слинковал оба, что противоречит требованию «только AUTO
линкует всё доступное». Поэтому ветвление идёт по исходной опции
`LAIUE_RENDER_BACKEND` (до resolve). Смысл `LAIUE_RENDER_BACKEND_RESOLVED`
для остальной сборки (шейдеры, top-level список модулей, тесты, платформа)
не изменён.

### 1.5. Шейдеры

`cmake/LaiueShader.cmake` и логика выбора `LAIUE_RENDER_BACKEND_RESOLVED`
не тронуты. Ограничение раздела 1.5 задания сохраняется полностью: даже
при двойной линковке смена на Vulkan в рантайме кадр не нарисует, пока
`LaiueShader.cmake` не научится компилировать оба набора шейдеров. Это
вне объёма задачи.

## 2. `git diff --stat` и новые файлы

```
 src/render/CMakeLists.txt    |  67 ++++++++++++++++++++++---
 src/render/renderer.h        |  19 +++++++
 src/render/renderer_d3d12.c  | 115 ++++++++++++++++++++++---------------------
 src/render/renderer_vulkan.c |  81 ++++++++++++++++--------------
 4 files changed, 182 insertions(+), 100 deletions(-)
```

Новый (untracked) файл: `src/render/renderer_dispatch.c` (765 строк).
Новый отчёт: `docs/render_backend_switch_parallel_work.md`.

## 3. Что пришлось добавить к чистому переименованию

Скрипта переименования оказалось недостаточно: рендереры зовут **свои**
суффиксные функции до их определения, а раньше эти вызовы разрешались
публичными объявлениями из `renderer.h` (они теперь указывают на
диспетчер). Точная проверка показала, что до определения вызывается
только `RendererDestroy_<backend>` (16 раз в D3D12, 1 раз в Vulkan — путь
отката `RendererCreate`). Поэтому в каждый бэкенд добавлено одно
forward-объявление:

```c
void RendererDestroy_D3D12(Renderer* renderer);   // renderer_d3d12.c
void RendererDestroy_Vulkan(Renderer *renderer);  // renderer_vulkan.c
```

Это единственные добавленные строки кода в бэкендах; `git diff` по
`renderer_vulkan.c` подтверждает, что поменялись только строки с
`_Vulkan` (плюс это объявление и комментарий к нему):

```
=== vulkan added lines without _Vulkan ===
+// Публичное имя RendererDestroy теперь живёт в renderer_dispatch.c, а
+// раньше её определения — объявляем её здесь.
+
```

Никакой логики внутри тел функций не изменилось.

## 4. Горячий путь: замер `RendererDrawMesh`

Стенд `build/peer/drawbench/draw_bench.c` создаёт скрытое окно, настоящий
D3D12-рендерер, мир и один меш, затем замеряет 200 000 вызовов
`RendererDrawMesh` подряд (1000 × 200 раундов) по `QueryPerformanceCounter`.
Стенд линкуется через установленный префикс движка
(`cmake --install ... --prefix build/windows-msvc/bench-prefix`).

| Вариант | ns на вызов (прогоны) |
| --- | --- |
| До правки (прямой вызов D3D12) | 49.66, 48.68, 49.21, 51.93, 47.17, 45.83 |
| После, реестр под мьютексом на каждый вызов | 113.93, 113.16, 117.94, 120.18, 122.82 |
| После, быстрый путь одним acquire-чтением | 49.00, 49.59, 46.06, 45.63, 51.03 |

Первый вариант диспетчера (буквально блок 1.2/3: мьютекс в `LookupBackend`
на каждый вызов) дал **регресс ~2.4×** на кадровом горячем пути. Задание
(свойство 3) прямо требует его устранить. Сделано аддитивно, не меняя
реестр как источник истины:

- добавлен кэш одной записи `g_rendererFastHandle` + `g_rendererFastKind`;
- writer (Register/Unregister, редкий, под мьютексом) пишет handle простым
  присваиванием и **потом** публикует backend `PlatformAtomicStoreU32Release`;
- reader (`LookupBackend`, каждый вызов) делает
  `PlatformAtomicLoadU32Acquire`; увидев ненулевой backend, читает handle и
  возвращает его (acquire гарантирует видимость записи handle); при
  промахе/нуле идёт в реестр под мьютексом;
- при уничтожении кэш гасится и в него поднимается любой оставшийся
  рендерер.

Итог: 45.6–51.0 нс/вызов — в пределах шума относительно «до». Аргументов
указателя/`u64`-атомиков в `platform/system.h` нет, поэтому одним
release/acquire вокруг 32-битного kind и обошлись. Многорендерных
сценариев (несколько живых рендеров, «медленный» путь под мьютексом) это
не касается — они редки и корректны.

## 5. Сборка и тесты

Конфигурация: `Vulkan_FOUND = FALSE`, `renderer_vulkan.c` в сборку
`laiue_render` не входит. Подтверждение — из `CMakeCache.txt`:

```
build/windows-msvc/CMakeCache.txt:Vulkan_INCLUDE_DIR:PATH=Vulkan_INCLUDE_DIR-NOTFOUND
build/windows-msvc/CMakeCache.txt:Vulkan_LIBRARY:FILEPATH=Vulkan_LIBRARY-NOTFOUND
build/windows-clang/CMakeCache.txt:Vulkan_INCLUDE_DIR:PATH=Vulkan_INCLUDE_DIR-NOTFOUND
build/windows-clang/CMakeCache.txt:Vulkan_LIBRARY:FILEPATH=Vulkan_LIBRARY-NOTFOUND
```

Состав объектов `laiue_render` (Release), MSVC и clang одинаков:

```
renderer_d3d12.c.obj
renderer_dispatch.c.obj
shader_pack.c.obj
texture_build.c.obj
texture_pack.c.obj
ui_image_wic.c.obj
```

`renderer_vulkan.c.obj` отсутствует. Экспорты `laiue_render.dll`
(`dumpbin /exports`, Release): все 30 старых имён на месте, добавлены ровно
три новых (`RendererBackendIsAvailable`, `RendererCreateWithBackend`,
`RendererGetBackend`); суффиксных имён (`_D3D12`) среди экспортов нет.
`ctest --no-tests=error`:

| Конфигурация | Результат |
| --- | --- |
| windows-msvc Debug | 30/30 passed |
| windows-msvc Release | 30/30 passed |
| windows-clang Debug | 30/30 passed |
| windows-clang Release | 30/30 passed |

Поведение golden-пути (`RendererCreate` без новых функций) не изменилось:
тест `laiue.render.packs` и остальные 29 проходят без правок, набор тестов
тот же (30).

## 6. Фальсификация

Стенд `build/peer/registryprobe/registry_probe.c` проверяет:
`RendererBackendIsAvailable` для всех трёх значений; что явный Vulkan
возвращает `NULL`; что `AUTO` даёт D3D12 и рабочий рендерер; что при двух
живых рендерах после уничтожения первого второй остаётся D3D12 и рабочим;
20 циклов create+destroy без потери записи; `RendererGetBackend(NULL)`.

1. **Корректный код:** `registry_probe OK`.
2. **Намеренно испорченный реестр** (`RegisterRenderer` записывает
   `RENDERER_BACKEND_VULKAN` вместо реального backend): стенд падает —
   `registry_probe FAIL: AUTO renderer reports D3D12`,
   `FAIL: AUTO renderer functional`, `failures=4`. После восстановления —
   снова `OK`. Значит проверки реестра «с зубами».
3. **Проверка `git diff` бэкендов:** показано в разделе 3 — изменились
   только имена `_Vulkan`/`_D3D12` и forward-объявление `RendererDestroy`.
4. **Леак `UnregisterRenderer` (вариант из задания)** на этой машине
   поведенчески **не наблюдаем**, и это честно зафиксировано. Причина: в
   сборке слинкован только D3D12, поэтому «перепутать бэкенды» не с чем;
   а    аллокатор `HeapAlloc(GetProcessHeap())` возвращает **тот же самый
   адрес** структуры `Renderer` на каждом цикле (проверено печатью
   указателя: во всех 20 циклах одного запуска адрес один и тот же).
   Протухшая запись реестра с этим адресом и правильным backend маскирует
   переполнение.
   Свежий адрес появляется только когда ≥8 блоков того же размера живы
   одновременно, но тогда и корректный код не сможет зарегистрировать
   9-й рендерер. Поэтому переполнение `RENDERER_REGISTRY_CAPACITY=8`
   через публичный API здесь не детектируется; вместо этого «зубы»
   доказаны порчей записанного backend (пункт 2).

## 7. Что не удалось проверить и что делать на машине с Vulkan SDK

На этой машине нет Vulkan SDK (`VULKAN_SDK` пуст, `Vulkan_*—NOTFOUND`),
поэтому **не проверено**:

- реальная одновременная линковка `renderer_d3d12.c` и `renderer_vulkan.c`
  в одну `laiue_render.dll`;
- что `LAIUE_RENDER_HAS_VULKAN`-ветки диспетчера компилируются и линкуются
  (extern-имена сверены текстуально, но не слинкованы);
- `RendererCreateWithBackend(..., RENDERER_BACKEND_VULKAN)`,
  `RendererGetBackend` для Vulkan-рендера, вызовы `_Vulkan`-функций через
  диспетчер.

Проверка на машине с Vulkan SDK (Windows, PowerShell/Developer Shell):

```bat
cmake -S . -B build\vulkan-dual -G "Ninja Multi-Config" ^
  -DCMAKE_C_COMPILER=cl.exe -DLAIUE_BUILD_GRAPHICS=ON ^
  -DLAIUE_RENDER_BACKEND=AUTO
cmake --build build\vulkan-dual --config Release --parallel 4
```

Ожидается: в выводе `Vulkan_FOUND=TRUE`/`Vulkan_LIBRARY` найден, в
`build\vulkan-dual\src\render\CMakeFiles\laiue_render.dir\Release\` лежат
`renderer_d3d12.c.obj` **и** `renderer_vulkan.c.obj`, `LAIUE_RENDER_HAS_D3D12`
и `LAIUE_RENDER_HAS_VULKAN` определены одновременно. Затем прогнать
`ctest` и отдельно проверить, что `RendererCreate` даёт D3D12
(`RendererGetBackend == RENDERER_BACKEND_D3D12`), а
`RendererCreateWithBackend(..., RENDERER_BACKEND_VULKAN)` возвращает
не-NULL. Напомню: **кадр** на Vulkan при этом всё равно не нарисуется,
пока не решена задача с `LaiueShader.cmake` (раздел 1.5 задания).

На Linux в CI (`linux-vulkan-engine`, программный lavapipe) пройдёт ветка
`LAIUE_RENDER_BACKEND=VULKAN`: `renderer_vulkan.c` + `renderer_dispatch.c`,
`LAIUE_RENDER_HAS_VULKAN` и `LAIUE_RENDER_DEFAULT_BACKEND_VULKAN` — это
единственное реальное исполнение `_Vulkan`-суффиксов, и оно случится после
приёма правки.
