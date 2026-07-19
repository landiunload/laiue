# Шейдерпаки

Шейдерпак — каталог `shaders/<name>.lsp` с манифестом:

```text
LAIUE SHADER 1
name = My Pack
contract = 1
```

Движок 0.5.0 поддерживает только contract 1. Пак содержит скомпилированный
DXBC, а не HLSL-текст. Допустим любой непустой набор стадий:

| Файл | Entry/profile | Проход |
|---|---|---|
| `chunk_vs.ls`, `chunk_ps.ls` | `VSMain/PSMain`, `*_5_0` | чанки |
| `panorama_vs.ls`, `panorama_ps.ls` | `VSMain/PSMain`, `*_5_0` | широкая проекция |
| `ui_vs.ls`, `ui_ps.ls` | `VSMain/PSMain`, `*_5_0` | UI |

Отсутствующая стадия берётся из встроенного набора. Максимальный размер файла
— 256 КиБ. Несовместимый или повреждённый активный пак сбрасывается на
встроенные шейдеры, причина показывается в UI. Выбор применяется при
подготовке игровой сессии; главное меню не создаёт игровые GPU-ресурсы.

## Contract 1

Эталонные реализации находятся в `shaders/*.hlsl`.

### Chunk

- `b0`: row-major `viewProjection`, `chunkOriginRelative`, `meshScale`,
  `sunDirection`, `sunColor`, `ambientColor`, `gammaInverse`;
- `t0`: packed quad `ByteAddressBuffer`;
- `t1`: albedo `Texture2DArray`, `t2`: normal/AO `Texture2DArray`;
- `t3`: instance buffer, один `float4 { origin.xyz, scale }`;
- `s0`: sampler.

При `meshScale < 0` vertex shader обязан читать transform по `SV_InstanceID`;
при положительном значении используется `chunkOriginRelative`. Renderer всегда
привязывает корректный `t3`, но чтение следует оставлять под `[branch]`, как в
эталонном `chunk.hlsl`.

### Panorama

`b0` содержит `fovHalfRadians`, `verticalScale`, `mapping`; `t0` —
`TextureCube`, `s0` — sampler. `mapping`: 0 — equidistant fisheye, 1 —
цилиндрическая проекция.

### UI

`b0` содержит `float2 screenSize`; `t0` — квады по 48 байт, `t1` — атлас
шрифта, `t2` — статичный фон, `s0` — sampler. У квада flag 1 выбирает альфу
шрифта, flag 2 — фоновую текстуру. Точная раскладка записана в
`shaders/ui.hlsl` и `RendererUiQuad`.

## Компиляция

Используйте `fxc.exe` с `/O3 /Qstrip_debug /Qstrip_reflect`:

```bat
fxc /nologo /T vs_5_0 /E VSMain /O3 /Qstrip_debug /Qstrip_reflect /Fo chunk_vs.ls chunk.hlsl
fxc /nologo /T ps_5_0 /E PSMain /O3 /Qstrip_debug /Qstrip_reflect /Fo chunk_ps.ls chunk.hlsl
```

Встроенные шейдеры CMake компилирует теми же параметрами; если `fxc` не
найден, используются закоммиченные заголовки с байткодом.
