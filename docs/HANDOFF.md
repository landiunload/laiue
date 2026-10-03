# Передача работы (ветка `claude/fervent-turing-yyrmzl`)

Записка для следующего исполнителя: что сделано, что в процессе, что
осталось, и где подводные камни. Правила проекта — в `CONTRIBUTING.md` и
`CLAUDE.md`. Главные из них: ответы пользователю на русском; на каждое
исправление нужен регрессионный тест, который до исправления падает; у
оптимизаций — замеры «до/после»; изменённые строки прогоняются через
`clang-format` (`git clang-format HEAD`); в коммитах и коде нет
идентификаторов моделей. Draft-PR уже открыт для этой ветки.

## Запросы пользователя (по порядку)

1. Каждый модуль автономен: удалили модуль — пропала его функция, игра
   работает без ошибок. Проверить разбиение на модули и поддержку платформ.
   **Сделано**: матрица удаления модулей (`tests/module_removal_matrix_test.c`),
   проверка ELF-импортов (`tools/check_elf_imports.cmake`), исправления в
   `module_host.c` (частичный профиль), walk-пример деградирует без любого
   модуля.
2. Решить, отдельный модуль звука или общий; добавить нужные модули;
   бесконечный мир без вокселей и чанков, на мешах; пак моделей; заложить
   паки анимаций.
   - Звук: системный вывод остаётся в `audio_output`, добавлены AAudio
     (Android) и CoreAudio/AudioUnit (macOS/iOS). **Сделано.**
   - Пак моделей `laiue.model` (`.lo`/`.lop`, импорт OBJ, кэш `.obj.lo`,
     конвертер `laiue_modelc`, fuzz). **Сделано**, см. `docs/modelpacks.md`.
   - Мир на мешах `laiue.mesh_world`. **Сделано и запушено** (коммит
     43f00f8), см. `docs/mesh_world.md`.
   - Адаптер отрисовки `laiue.mesh_world_render`: **код написан и
     собирается, но тестов ещё нет** (см. ниже).
   - Паки анимаций: **не начато.**
3. «Все модули мультиплатформенные, без модулей вида android/windows;
   проверить, что нигде нет ошибок и предупреждений; доделать задачи».
   Модулей с именем ОС нет. Полная проверка предупреждений во всех сборках
   ещё не прогнана (см. «Осталось»).
4. Вопрос про музыкальный плеер: ответ дан. Модули уже позволяют собрать
   плеер без мира и графики, но не хватает потокового воспроизведения,
   паузы, перемотки и позиции. **Пообещано добавить** (задача ниже).
5. Вопрос про бесконечный мир по любой оси: ответ дан. Добавлен поставщик
   ячеек `setProvider`/`stream` в `laiue.mesh_world`. **Сделано.**

## Состояние кода

### Готово и проверено

- `src/simulation/mesh_world/`: сервис `LaiueMeshWorldServiceV1`.
  Разреженные ячейки int64 + смещение float, ±2⁶¹ ячеек, формы (габарит и до
  64 OBB), хэндлы с поколением, ревизии ячеек (общий счётчик, переживают
  rebase), `queryCells`, `cellInstances`, `sweepBox` (SAT по 15 осям, выход
  из пересечения), `moveBox` (скольжение, зазор 1 мм, опора при normal.z >
  0.7), `raycast`, `overlapBoxes` (под `VoxelDynamicColliderQuery`),
  `blockSolid` (под `VoxelBlockPhysicsQuery`), `rebase`, поставщик ячеек
  `setProvider`/`stream` (бюджет, ближние первыми, гистерезис в одну ячейку,
  инкрементальный проход по новому слою), границы занятых ячеек для
  отсечения диапазона. Тест `laiue.mesh_world.queries`
  (`tests/mesh_world_test.c`) с проверкой на мутациях: обратный сдвиг в
  хэш-таблице, reach, оси ребро×ребро, поколения хэндлов, сортировка, зазор,
  скольжение, выход из пересечения.
- Графическое устройство (`src/graphics/render/module_entry.c`): ёмкость
  хэндлов поднята с 256 до 4096 (`DEVICE_HANDLE_CAPACITY`). Регрессия в
  `tests/optional_modules_test.c`: «graphics device holds a thousand
  buffers». До исправления тест падал, после — проходит (Vulkan offscreen,
  lavapipe).

### Написано, собирается, без тестов: `src/graphics/mesh_world_render/`

Контракт: `mesh_world_render_service.h`. Реализация:
`mesh_world_render.c`.
- `create(config{worldService, world, device V2, maximumBuffers})`.
- `registerModel(model, {vertices 36 байт как LaiueModelVertexV1, indices,
  parts{firstIndex, indexCount, material id}})` копирует геометрию.
- `setMaterial(material, texture, sampler)`: хэндлы устройства, 0 — белая
  текстура.
- `setLighting(toSun, sunColor, ambient)` запекается в цвет вершин.
- `update(camera, radius, cellBudget, &pending)`: `queryCells` →
  merge-join с отсортированным кэшем → пересборка изменённых ячеек
  (ревизия или generation) ближними первыми → по буферу на материал в
  ячейке, вершины `LaiueGraphicsVertexV2` в системе ячейки, без индексов.
- `draws(renderOrigin, viewProjection|NULL, items, cap, &count)`: элементы
  `LaiueGraphicsDrawItemV2` с originRelative = (ячейка − origin)·cellSize −
  origin.local, отсечение по frustum (row-major, clip = pos·M, глубина
  0..1, как в `scene/math.c`).
- `stats`.
- Модуль подключён в корневом `CMakeLists.txt` вместе с mesh_world
  (`LAIUE_BUILD_MESH_WORLD`) и собирается во всех профилях: ему нужен
  только заголовок графического API.

**Что сделать дальше по адаптеру:**
1. Тест `tests/mesh_world_render_test.c` с подставным
   `LaiueGraphicsDeviceV2`: устройство записывает createBuffer, upload,
   destroy, считает живые буферы. Проверить: число батчей = числу
   материалов в ячейке; пересборка только при смене ревизии; освобождение
   ушедших из радиуса ячеек (живых буферов 0 после destroy); бюджет
   `cellBudget` и `pending`; `maximumBuffers` → `overBudget`; запекание
   света (грань к солнцу ярче грани от солнца; ambient); originRelative
   после rebase; отсечение frustum (камера смотрит в сторону — `culled`);
   смену `setLighting` → пересборка всех ячеек. Зарегистрировать в
   `tests/CMakeLists.txt` рядом с `laiue.mesh_world.queries`, линковать
   `laiue_mesh_world` и `laiue_mesh_world_render`.
2. Добавить `L"mesh_world_render"` в список имён
   `tests/module_removal_matrix_test.c`.
3. Документ: раздел «Отрисовка» в `docs/mesh_world.md` или отдельный
   `docs/mesh_world_render.md`, строку в README и
   `docs/module_architecture.md`.
4. Известные упрощения: при нехватке `maximumBuffers` дальние уже
   построенные ячейки не вытесняются ради ближних; фасад устройства держит
   CPU-копию каждого буфера (особенность `render/module_entry.c`).

## Осталось (задачи)

1. **Тест и документы адаптера отрисовки** (выше).
2. **Демо `examples/mesh_world`**: модели из пака (`laiue.model`, OBJ),
   поставщик ячеек с плиткой земли и деревьями, игрок через `moveBox`,
   камера, `mesh_world_render` поверх устройства V2. Проверить под Xvfb +
   lavapipe (сборка `linux-vulkan-x11-walk` — пример, как собирается walk).
   В headless-режиме демо должно отрабатывать N кадров offscreen и
   выходить с кодом 0, чтобы его можно было добавить в CTest.
3. **Потоковый звук** (обещано пользователю): в `src/audio` голос,
   который декодирует WAV/MP3 на лету, а не весь трек в память (сейчас
   `AudioClip` хранит весь int16 PCM: 5 минут стерео ≈ 53 МБ). Плюс
   pause/resume, seek и позиция воспроизведения. Добавлять в хвост
   `LaiueAudioServiceV1` как необязательные поля (проверка по structSize).
   Сейчас у голоса есть только play/stop/volume/pan/speed/looping
   (`src/audio/audio.h`, `audio_service.h`). Нужны тесты на offscreen-бэкенде
   (`audio_offscreen.c`).
4. **Основа паков анимаций**: `src/assets/animation` (в
   `tools/check_architecture.cmake` уже есть `allowed_animation` и
   canonical prefix; форматы `.lk`/`.lkp` и каталог `animations/` уже
   объявлены в `src/assets/content/content_format.*`, `api.h` содержит
   `LAIUE_ANIMATION_API`). Задумано: каналы TRS, скалярные каналы и
   flipbook (кадры текстуры), сэмплер с интерполяцией, поза скелета,
   CPU-скиннинг. Анимированные GIF остаются в текстурпаках, а анимация
   может управлять кадром текстуры. Нужны кодек по образцу `media/model.c`
   (inspect → буферы вызывающего → decode, без аллокаций и без CRT), тесты,
   fuzz (`tests/fuzz`), документ.
5. **Проверка предупреждений во всех сборках**: Linux GCC, Clang, ASan,
   musl, ARM64 (кросс), fuzz (`--target laiue_fuzzers`), Vulkan offscreen и
   x11-walk, Android NDK r29 (`android-ndk-r29` лежит в scratchpad прошлой
   сессии; путь в CI — `.github/workflows/build.yml`). Windows MSVC,
   clang-cl, macOS и iOS — через CI. MSVC `/W4`: избегать C4232 (адрес
   dllimport в статическом инициализаторе) и C4221.
6. **Отчёт о платформах**: модулей с именем ОС нет. Окно и ввод есть
   только Win32 и X11; на macOS, Android, iOS и Wayland их нет.
   Провайдер D3D12 — только Windows (это свойство API). Звук: WASAPI, ALSA,
   AAudio, CoreAudio.
7. **README**: строка `window`, `input` устарела («Windows-окно и Raw
   Input»), а X11 уже есть. Обновить.
8. **PR**: обновить описание draft-PR (mesh_world, render-адаптер,
   ёмкость устройства), следить за CI.

## Как собирать и проверять

```sh
cmake --preset linux-gcc && cmake --build build/linux-gcc --config Release -j8
ctest --test-dir build/linux-gcc -C Release -j8          # 99 тестов, 1 пропуск (нет звуковой карты)
cmake --preset linux-gcc-asan && cmake --build build/linux-gcc-asan -j8
ctest --test-dir build/linux-gcc-asan -C Debug -j8
cmake --preset linux-clang && cmake --build build/linux-clang --config Release -j8
cmake --preset linux-vulkan-offscreen && cmake --build build/linux-vulkan-offscreen --config Release -j8
ctest --test-dir build/linux-vulkan-offscreen -C Release -j8   # нужен lavapipe
```

Подводные камни:
- Модули без CRT: нет `strcmp`, `printf`, `qsort`, libm. Математика —
  `math/scalar.h` (`ScalarSqrt` и др.), память — `PlatformAllocate`.
- `PlatformReallocate(..., zeroNewMemory)` на POSIX новую память **не
  обнуляет** (на Windows обнуляет). Не полагаться на это.
- `-Wpedantic` в C17 запрещает передавать `float[3][3]` в параметр
  `const float[3][3]`: параметры-матрицы объявлять без `const`.
- `LaiueModuleHostQueryService` принимает 6 аргументов (последние два —
  `outVersion`, `outSize`, можно `NULL`).
- В модулях из списка `portable_modules`
  (`tools/check_architecture.cmake`) разрешены только стандартные
  заголовки C.
- При запуске libFuzzer первым аргументом передавать рабочий каталог в
  scratchpad, а не корпус из репозитория: иначе новые входы окажутся в
  `tests/fuzz/corpus` (такое уже было и откатывалось).
- Тесты POSIX используют обычный `main`, Windows-тесты — no-CRT entry
  (`tests/test_runtime.h`, `LAIUE_TEST_ENTRY`).
