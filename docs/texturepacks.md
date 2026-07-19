# Текстурпаки

Runtime использует GPU-ready `.ltp`: PNG и ZIP в игре не декодируются.
Активное UTF-8-имя хранится в `textures/active.txt`. Это только имя файла:
запрещены разделители пути, управляющие символы, Windows device names и
конечные пробел/точка. Повреждённый пак заменяется встроенным 1×1 fallback,
выбор сбрасывается, а причина показывается в UI.

Пак применяется при подготовке игровой сессии и при смене в настройках.
Главное меню игровые текстуры не загружает.

## Сборка

Исходный каталог содержит три квадратных power-of-two PNG одинакового размера
от 1 до 4096 пикселей:

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

Для LTP2 добавьте `dirt_n.png`, `grass_top_n.png`, `grass_side_n.png` и
параметр `-IncludeNormals`. Генератор собственных бесшовных текстур —
`tools/generate_textures.ps1`.

## Формат LTP

Все числа little-endian. Header занимает 24 байта:

| Offset | Тип | Значение |
|---:|---|---|
| 0 | `u32` | magic `LTP1` (`0x3150544C`) |
| 4 | `u16` | version: 1 или 2 |
| 6 | `u16` | header size = 24 |
| 8, 10 | `u16` | width, height |
| 12 | `u16` | layer count = 3 |
| 14 | `u16` | полная цепочка mip до 1×1 |
| 16 | `u32` | format: 1 или 2 |
| 20 | `u32` | полный размер payload |

Порядок albedo-слоёв стабилен: dirt, grass top, grass side. Внутри слоя mip
идут от исходного размера до 1×1, RGBA8 без D3D12 row padding.

- version 1 + format 1: только albedo;
- version 2 + format 2: после albedo расположен такой же по размеру блок
  normal/AO; RGB — касательная нормаль, A — ambient occlusion.

Loader требует точное число слоёв, полную mip-цепочку, согласованный размер
payload и точный размер файла. Для LTP1 renderer подставляет плоскую нормаль.

## Поставляемые паки

- `laiue_32x.ltp` — собственный 32×32 albedo + normal/AO;
- `laiue_soft_16x.ltp` — облегчённый вариант;
- `patrix_32x.ltp` — пример импортированного стороннего пака.

Импорт Patrix поддерживает `-PatrixZip`, `-Tile` и `-GrassTint`; перед
распространением результата необходимо отдельно проверить лицензию исходных
текстур.

Текстурпак меняет только внешний вид. Трение и другие gameplay-свойства
блоков задаются в `world/block_properties.*`.
