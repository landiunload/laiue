# laiue

Каркас окна и raw-input под Windows без CRT: крошечный лаунчер `laiue.exe`
загружает `laiue_core.dll`, ядро создаёт окно (`laiue_window.dll`)
и подключает ввод (`laiue_input.dll`).

## Сборка

Требования: любая Visual Studio с C-тулчейном (CMake сам берёт самую новую
установленную), CMake ≥ 3.30. Для ninja-пресетов — Ninja,
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
└── laiue_core.dll        — логика приложения (цикл кадров, горячие клавиши)
    ├── laiue_window.dll  — окно, цикл сообщений, mouse look
    └── laiue_input.dll   — raw input: клавиатура и мышь
```

Все модули собираются без CRT (`/NODEFAULTLIB`), DLL линкуются с `/NOENTRY`.
Состояние окна и ввода — непрозрачные экземпляры (`Window*`, `Input*`),
глобальных переменных нет.

## Управление

- `Esc` — выход
- `F7` — включить mouse look, `Shift+F7` — выключить

## Лицензия

MIT
