# Формат сохранения мира v1

Одиночный мир хранится в `saves/<slot>` рядом с `laiue.exe`:

```text
world.meta     seed, время суток, версия игры
chunks.dat     пользовательские правки блоков
player.dat     позиция, взгляд и режим
inventory.dat  36 слотов и выбранная ячейка хотбара
mods.lock      включённые моды и их версии
moddata/       по одному блобу на мод
```

Основные файлы записываются через соседний `.tmp`, `FlushFileBuffers` и
`MoveFileExW`; многобайтовые числа little-endian. Повреждённый необязательный
файл игнорируется, не блокируя загрузку остальных частей. `moddata` пишет сам
мод через API и обязан обеспечивать нужную ему атомарность самостоятельно.

## `world.meta`

```text
LAIUE WORLD 1
seed = 42
time_minutes = 618
game = 0.5
```

`time_minutes` ограничено диапазоном 0..1439. `game` пока информационно.
Каталог без читаемого `world.meta` не показывается как сохранённый мир.

## `chunks.dat` — LWC1

| Поле | Тип |
|---|---|
| magic, version, reserved | `u32 LWC1`, `u16 1`, `u16` |
| seed | `i64` |
| block origin | 3 × `InfiniteCoord` |
| chunk count | `u32` |
| каждый чанк | 3 × absolute `InfiniteCoord`, `u32` count, `u32[]` deltas |

`InfiniteCoord` — `i32 sign`, `u32 limbCount`, затем `u64` limbs. Delta
содержит 18-битный индекс блока внутри чанка 64³ и 8-битный тип блока.
Seed обязан совпадать. Origin при восстановлении должен помещаться в `int64`;
непредставимые относительно него чанки пропускаются.

## `player.dat` — LWP1

Запись фиксированного размера: magic, version, локальная позиция глаз
`double[3]`, `yaw/pitch` `float` и режим 0 (креатив) или 1 (выживание).
Неизвестная версия оставляет начальные параметры игрока.

## `inventory.dat` — LIV1

Magic, version, выбранный hotbar slot 0..8 и 36 пар `u16 item/u16 count`.
Стек ограничен 64; у пустого слота item и count равны нулю. Неверный файл
заменяется начальным инвентарём.

## `mods.lock` и `moddata`

`mods.lock` — UTF-8 строки `<pack-name> <version>` в порядке
`mods/enabled.txt`. Несовпадение выводит предупреждение, но не запрещает
загрузку мира.

`moddata/<pack-name>.bin` читается и пишется через `readModData`/
`writeModData`; один блоб ограничен 16 МиБ. Подробнее:
[modding.md](modding.md).

## Dedicated server

Сервер загружает `saves/default/chunks.dat` при старте и атомарно сохраняет
его при штатной остановке; серверные моды используют
`saves/default/moddata/`. Время, игроки и их инвентари между запусками пока не
сохраняются. Завершайте сервер через `Ctrl+C`, чтобы отработал graceful
shutdown.
