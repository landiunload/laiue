# Пиксельный oracle Vulkan instance-кольца

Независимая задача `ringpixels`. Зона — только
`tests/renderer_vulkan_instance_ring_test.c` и этот отчёт. Production-
исходники, CMake и публичный ABI не менялись. Реальный GPU-прогон
(NVIDIA GeForce RTX 4060), benchmark не выполнялся.

## 1. Что было не так с прежней проверкой

Прежний `renderer_vulkan_instance_ring_test.c` подавал всем четырём draw
один и тот же массив из `200000` инстансов и сверял только счётчики
`drawCalls`/`drawnQuads`. Такой тест не различает:

- ошибку `dynamicOffset` при привязке instance-чанка: все вызовы читают
  один и тот же участок и рисуют одинаково, счётчики при этом целы;
- повторное связывание одного буфера для нового чанка: счётчики растут,
  но геометрия берётся не из того буфера;
- перезапись уже записанных instance bytes при возврате курсора назад
  внутри кадра.

Счётчики здесь ничего не доказывают про пиксели: доказательство даёт
чтение кадра. Compile-only тоже не считается прогоном.

## 2. Изменения относительно входного snapshot

| файл | что |
| --- | --- |
| `tests/renderer_vulkan_instance_ring_test.c` | переписан: пиксельный oracle через `RendererCaptureFrame`, сценарии с probe/поворотами/балластом, resize, оба кадровых слота |
| `docs/ds41_r2_ringpixels.md` | этот отчёт |

`tests/CMakeLists.txt` не менялся: цель `laiue_renderer_vulkan_instance_ring_test`
и тест `laiue.render.vulkan_instance_ring` уже зарегистрированы. Полная
сверка по `build/input-snapshot/manifest.json` (301 файл) даёт ровно одно
отличие — тестовый файл; итоговый SHA256 теста
`ebe04969d4b582d2521325981b310647f1284192b131c592ab1e88355daacc81`.
`src/render/renderer_vulkan.c` побайтово равен snapshot
(`6f74fa39c7d9222f951da0b3372c30dfb93ba8b2dc3655b79aba149e981c9e19`).

## 3. Устройство oracle

Кадр сверяется не по golden hash (он драйвер-зависим), а по маске занятых
слотов и площади геометрии.

- **Слоты.** Кадр делится на сетку 3×2 в NDC (три столбца по `-0.66/0/+0.66`,
  две строки по `+0.45/-0.45`, размер слота `0.35`). Ядро слота — центральные
  50%: края растеризации в решение не входят.
- **Probe.** Каждый видимый вызов несёт один инстанс единичного меша
  (грань +Z, материал 1). `origin` и `scale` кладут его ровно в слот. Небо —
  чистый красный; всё, что не небо, — геометрия. Пиксель-в-пиксель цвета
  материала не проверяются.
- **Повороты.** Probe средних слотов повёрнуты на 180° вокруг Z
  (кватернион `(0,0,1,0)`); их `origin` выбран в правом верхнем углу слота,
  поэтому квадрат всё равно покрывает слот, а потеря поворота уводит его за
  слот и ломает маску. Identity задана корректно как `(0,0,0,1)`.
- **Балласт >16 МиБ.** Лишние инстансы (2×300000 = 18.3 МиБ либо
  1×560000 = 17.1 МиБ) расположены за пределами clip space (`origin` ~1000):
  они переводят кольцо на новые чанки и нагружают вершинный шейдер, но не
  дают фрагментов и не создают overdraw.
- **Оба кадровых слота** покрываются прогоном 12 кадров: курсоры и чанки у
  слотов свои.

Сценарии (`STEP_PROBE slot rotated` / `STEP_BALLAST count`), порядок и
величина нагрузки меняются по кадрам:

| сценарий | порядок | probe-слоты |
| --- | --- | --- |
| `probes-then-split` | probe 0/1/2 → 300k → 300k → probe 3/4/5 | 0,1,2,3,4,5 |
| `ballast-then-split` | 300k → probe 0/1 → 300k → probe 2/3/4/5 | 0,1,2,3,4,5 |
| `single-big-subset` | probe 0/2 → 560k → probe 4 | 0,2,4 |
| `reverse-subset` | probe 5/4 → 560k → probe 0/1 | 0,1,4,5 |

Подмножества важны отдельно: пустые слоты обязаны остаться небом, то есть
«следов» probe из соседних кадров и лишних занятых слотов быть не должно.

Число вызовов и инстансов (`drawCalls`, `drawnQuads`) проверяется как
вторичный контракт «вызовы не потеряны», но пиксельное доказательство —
маска и площадь, а не счётчики.

## 4. Воспроизведение

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset windows-msvc ^
  -DVulkan_INCLUDE_DIR=C:/Users/landi/vulkan-sdk-user/include ^
  -DVulkan_LIBRARY=C:/Users/landi/vulkan-sdk-user/Lib/vulkan-1.lib
cmake --build --preset windows-msvc-release --parallel 1
ctest --preset windows-msvc-release --output-on-failure
cmake --build --preset windows-msvc-debug --parallel 1
ctest --preset windows-msvc-debug --output-on-failure
```

Один тест:

```bat
build\windows-msvc\bin\Release\laiue_renderer_vulkan_instance_ring_test.exe
```

Стенд и протоколы собраны в `build/ds41-r2-ringpixels/stand.md`
(`build/` игнорируется git).

## 5. Фактические результаты

- Release, прямой прогон: `Vulkan instance ring pixel checks passed`, exit 0.
- Release CTest: `100% tests passed out of 39`.
- Debug CTest: `100% tests passed out of 39`.
- `git diff --check`: чисто. `tools/check_architecture.ps1`:
  `Architecture boundaries: OK (105 files)`.

## 6. Фальсификация (временная, в своей копии)

Обе поломки внесены в `src/render/renderer_vulkan.c` на время, затем
исходник восстановлен по snapshot (SHA256 совпал).

**A. Игнорирование dynamic offset** — вызов
`DrawMeshInternal(..., chunkIndex, offset)` заменён на
`DrawMeshInternal(..., chunkIndex, 0u)`:

```
Vulkan instance ring check failed: probes-then-split slot mask expected 63
got 9 geometry pixels 242 expected area 726   (exit 1)
```

Маска `9 = 0b001001`: выжили только первые probe чанков 0 и 3 — ровно то,
что даёт чтение по смещению 0 в каждом чанке.

**B. Перепривязка к буферу чанка 0** — в `DrawMeshInternal`
`block->sets[instanceChunkIndex][frameIndex]` заменён на
`block->sets[0][frameIndex]`:

```
Vulkan instance ring check failed: probes-then-split slot mask expected 63
got 7 geometry pixels 374 expected area 726   (exit 1)
```

Маска `7 = 0b000111`: probe поздних чанков прочитали данные чанка 0.

Оба варианта падают на первом же сценарии, то есть проверка чувствительна
именно к правке кольца, а не к шуму.

## 7. Ограничения и что не проверено

- **Проверялись только Windows + Vulkan + NVIDIA RTX 4060.** Linux, macOS,
  ARM64 и другие драйверы не запускались; результат одного драйвера не
  переносится на другой.
- **Oracle — coarse.** Это маска 6 слотов плюс порядок площади, а не
  суб-пиксельная сверка и не точный поворот. Он ловит промах offset,
  перепривязку буфера, потерю/дубликат probe и «следы» соседних кадров, но
  не отличит, например, малое смещение внутри слота.
- **Цветовой контраст — предпосылка.** Геометрия считается «не красным»
  небом. Со встроенным нейтральным материалом (без текстурпака) это
  устойчиво; смена встроенной заглушки на красную сломала бы oracle.
- **Порог 16 МиБ проверяется по нагрузке кадра**, а не по committed-байтам
  адаптивного пула: байты буферов этот тест не измеряет.
- **Команды построены на том, что glslang/glslc не найдены**, поэтому
  используются checked-in fallback-шейдеры `src/render/generated/vulkan/*`.
  Правка `shaders/chunk.hlsl` этим тестом не покрывается.
