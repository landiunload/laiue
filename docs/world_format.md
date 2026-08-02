# Формат сохранения мира v1

Одиночный мир хранится в `saves/<slot>` рядом с `laiue.exe`:

```text
world.meta        seed, время суток, версия игры
chunks.dat.{0,1}  пользовательские правки блоков, два поколения
constructs.dat.{0,1} физические конструкции, те же поколения
state.commit.{0,1} commit records общей пары world + constructs
player.dat     позиция, взгляд и режим
inventory.dat  36 слотов и выбранная ячейка хотбара
mods.lock      включённые моды и их версии
moddata/       по одному блобу на мод
```

Каждый data-файл записывается через соседний `.tmp`, flush и атомарную замену.
Последним атомарно публикуется `state.commit.<slot>`: version, номер slot,
монотонный `u64 generation`, размеры и SHA-256 обоих data-файлов, а также
SHA-256 самого record. Загрузчик выбирает новейшую целую пару; незавершённое
или повреждённое поколение игнорируется с откатом на предыдущее. Если commit
records уже существуют, но целой пары нет, загрузка завершается fail-closed.
До первой новой записи поддерживается legacy-пара без suffix:
`chunks.dat` и необязательный `constructs.dat`.
Многобайтовые числа little-endian.
`moddata` пишет сам мод через API и обязан обеспечивать нужную ему
атомарность самостоятельно.

## `world.meta`

```text
LAIUE WORLD 1
seed = 42
time_minutes = 618
game = 0.5
```

`time_minutes` ограничено диапазоном 0..1439. `game` пока информационно.
Каталог без читаемого `world.meta` не показывается как сохранённый мир.

## `chunks.dat.<slot>` — LWC1

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

## `constructs.dat.<slot>` — LPCS1

Все числа записаны явно little-endian; `double` обязан быть IEEE-754
binary64, а `-0` канонизируется в `+0`.

| Поле | Тип |
|---|---|
| magic, version, header size | `u8[4] "LPCS"`, `u16 1`, `u16 48` |
| body count, file size | `u32`, `u32` |
| digest | `u8[32]` SHA-256 всего payload после header |
| каждое тело | `u64 id`, `u64 topologyRevision`, `double[3] origin`, `double[3] velocity`, `u32 blockCount`, `u32 reserved` |
| каждый блок | `i32[3] local`, `i8[3] mountNormal`, `u8 material`, `u8 kind`, `u8[3] reserved` |

Файл ограничен 16 телами по 256 записей. Voxel material допускает только
earth/grass; lever имеет material 0, local `(0,0,0)` и единичный
`mountNormal`. Локальные координаты обязаны помещаться в wire `int16`, ID и
revision ненулевые, координаты/скорости конечные и bounded. Блоки хранятся в
лексикографическом порядке и образуют одну компоненту; рычаг соединяется
только со своим монтажным voxel. Transient grab owner в файл не попадает.

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

Сервер загружает и сохраняет ту же committed two-slot пару в `saves/default`;
server world и constructs никогда не публикуются по отдельности. Моды используют
`saves/default/moddata/`. Время, игроки и их инвентари между запусками пока
не сохраняются. Завершайте сервер через `Ctrl+C`, чтобы отработал graceful
shutdown.
