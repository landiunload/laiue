@echo off
rem Сборка примера из Developer Command Prompt for VS (x64).
rem Мод не линкуется с игрой — нужен только заголовок из sdk.
cl /nologo /W4 /O2 /utf-8 /LD /I..\.. auto_bridge.c ^
  /Fe:auto_bridge.windows-x86_64.dll
if errorlevel 1 exit /b 1

powershell -NoProfile -Command ^
  "(Get-Content -Raw -Encoding UTF8 'mod.lm.in').Replace('@LAIUE_MOD_NATIVE_ENTRIES@', 'entry_windows_x86_64 = auto_bridge.windows-x86_64.dll') | Set-Content -NoNewline -Encoding UTF8 'mod.lm'"
if errorlevel 1 exit /b 1

rem Раскладка: mods\auto_bridge.lmp\{mod.lm, platform binary}
echo.
echo Готово. Скопируйте auto_bridge.windows-x86_64.dll и mod.lm в
echo   ^<игра^>\mods\auto_bridge.lmp\
echo и включите мод на вкладке "Моды".
