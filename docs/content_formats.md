# Форматы содержимого

| Назначение | Одиночный формат | Пак | Runtime-каталог |
|---|---|---|---|
| моды | `.lm` | `.lmp` | `mods/` |
| шейдеры | `.ls` | `.lsp` | `shaders/` |
| текстуры | `.lt` | `.ltp` | `textures/` |

`.lr/.lrp` и `.ld/.ldp` не поддерживаются. Форматы не взаимозаменяемы.
Имена проверяются до построения пути: запрещены абсолютные пути,
разделители, `..`, управляющие и зарезервированные Windows-имена.

## Моды

Runtime-мод — каталог `mods/<name>.lmp`:

```text
my_mod.lmp/
  mod.lm
  my_mod.windows-x86_64.dll
  my_mod.linux-x86_64-gnu.so
  my_mod.linux-x86_64-musl.so
```

Минимальный UTF-8-манифест:

```text
LAIUE MOD 2
id = author.my_mod
name = My Mod
version = 1.0.0
game = 0.5
side = both

[native]
entry_windows_x86_64 = my_mod.windows-x86_64.dll
entry_linux_x86_64_gnu = my_mod.linux-x86_64-gnu.so
entry_linux_x86_64_musl = my_mod.linux-x86_64-musl.so
api = 1
```

Можно объявить подмножество платформ, но каждый объявленный файл обязателен.
Полный контракт и правила fingerprint — в [modding.md](modding.md).

## Шейдеры

Шейдерпак — каталог `shaders/<name>.lsp` с обязательным `pack.lm` и
непустым подмножеством DXBC-файлов:

```text
pack.lm
chunk_vs.ls       chunk_ps.ls
panorama_vs.ls    panorama_ps.ls
ui_vs.ls          ui_ps.ls
```

Отсутствующие стадии берутся из встроенного набора. Контракт — в
[shaderpacks.md](shaderpacks.md).

## Текстуры

`.ltp` — бинарный GPU-ready текстурпак LTP1/LTP2. `.lt` зарезервирован и
пока не имеет runtime-семантики. Точный формат — в
[texturepacks.md](texturepacks.md).

## Активный выбор

`shaders/active.txt` и `textures/active.txt` содержат UTF-8-имя одного
активного пака. Пустой/отсутствующий файл означает встроенный набор.
Шейдеры и текстуры загружаются при подготовке игровой сессии и могут быть
переключены из настроек.

Моды используют не `active.txt`, а ordered-файлы `mods/enabled.txt` и
`mods/server_enabled.txt`.

## Сетевой bundle

Writer создаёт `LCB2`: все относительные пути — canonical UTF-8 с `/`,
entries отсортированы детерминированно. Сборка отклоняет symlink/reparse
points, traversal, duplicate и case-collision, даже на case-sensitive Linux.
Installer читает LCB1 для совместимости и LCB2, но новые bundles всегда v2.
Размеры, число файлов и SHA-256 проверяются до переключения staging-каталога.

## Встроенные примеры

- моды: пять `.lmp` из `sdk/examples/`;
- шейдеры: `photon.lsp`, `vibrant.lsp`;
- текстуры: `laiue_32x.ltp`, `laiue_soft_16x.ltp`, `patrix_32x.ltp`.
