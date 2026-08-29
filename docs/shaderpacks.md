# Шейдерпаки

Шейдерпак — каталог `shaders/<name>.lsp` с UTF-8-манифестом:

```text
LAIUE SHADER 1
name = My Pack
contract = 1
```

Contract 1 принимает скомпилированный DXBC, а не HLSL-текст. Допустим любой
непустой набор стадий:

| Файл | Entry/profile | Проход |
|---|---|---|
| `chunk_vs.ls`, `chunk_ps.ls` | `VSMain/PSMain`, `*_5_0` | чанки |
| `panorama_vs.ls`, `panorama_ps.ls` | `VSMain/PSMain`, `*_5_0` | panorama |
| `ui_vs.ls`, `ui_ps.ls` | `VSMain/PSMain`, `*_5_0` | UI |

Отсутствующая стадия берётся из встроенного набора. Максимальный размер
одного файла — 256 КиБ. Renderer сначала проверяет весь набор и создаёт
pipeline replacement; повреждённый или несовместимый pack не заменяет уже
рабочий pipeline.

Это все стадии, которыми владеет renderer 0.7. Пак не может произвольно
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

## Contract 1

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

Используйте `fxc.exe` с `/O3 /Qstrip_debug /Qstrip_reflect`:

```bat
fxc /nologo /T vs_5_0 /E VSMain /O3 /Qstrip_debug /Qstrip_reflect /Fo chunk_vs.ls chunk.hlsl
fxc /nologo /T ps_5_0 /E PSMain /O3 /Qstrip_debug /Qstrip_reflect /Fo chunk_ps.ls chunk.hlsl
```

CMake компилирует встроенные стадии теми же параметрами. Если `fxc` не
найден, используются checked-in fallback headers. Их обновление выполняется
явно и всегда проверяется через `git diff`.
