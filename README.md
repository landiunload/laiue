# laiue

Воксельный каркас под Windows без CRT: крошечный лаунчер `laiue.exe`
загружает `laiue_core.dll`, ядро создаёт окно, подключает raw input,
генерирует мир чанками и рисует его через Direct3D 12.

## Сборка

Требования: любая Visual Studio с C-тулчейном (CMake сам берёт самую новую
установленную), CMake ≥ 3.28. Для ninja-пресетов — Ninja,
для clang-пресетов — LLVM (clang-cl + lld-link).

```cmd
:: последняя установленная Visual Studio (сейчас — VS 2026)
cmake --preset visual-studio
cmake --build --preset visual-studio-release

:: Ninja + MSVC (из Developer Command Prompt)
cmake --preset ninja-msvc-release
cmake --build --preset ninja-msvc-release

:: Ninja + clang-cl
cmake --preset ninja-clang-release
cmake --build --preset ninja-clang-release
```

## Архитектура

```
laiue.exe                 — лаунчер без CRT, только LoadLibrary + Start
└── laiue_core.dll        — цикл кадров, камера, стриминг чанков
    ├── laiue_window.dll  — окно, цикл сообщений, mouse look, время
    ├── laiue_input.dll   — raw input: клавиатура и мышь
    ├── laiue_world.dll   — воксельный мир: шум, чанки-дельты, кеш высот
    ├── laiue_mesher.dll  — бинарный greedy meshing (мир -> геометрия)
    └── laiue_render.dll  — Direct3D 12, GPU-резидентные меши
```

Все модули собираются без CRT (`/NODEFAULTLIB`), DLL линкуются с `/NOENTRY`.
Состояние — непрозрачные экземпляры (`Window*`, `Input*`, `World*`,
`Renderer*`), глобальных переменных нет. Рендерер не знает о мире,
мир не знает о геометрии; их связывает модуль mesher.

Конвейер чанков: пул рабочих потоков строит меши бинарным greedy meshing
(колонна чанка 64 блока = один uint64, слияние граней — битовые маски),
квад упакован в 8 байт и разворачивается вершинным шейдером по
SV_VertexID (vertex pulling — без вершинных и индексных буферов),
цвет и затенение восстанавливаются в пиксельном шейдере. Меши
суб-аллоцируются из общих DEFAULT-буферов GPU (staging-загрузка,
отложенное освобождение под fence), рисуются спереди-назад с отсечением
по пирамиде видимости. Загрузка — с бюджетом на кадр и гистерезисом
радиуса. Origin rebasing: камера двигается в double, рендер работает
в координатах относительно неё — мир не деградирует вплоть до пределов
int64 (±9,2·10¹⁸ блоков).

Новый модуль объявляется в CMake одной командой:

```cmake
laiue_add_module(имя SOURCES <файлы...> [LINK <библиотеки...>])
```

## Управление

- `Esc` — выход
- `W A S D` — полёт камеры, `Space` — вверх
- `F7` — включить mouse look, `Shift+F7` — выключить
- `V` — переключить вертикальную синхронизацию
- `ЛКМ` — сломать блок, `ПКМ` — поставить (при захваченной мыши)

## Лицензия

MIT
