@echo off
rem Сборка примера из Developer Command Prompt for VS (x64).
rem Мод не линкуется с игрой — нужен только заголовок из sdk.
cl /nologo /W4 /O2 /utf-8 /LD /I..\.. auto_bridge.c /Fe:auto_bridge.dll
if errorlevel 1 exit /b 1

rem Раскладка мода: mods\auto_bridge.lmp\{mod.lm, auto_bridge.dll}
echo.
echo Готово. Скопируйте auto_bridge.dll и mod.lm в
echo   ^<игра^>\mods\auto_bridge.lmp\
echo и включите мод на вкладке "Моды".
