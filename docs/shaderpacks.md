# Шейдерпаки

Шейдерпак — каталог `shaders/<name>.lsp` с UTF-8-манифестом:

```text
LAIUE SHADER 1
name = My Pack
contract = 2
```

Contract 2 принимает скомпилированный байткод, а не HLSL-текст: DXIL Shader
Model 6.0 для D3D12 и SPIR-V для Vulkan. Пак собирается под бэкенд, на
котором его загружают; чужой байткод бэкенд отвергает. Допустим любой
непустой набор стадий:

| Файл | Entry/profile | Проход |
|---|---|---|
| `chunk_vs.ls`, `chunk_ps.ls` | `VSMain/PSMain`, `*_6_0` | чанки |
| `panorama_vs.ls`, `panorama_ps.ls` | `VSMain/PSMain`, `*_6_0` | panorama |
| `ui_vs.ls`, `ui_ps.ls` | `VSMain/PSMain`, `*_6_0` | UI |

Отсутствующая стадия берётся из встроенного набора. Максимальный размер
одного файла — 256 КиБ. Renderer сначала проверяет весь набор и создаёт
pipeline replacement; повреждённый или несовместимый pack не заменяет уже
рабочий pipeline.

Это все стадии, которыми сейчас владеет renderer. Пак не может произвольно
добавить новый render pass, root signature или compute pipeline: для такого
расширения нужен новый versioned renderer contract. Структурированный
`LaiueShaderSet` имеет отдельный slot и bit маски для каждой текущей стадии,
поэтому частичная настройка не требует шести несвязанных параметров.

Обычный путь применения:

1. приложение создаёт явный `LaiueContentCatalog`;
2. `ShaderPackActivateIn` записывает выбранное имя;
3. `RendererReloadShaderPackFrom` загружает pack, создаёт все replacement
   PSO и переключает их одной операцией;
4. при ошибке прежние PSO и скопированный bytecode остаются активными.

`ShaderPackLoadActiveSet` доступен отдельно для editor/предпросмотра. Он
возвращает immutable owned object; renderer копирует bytecode, после чего
объект освобождается через `ShaderPackLoadedSetRelease`.

## Contract 2

Эталонные реализации находятся в `shaders/*.hlsl`.

### Chunk

- `b0`: row-major `viewProjection`, `chunkOriginRelative`, `meshScale`,
  `sunDirection`, `textureLayerCount`, `sunColor`, `ambientColor` и
  `gammaInverse`;
- `t0`: packed quad `ByteAddressBuffer`;
- `t1`: albedo `Texture2DArray`;
- `t2`: normal/AO `Texture2DArray`;
- `t3`: instance buffer, один `float4 { origin.xyz, scale }`;
- `s0`: sampler.

Материал 0 означает воздух. Для материалов 1–255 слой вычисляется как
`material - 1` и ограничивается последним доступным слоем texture array.
`textureLayerCount` всегда не меньше 1.

При `meshScale < 0` vertex shader читает transform по `SV_InstanceID`; при
положительном значении используется `chunkOriginRelative`. Renderer всегда
привязывает корректный `t3`, но чтение должно оставаться под `[branch]`, как
в эталонном `chunk.hlsl`.

`chunkOriginRelative` уже выражен относительно выбранного приложением render
origin. Shader не получает абсолютные или `InfiniteCoord`-координаты.

### Panorama

`b0` содержит `fovHalfRadians`, `verticalScale`, `mapping`; `t0` —
`TextureCube`, `s0` — sampler. `mapping`: 0 — equidistant fisheye, 1 —
цилиндрическая проекция.

### UI

`b0` содержит `float2 screenSize`; `t0` — квады по 48 байт, `t1` — атлас
шрифта, `t2` — фон, `s0` — sampler. У квада flag 1 выбирает альфу шрифта,
flag 2 — фоновую текстуру. Точная раскладка записана в `shaders/ui.hlsl` и
`RendererUiQuad`.

## Компиляция

Подходит DXC с поддержкой Shader Model 6.0. D3D12 принимает только
подписанный DXIL: DXC без встроенного валидатора подписывает его, лишь
когда рядом лежит `dxil.dll`.

```bat
dxc -HV 2021 -O3 -Qstrip_debug -Qstrip_reflect -T vs_6_0 -E VSMain -Fo chunk_vs.ls chunk.hlsl
dxc -HV 2021 -O3 -Qstrip_debug -Qstrip_reflect -T ps_6_0 -E PSMain -Fo chunk_ps.ls chunk.hlsl
```

Для Vulkan сдвиги регистров дают раскладку дескрипторов рендерера:

```sh
dxc -spirv -fspv-target-env=vulkan1.1 -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-s-shift 5 0 \
    -HV 2021 -O3 -T vs_6_0 -E VSMain -Fo chunk_vs.ls chunk.hlsl
```

CMake компилирует встроенные стадии теми же параметрами. Если DXC не
найден, используются checked-in заголовки. Их обновление выполняется явно
и всегда проверяется через `git diff`.

Паки contract 1 с DXBC от `fxc` больше не загружаются: D3D12 не собирает
конвейер из стадий DXBC и DXIL, а частичный пак смешал бы свои стадии со
встроенными.

## Кеш конвейеров

Шейдеры компилируются в DXIL и SPIR-V при сборке, но драйвер ещё раз
переводит их в машинный код GPU, когда создаётся конвейер. Рендерер создаёт
все конвейеры при создании и подготовке мира, а не посреди кадра, поэтому
этот перевод не вызывает рывков в игре. Кеш на диске убирает его и из
загрузки.

`RendererSetPipelineCacheDirectory` до создания рендерера включает кеш:
D3D12 использует `ID3D12PipelineLibrary`, Vulkan — `VkPipelineCache`. Файл
`pipelines-<бэкенд>-<vendor>-<device>.lpc` свой у каждого бэкенда и
устройства. Данные драйвера лежат за заголовком движка: бэкенд,
производитель, устройство, версия драйвера, UUID кеша Vulkan и SHA-256
содержимого. Другой GPU, обновлённый драйвер или порча файла дают состояние
`RENDERER_PIPELINE_CACHE_REJECTED` и пустой кеш, а не ошибку и не чужие
байты в драйвере. Файл пишется атомарно.

`RendererDestroy` сохраняет кеш сам, если появились конвейеры, которых в нём
не было; `RendererSavePipelineCache` сохраняет его сразу, например в конце
экрана загрузки. Имя конвейера D3D12 включает хеш описания и байт-кода,
поэтому изменённый шейдер никогда не загружается под старым именем.
`RendererGetPipelineCacheStatusV1` сообщает состояние, число конвейеров и
сколько из них пришло из кеша (на Vulkan — через creation feedback 1.3).

Замер `laiue_pipeline_cache_test` (Release, создание рендерера и подготовка
мира, 4 конвейера): D3D12 — 245 мс без кеша и 157 мс с кешем, Vulkan —
101 мс и 30 мс; во втором запуске все 4 конвейера взяты из кеша.
