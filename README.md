# laiue

## Сборка

```cmd
cmake -B build/msvc -G "Visual Studio 17 2022" -A x64
cmake --build build/msvc --config Release
```

Требования: Visual Studio 2022, Windows SDK, CMake ≥ 3.20.

## Архитектура

```
laiue.exe
├── laiue_core.dll
│   ├── laiue_window.dll
│   └── laiue_input.dll
```

## Лицензия

MIT
