# Texture packs

Повреждённый или нечитаемый активный пак не роняет игру: движок загружает
встроенный fallback, сбрасывает ложный выбор из `active.txt` и показывает
причину на вкладке «Текстуры».

Runtime-пак `LTP1` содержит только готовые RGBA8-данные, необходимые GPU.
Игра не распаковывает ZIP и не декодирует PNG при запуске: тяжёлая часть
выполняется один раз скриптом `tools/build_texture_pack.ps1`.

## Выбор активного пака

Рядом с `laiue.exe` должен находиться каталог `textures`:

```text
textures/
  active.txt
  laiue_32x.ltp
```

`active.txt` содержит одно ASCII-имя, например `laiue_32x.ltp`. Первый символ
должен быть латинской буквой или цифрой; далее разрешены также `.`, `_` и `-`.
Путь, диски и подкаталоги запрещены.
Если файл отсутствует, имя небезопасно или LTP повреждён, загрузчик использует
встроенный 1x1 fallback. Текущий пак можно безопасно заменить между запусками.

## Сборка обычного пака

Исходный каталог содержит три финальные квадратные power-of-two PNG одинакового
размера (от 1x1 до 4096x4096):

```text
my_pack/
  dirt.png
  grass_top.png
  grass_side.png
```

```powershell
pwsh -File tools/build_texture_pack.ps1 `
  -Directory .\my_pack `
  -Output .\assets\texturepacks\my_pack.ltp
```

Порядок слоёв стабилен: `0=dirt`, `1=grass_top`, `2=grass_side`.

## Импорт Patrix

Обычный `assets/minecraft/textures/block/grass_block_top.png` в Patrix является
заглушкой Connected Textures. Импортёр берёт настоящие CTM-тайлы из:

- `assets/minecraft/optifine/ctm/patrix/dirt/default/<N>.png`;
- `assets/minecraft/optifine/ctm/patrix/grass/block/top/<N>.png`;
- `assets/minecraft/optifine/ctm/patrix/grass/block/side_default/<N>.png`;
- `assets/minecraft/optifine/ctm/patrix/grass/block/side_overlay/<N>.png`.

Overlay стороны заранее тонируется и смешивается с base; верх травы также
тонируется. Поэтому пиксельному шейдеру достаточно одного texture sample.
По умолчанию используется тайл 1 и цвет `#75AD55` из Patrix grass colormap.

```powershell
pwsh -File tools/build_texture_pack.ps1 `
  -PatrixZip .\Patrix_26.2_32x_basic.zip `
  -Tile 1 `
  -GrassTint '#75AD55' `
  -Output .\assets\texturepacks\patrix_32x.ltp
```

ZIP и исходные PNG в runtime-каталог не копируются. Перед распространением
сгенерированного пака необходимо самостоятельно проверить лицензию исходных
текстур.

## Бинарный формат LTP1

Все числа little-endian. Header занимает ровно 24 байта:

| Offset | Type | Значение |
|---:|---|---|
| 0 | `u32` | magic `0x3150544C` (`LTP1`) |
| 4 | `u16` | version `1` |
| 6 | `u16` | header size `24` |
| 8 | `u16` | width |
| 10 | `u16` | height |
| 12 | `u16` | layer count `3` |
| 14 | `u16` | полное число mip-уровней |
| 16 | `u32` | format `1` (`RGBA8`) |
| 20 | `u32` | размер payload |

Payload идёт по слоям, внутри слоя — от mip 0 до 1x1. Строки хранятся плотно,
`rowBytes = mipWidth * 4`; padding D3D12 в файл не записывается.

Версия 1 содержит только albedo.

## LTP2: albedo + нормали

Версия `2` с форматом `2` добавляет карты нормалей: сразу за payload
albedo идёт второй блок той же раскладки и того же размера (RGBA8,
слои и mip-уровни идентичны). RGB — касательная нормаль (R = +U,
G = вверх текстуры, B — из поверхности), A — ambient occlusion.
Поле «размер payload» в заголовке — суммарный размер обоих блоков.
Паки версии 1 продолжают работать: рендер подставляет плоскую нормаль.

Сборка LTP2 из каталога с `dirt.png`, `grass_top.png`, `grass_side.png`
и парными `*_n.png`:

```powershell
pwsh -File tools/build_texture_pack.ps1 `
  -Directory .\assets\texturepacks_src\laiue32 `
  -IncludeNormals `
  -Output .\assets\texturepacks\laiue_32x.ltp
```

Mip-уровни нормалей после усреднения перенормируются.

## Собственные текстуры laiue

`tools/generate_textures.ps1` детерминированно (параметр `-Seed`)
генерирует бесшовный пиксель-арт: периодический value noise даёт
тайлящиеся карты, палитры квантуют их в стиль 32x32, нормали считаются
из карт высот с wrap-соседями, AO — из высоты. Активный пак
`laiue_32x.ltp` собран именно так и не содержит чужих ассетов.

Анимация `.mcmeta` и схема CTM 6x6 в runtime-формат пока не входят.

## Отложенный roadmap LTP2

Эти пункты зафиксированы как направление следующей версии и пока не входят
в runtime:

- человекочитаемый source-pack: `pack.json`, таблица материалов и PNG;
- отдельная компиляция source-pack в GPU-ready `.ltp`;
- произвольное количество материалов и соответствия
  `block type + face -> material` вместо трёх фиксированных слоёв;
- индекс чанков данных с offsets, размерами, версиями и контрольными суммами;
- BC7 для albedo, BC5 для normal и BC4 для roughness/height;
- normal, roughness, height/emissive и другие независимые каналы;
- несколько texture arrays, сгруппированных по разрешению и DXGI-формату;
- CTM, анимации и metadata без условий для конкретного pack в shader;
- асинхронная горячая смена pack через отдельный resource manager и GPU fence;
- fallback материалов и совместимость старых версий контейнера;
- authoring/import pipeline остаётся вне игры: ZIP и PNG не декодируются при
  каждом запуске.

Физические свойства блоков (например, трение льда) намеренно не являются
частью текстурпака: внешний вид не должен менять gameplay.
