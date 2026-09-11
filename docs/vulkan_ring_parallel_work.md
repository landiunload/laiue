# Крупная арена загрузки мешей в Vulkan-бэкенде

Отчёт о переносе починки кольца загрузки мешей с D3D12 на Vulkan. Зона —
только `src/render/renderer_vulkan.c`. Публичный API/ABI (`renderer.h`),
заголовки, CMake, шейдеры, физика и тесты не менялись. Изменён ровно один
файл: `git diff --stat` показывает `src/render/renderer_vulkan.c | 57 +++--`
(57 добавлено, 10 удалено).

## 0. Итог коротко

- На этой машине **Vulkan SDK по-прежнему нет**: `VULKAN_SDK`/`VK_SDK_PATH`
  пусты, заголовка `vulkan/vulkan.h` нет нигде, `glslangValidator`/`glslc`
  не установлены. `vulkan-1.dll` есть только в `C:\Windows\System32` — это
  системный loader, не SDK.
- Штатная CMake-цель с `-DLAIUE_RENDER_BACKEND=VULKAN` **не конфигурируется**:
  `find_package(Vulkan REQUIRED)` падает. Значит, собрать, а тем более
  исполнить Vulkan-бэкенд на этой машине физически нельзя (раздел 1).
- Правка сделана **по чтению кода** и структурному соответствию с D3D12
  (раздел 3). Вторая крупная арена (`8 МиБ`/кадровый слот, ленивая,
  двойное буферирование) перенесена в Vulkan-эквивалент
  (`EnsureVulkanLargeMeshBuffer`, порядок «малое кольцо → крупная арена →
  отдельный буфер»).
- Вместо исполнимого измерения — построчная таблица соответствий и разбор
  каждого свойства (разделы 3–5). Честно перечислено, что именно **не**
  проверено (раздел 6).
- D3D12-сборка (штатный `AUTO` на Windows) собралась, `ctest` — **30/30**.
  `renderer_vulkan.c` в это дерево не входит вообще (0 упоминаний в
  сгенерированных ninja-файлах), поэтому правка Vulkan-файла на эти тесты
  повлиять не может (раздел 2).

## 1. Доказательство отсутствия Vulkan SDK

Проверка окружения:

```
VULKAN_SDK=[]        (пусто)
VK_SDK_PATH=[]       (пусто)
C:\Windows\System32\vulkan-1.dll   (есть — это loader, не SDK)
find ... -iname vulkan.h        → ничего
where glslangValidator glslc   → ничего
```

Проба штатной конфигурации Vulkan (`-DLAIUE_RENDER_BACKEND=VULKAN`,
отдельное дерево вне репозитория):

```
-- laiue platform backend: WINDOWS; ... render backend: VULKAN
-- laiue graphics modules (VULKAN): audio;mesh;render;scene
CMake Error at .../FindPackageHandleStandardArgs.cmake:290 (message):
  Could NOT find Vulkan (missing: Vulkan_LIBRARY Vulkan_INCLUDE_DIR) (found
  version "")
Call Stack:
  .../src/render/CMakeLists.txt:23 (find_package)
-- Configuring incomplete, errors occurred!
EXITCODE=1
```

Синтаксическая компиляция `renderer_vulkan.c` через `cl.exe /c ... /W4 /WX`
тоже невозможна: файл на строке 16 делает `#include <vulkan/vulkan.h>`,
которого на машине нет; считать «компиляцию» по отсутствующему заголовку
нельзя без подмены реального API вымышленным (это была бы не проверка, а
самообман). Поэтому, как и разрешено условием задачи, исполнимой сборки и
замера времени не делалось; вместо них — структурное доказательство.

## 2. Подтверждение, что D3D12-тесты не затронуты

Конфигурация по умолчанию на Windows разрешается в D3D12:

```
-- laiue platform backend: WINDOWS; modules: SHARED; native mods: DYNAMIC;
   render backend: D3D12
-- laiue graphics modules (D3D12): input;audio;mesh;render;scene;ui
CONFIGURE_EXITCODE=0
```

`src/render/CMakeLists.txt:22-27`: `renderer_vulkan.c` попадает в
`backend_sources` **только** при `LAIUE_RENDER_BACKEND_RESOLVED STREQUAL
"VULKAN"`. Проверка сгенерированного дерева:

```
grep -rl renderer_vulkan build/windows-msvc/   → 0 файлов
```

То есть Vulkan-файл в D3D12-сборку не входит, и его правка на 30/30
тестов повлиять не может.

## 3. Построчное соответствие D3D12 → Vulkan

Все ссылки — на текущий HEAD после правки. D3D12 — измеренная и работающая
версия, Vulkan — перенос.

### 3.1. Бюджет и алигн

| Смысл | D3D12 (`renderer_d3d12.c`) | Vulkan (`renderer_vulkan.c`) |
| --- | --- | --- |
| Малое кольцо | `MESH_UPLOAD_BYTES_PER_FRAME` = 4 МиБ, стр. 67 | `MESH_UPLOAD_BYTES_PER_FRAME` = 4 МиБ, стр. 52 |
| Крупная арена | `LARGE_MESH_UPLOAD_BYTES_PER_FRAME` = 8 МиБ, стр. 72 | `LARGE_MESH_UPLOAD_BYTES_PER_FRAME` = 8 МиБ, стр. 57 |
| Алигн записей | `(offset + 15u) & ~15u`, стр. 1754/1771 | `AlignUp(offset, 16u)`, стр. 1842/1859 |

Тот же байтовый бюджет и то же выравнивание 16, что и в D3D12.

### 3.2. Поля состояния

D3D12 (стр. 210-215):

```c
ID3D12Resource* meshUploadBuffers[FRAME_COUNT];
uint8_t*        meshUploadMapped[FRAME_COUNT];
uint32_t        meshUploadOffsets[FRAME_COUNT];
ID3D12Resource* largeMeshUploadBuffers[FRAME_COUNT];
uint8_t*        largeMeshUploadMapped[FRAME_COUNT];
uint32_t        largeMeshUploadOffsets[FRAME_COUNT];
```

Vulkan (стр. 249-251): у Vulkan-буфера `mapped` уже лежит внутри
`GpuBuffer`, поэтому маппированный указатель не нужен отдельным полем:

```c
GpuBuffer meshUploadBuffers[FRAME_COUNT];
uint32_t  meshUploadOffsets[FRAME_COUNT];
GpuBuffer largeMeshUploadBuffers[FRAME_COUNT];
uint32_t  largeMeshUploadOffsets[FRAME_COUNT];
```

### 3.3. Ленивое создание арены

D3D12 `EnsureLargeMeshUploadBuffer` (стр. 1694-1731): если буфер слота уже
есть — `true`; иначе `CreateCommittedResource` UPLOAD на 8 МиБ и один раз
`Map`; при любой неудаче освобождает буфер и возвращает `false`.

Vulkan `EnsureVulkanLargeMeshBuffer` (стр. 1811-1820): та же логика в
терминах Vulkan — если `largeMeshUploadBuffers[frameIndex].buffer` уже не
`VK_NULL_HANDLE`, вернуть `true`; иначе `BufferCreate(...)` 8 МиБ с
`VK_BUFFER_USAGE_TRANSFER_SRC_BIT`, `hostVisible = true` (тот же
host-visible/coherent путь, что у малого кольца при создании,
`CreateFrameBuffers`, стр. 1576). `BufferCreate` сам делает
`vkCreateBuffer` + подбор памяти + `vkAllocateMemory` + `vkBindBufferMemory`
+ `vkMapMemory` и при любой неудаче вызывает `BufferDestroy` и возвращает
`false` (стр. 315-367). Отдельного `Unmap`-пути нет: его делает
`BufferDestroy` (стр. 301-313) при финальном освобождении.

### 3.4. Порядок проверок в `RendererCreateMesh`

| Шаг | D3D12 (стр. 1752-1819) | Vulkan (стр. 1839-1885) |
| --- | --- | --- |
| 1. Малое кольцо | `fitsSmallRing`; `memcpy` в `meshUploadMapped`, сдвиг `meshUploadOffsets` | `fitsSmallRing`; `memcpy` в `meshUploadBuffers[...].mapped`, сдвиг `meshUploadOffsets` |
| 2. Крупная арена при переполнении | `largeOffset` из `largeMeshUploadOffsets`; `fitsLargeRing && EnsureLargeMeshUploadBuffer`; `memcpy` в `largeMeshUploadMapped`, сдвиг `largeMeshUploadOffsets`, `usedLargeRing = true` | то же: `largeOffset` из `largeMeshUploadOffsets`; `fitsLargeRing && EnsureVulkanLargeMeshBuffer`; `memcpy` в `.mapped`, сдвиг, `usedLargeRing = true` |
| 3. Иначе отдельный буфер | `CreateCommittedResource` + `Map` + `memcpy` + `Unmap`; `ownsStaging = true`; при неудаче `PoolFree` и `NULL` | `BufferCreate` + `memcpy`; `ownsStaging = true`; при неудаче `PoolFree` и `NULL` |

Порядок ровно тот же: сначала малое кольцо, потом крупная арена, и только
если запись не влезла и туда — отдельное выделение. Именно шаг 2 убирает
создание буфера на каждый переполнивший меш.

### 3.5. Сброс смещений арены

D3D12 сбрасывает обе величины одного слота в `RendererEndFrame` после
`MoveToNextFrame` (стр. 2496-2497). У Vulkan кольцо сбрасывается в
`RendererBeginFrame` для текущего слота (стр. 2078-2079), рядом с уже
существующим сбросом `meshUploadOffsets`; крупная арена сброшена ровно там
же:

```c
renderer->meshUploadOffsets[renderer->frameIndex] = 0u;
renderer->largeMeshUploadOffsets[renderer->frameIndex] = 0u;
```

Это то же двойное буферирование: слот переиспользуется только после
`vkWaitForFences` в `RendererBeginFrame` (стр. 2074), как и раньше для
малого кольца.

### 3.6. Уничтожение арены

D3D12 освобождает крупную арену в `RendererReleaseWorld` (стр. 1476-1490)
вместе с малым кольцом, потому что там upload-буферы имеют время жизни
мира. В Vulkan малое кольцо живёт у рендерера (`CreateFrameBuffers` /
`RendererDestroy`), не у мира, поэтому арена освобождается в том же месте,
что и `meshUploadBuffers` (стр. 1772):

```c
BufferDestroy(renderer, &renderer->meshUploadBuffers[frame]);
BufferDestroy(renderer, &renderer->largeMeshUploadBuffers[frame]);
```

Это осознанное отличие жизненного цикла между бэкендами, а не пропуск:
в Vulkan `RendererReleaseWorld` (стр. 1673-1695) upload-кольцо не трогает,
и арена обязана жить столько же, сколько кольцо.

## 4. Обязательные свойства

1. **Тот же набор мешей, та же геометрия.** Изменился только выбор
   staging-буфера для копии. `upload->sizeBytes`, `sourceOffset` и
   `destinationOffset` в `PendingUpload` (стр. 1903-1909) заполняются так
   же; `RecordPendingUploads` по-прежнему делает один `vkCmdCopyBuffer` на
   запись из `upload->staging` по `sourceOffset` в блок пула
   (стр. 2031-2049). `currentStats.uploadedBytes` растёт на ту же
   `sizeBytes`. `vkCmdDraw` рисует по `mesh->quadCount * 6` без изменений.
   Арена лишь переносит байты туда же, куда их переносило бы отдельное
   выделение.
2. **API/ABI/CMake.** `renderer.h` и `RendererCreateMesh` не менялись,
   новых экспортов нет. CMake не менялся. Только C17.
   Про CRT: на Windows **все** модули движка наследуют
   `laiue_common` → `laiue::windows_no_crt` (`cmake/LaiueToolchain.cmake:396-397`),
   то есть `/NODEFAULTLIB` и собственные `memset`/`memcpy` из
   `src/runtime/memory.c`. Значит, и Vulkan-бэкенд на Windows собирался бы
   по тому же no-CRT контракту. На Linux/macOS/portable-профилях такого
   запрета нет — `laiue_windows_no_crt` применяется только при
   `LAIUE_PLATFORM_WINDOWS`; для не-Windows ветки это обычный CRT. Правка
   добавляет лишь `memset`/`memcpy`, которые в файле уже использовались,
   так что новых зависимостей от CRT нет ни там, ни там.
   Предупреждения-как-ошибки включены по умолчанию:
   `option(LAIUE_WARNINGS_AS_ERRORS ... ON)` (`LaiueToolchain.cmake:3`) и
   `/W4 ... /WX` (`:208-209`, `:319-320`).
3. **Нехватка памяти.** Три точки отказа, как в D3D12:
   - `BufferCreate` крупной арены падает → `EnsureVulkanLargeMeshBuffer`
     возвращает `false` → идём в ветку 3 (отдельный буфер), а не роняем
     кадр;
   - `BufferCreate` отдельного буфера падает → `PoolFree` и `return NULL`
     (стр. 1874-1879);
   - `PlatformAllocate` под `RendererMesh` падает → освобождается
     `ownedStaging`, откатывается смещение нужного кольца
     (`largeMeshUploadOffsets` при `usedLargeRing`, иначе
     `meshUploadOffsets`) и `PoolFree`, затем `return NULL`
     (стр. 1887-1896). Кадр и рендер не роняются.

## 5. Что осталось не проверено и почему

- **Сборка/исполнение Vulkan.** Нет SDK, см. раздел 1; CMake-цель
  не конфигурируется, `vulkan/vulkan.h` отсутствует. Замера времени нет,
  таблиц до/после по образцу `render_ring_parallel_work.md` нет — измерять
  нечего.
- **Компиляторная проверка синтаксиса `cl.exe /c ... /W4 /WX`.** То же:
  без заголовков Vulkan файл не компилируется. Делать вид, что проверил,
  подсовывая фиктивный `vulkan.h`, значило бы проверять вымышленный API.
- **Геометрия на пикселях.** Даже в D3D12-отчёте это названо дырой
  контроля; в Vulkan добавился бы ещё и оффскрин-захват, которого здесь
  запустить нельзя.
- **Эквивалентность по чтению, а не по счётчику.** Счётчика созданных
  Vulkan-буферов на кадр (аналог `CreateCommittedResource`) здесь нет и
  исполнить его нельзя. Вместо числа — таблица 3.4: ветка, где D3D12
  создаёт ресурс на каждый переполнивший меш, в Vulkan заменена на общую
  арену с теми же условиями входа.

## 6. Сборка и тесты (D3D12, единственное, что запускается)

```
build\peer\setup.bat                 → SETUP-OK
cmake --build --preset windows-msvc-release --parallel 4
ctest  --preset windows-msvc-release --no-tests=error
100% tests passed out of 30
Total Test time (real) = 11.86 sec
CTEST_EXITCODE=0
```

`30/30` совпадает с заявленным «сейчас 30/30». Как показано в разделе 2,
Vulkan-файл в эту сборку не входит, поэтому она не является проверкой
самой правки — только подтверждением, что дерево осталось здоровым.

## 7. Изменённые строки

- `#define LARGE_MESH_UPLOAD_BYTES_PER_FRAME` — стр. 57.
- Поля `largeMeshUploadBuffers` / `largeMeshUploadOffsets` — стр. 250-251.
- `EnsureVulkanLargeMeshBuffer` — стр. 1811-1820.
- `RendererCreateMesh` (порядок «малое кольцо → крупная арена → отдельный
  буфер», откат OOM) — стр. 1839-1896.
- Сброс `largeMeshUploadOffsets` в `RendererBeginFrame` — стр. 2079.
- `BufferDestroy` крупной арены в `RendererDestroy` — стр. 1772.

Публичный `renderer.h`, `renderer_offscreen.h`, CMake, шейдеры и тесты не
трогались.
