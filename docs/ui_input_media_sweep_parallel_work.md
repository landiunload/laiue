# Разведка UI, ввода и медиа-декодеров: что из них горячий путь

Работа велась в изолированном дереве `laiue-uisweep`, ветка `peer/uisweep`.
Исходники после разведки не изменены: `git diff` пуст, `src/` и `tests/`
побайтово те же. Всё, что написано для измерения, лежит вне дерева и вне
git — в `build/peer/` (каталог в `.gitignore`). Сборка и тесты —
**30/30** до и после разведки.

Задача была не коснуться трёх каталогов, пропущенных утренней разведкой
[`hotpath_sweep_parallel_work.md`](hotpath_sweep_parallel_work.md):
`src/ui/` (три файла), `src/input/input_windows.c` и `src/media/` (12
файлов декодеров и кодировщиков).

Вывод короткий. На кадровом пути в дереве лежит ровно один файл —
`src/input/input_windows.c`, и в нём нет избыточной работы: каждый его
интерфейс за кадр — это одно сравнение и одно чтение/запись, полная
проверка ввода на кадр стоит **17 нс**. `src/ui/` — публичный API без
единого потребителя в дереве (текущая игра `ui.h` не подключает), поэтому
его цена за кадр (полное меню — 1,53 мкс) не измеряется ни на каком
реальном прогоне. `src/media/` — статическая внутренняя библиотека
форматов, вызываемая только при разборе конкретного файла (загрузка пака
или офлайн-инструмент), в кадровый цикл не входит. Ни одной правки,
которую стоит оставлять, не нашлось.

## 1. Среда и метод

- Windows x64, MSVC 19.44.35228, CMake/Ninja Multi-Config, preset
  `windows-msvc` (Release, AVX2, LTO `/GL`, `/W4 /WX /Ob3`), модули
  `SHARED` (`laiue_platform.cmake:97` требует shared для desktop-бэкендов).
- Сборка и тесты — как в задании: `build/peer/setup.bat`, затем
  `cmake --build --preset windows-msvc-release --parallel 4` и
  `ctest --preset windows-msvc-release --no-tests=error`. Базовое
  состояние — **30/30**.
- «Горячий» здесь означает: вызывается на каждом кадре, на каждом шаге
  физики, при каждом запросе блока или на каждом буфере звука. Признак
  «вызывается только из `PrepareWorld`/загрузчика пака/инструмента» горячим
  не считается.
- Вызовы определялись `grep` по `src/`, `tests/` и `tools/`; отдельно
  проверено, что `ChunkStreamingPump` (`chunk_streaming.c:1280`) и
  `RendererBeginFrame`/`RendererEndFrame` не тянут ни `media/`, ни `ui/`.
- Цена измерена двумя зондами вне дерева, слинкованными с **настоящими**
  `laiue_input.dll` и `laiue_ui.dll`:
  - `build/peer/input_probe.c` — публичный ABI ввода, 20 000 000 вызовов
    на серию, минимум из 15 серий (через указатель на функцию, чтобы
    LTO не выкинул вызовы);
  - `build/peer/ui_probe.c` — `UiBegin`/`UiText`/`UiButton`/
    `UiFontMeasure` и условное меню, 2 000 000 вызовов на серию, минимум
    из 15.
- Оговорка про границу: приложение `simulation-of-sins` лежит вне
  разрешённого дерева и напрямую не читалось; вывод о вводе опирается на
  то, что это прикладные экспорты, которые любое окно-приложение зовёт
  каждый кадр (так сказано и в задании).

## 2. Вердикт по каждому файлу

| файл | горячий? | доказательство |
| --- | --- | --- |
| `src/input/input_windows.c` | **на кадре, но нечего убирать** | ни одного вызова из `src/`, `tests/`, `tools/`; зовётся приложением. Все покадровые входы O(1), полная корзина ввода — 17 нс, см. 3 |
| `src/ui/ui.c` | нет | ни одного вызова `Ui*` вне `src/ui/`; текущая игра `ui.h` не подключает; `UiBegin` перепекает шрифт только при смене масштаба, см. 4 |
| `src/ui/ui_font.c` | нет | `UiFontBake` зовётся только `ui.c:26` при смене `pixelSize`; `UiFontFindGlyph`/`UiFontMeasure` — только из `ui.c`. Кадровой работы после прогрева нет |
| `src/ui/ui_format.c` | нет | форматтеры строк — ни одного вызова в `src/`, `tests/`, `tools/` |
| `src/media/image.c` | нет | `ImageDecode` зовётся только `texture_build.c:213`, `tools/texc/main.c:151`, тестом `texc_test.c` |
| `src/media/png_decode.c` | нет | через `image.c:53`, то есть из `texture_build.c` (сборка пака) |
| `src/media/gif_decode.c` | нет | через `image.c:55` |
| `src/media/jpeg_decode.c` | нет | через `image.c:57` |
| `src/media/inflate.c` | нет | единственный потребитель — `png_decode.c:494` |
| `src/media/wave_decode.c` | нет | через `sound.c:113`, то есть из `audio_pack.c:282` (загрузка звука) |
| `src/media/mp3_decode.c` | нет | через `sound.c:126` |
| `src/media/sound.c` | нет | `SoundDecodeSamples` зовётся только `audio_pack.c:282`, `tools/soundc/main.c:130`, тестами |
| `src/media/la_encode.c` | нет | `SoundEncode`/`SoundEncodedBytes` зовутся `audio_pack.c:439,455`, `tools/soundc` |
| `src/media/lt_encode.c` | нет | `LtEncode`/`LtEncodedBytes` зовутся `texture_build.c:310,327`, `tools/texc`, `texc_test.c` |

CMake подтверждает статус `media`: это `laiue_media_support` **STATIC**
(`src/media/CMakeLists.txt:8`), символы не входят ни в один ABI модулей —
её тянет только загрузка контента и инструменты.

## 3. `input_windows.c`: горячий, но уже упёрся в минимум

Движок сам его не вызывает (`grep` даёт только определения и заголовок), а
любое оконное приложение зовёт каждый кадр. Покадровые входы — это
геттеры состояния и `InputEndFrame`:

- `InputIsKeyDown` (`input_windows.c:215`) — `(uint32_t)key < COUNT` плюс
  чтение `bool`;
- `InputWasKeyPressed` (`:220`), `InputConsumeKeyPress` (`:226`) — то же
  плюс инкремент/декремент счётчика;
- `InputIsMouseButtonDown` (`:238`), `InputWasMouseButtonPressed` (`:243`)
  — то же для кнопок мыши;
- `InputGetMouseDelta` (`:248`) — две записи;
- `InputEndFrame` (`:185`) — сброс двух защёлок и двух дельт.

`InputHandleRawInput` (`:94`) и `InputResetState` (`:196`) на кадровом
пути не стоят: первый — на событие Raw Input, второй — на потерю фокуса.

Зонд `build/peer/input_probe.c`, минимум из 15 серий по 20 000 000
вызовов, через реальный `laiue_input.dll`:

| вызов | нс |
| --- | ---: |
| пустой вызов через указатель (фон) | 1,00 |
| `InputIsKeyDown` | 2,04 |
| `InputWasKeyPressed` | 1,85 |
| `InputConsumeKeyPress` | 1,88 |
| `InputIsMouseButtonDown` | 2,07 |
| `InputWasMouseButtonPressed` | 1,87 |
| `InputGetMouseDelta` | 2,21 |
| `InputEndFrame` | 1,31 |
| **корзина кадра**: дельта + 7 `IsKeyDown` + 2 `WasKeyPressed` + `Consume` + 2 `IsMouseButtonDown` + `WasMouseButtonPressed` + `EndFrame` | **17,07** |

Против кадра в 111–117 мкс из
[`render_profile_parallel_work.md`](render_profile_parallel_work.md:147)
это **0,015 %**. Убирать здесь нечего: тело каждого вызова — это сравнение
с границей и одна-две операции с полем; после инлайна компилятором (в
своей DLL, с LTO) остаётся примерно то же. Единственное, что дороже, —
переход через границу DLL, но он задан публичным ABI, а сигнатуры менять
нельзя. Избыточных повторных чтений или циклов нет: `InputEndFrame`
проходит ровно `INPUT_MOUSE_BUTTON_COUNT` (= 2) элементов.

## 4. `src/ui/`: публичный API без потребителя в дереве

`grep` по `src/`, `tests/` и `tools/` не находит **ни одного** вызова
`Ui*`/`UiFont*`/`UiFormat*` за пределами самого `src/ui/`. Единственное
упоминание `RendererUiQuad` в тестах (`renderer_offscreen_test.c:439`) —
это локальная переменная для рендерера, `ui.c` не вызывается. Текущая
игра `ui.h` не подключает (сказано в задании), поэтому на реальном
прогоне UI-файлы не исполняются вовсе; их «горячесть» доказать нечем.

Для полноты измерена цена, какой она была бы у UI-консьюмера
(`build/peer/ui_probe.c`, реальный `laiue_ui.dll`, атлас `Segoe UI`
1024×109, 162 глифа, минимум из 15 серий):

| вызов | нс |
| --- | ---: |
| `UiBegin` (кеш прогрет, размер тот же) | 3,9 |
| `UiText`, ASCII, 57 символов | 611 |
| `UiText`, кириллица, 13 символов | 181 |
| `UiButton`, метка 13 символов | 236 |
| `UiFontMeasure`, 44 символа | 282 |
| условное меню: панель + 2 строки «подпись/значение» + 3 кнопки | 1 527 |

Это ~1,4 % кадра для меню — не тот случай, когда «удешевить» можно
доказать на реальном прогоне. Замечу: `UiBegin` тяжёл только при смене
масштаба окна — тогда `pixelSize != ui->bakedPixelSize` и вызывается
`UiFontBake` (`ui.c:24`–`:35`); в остальные кадры это 4 нс. Внутри
текстового пути есть известная двойная работа (`UiTextCentered` сначала
меряет, потом рисует; `UiTextCentered`/`UiLabelValueRow` делают по два
прохода `UiFontFindGlyph` на символ), но без потребителя это ровно
«преждевременная оптимизация холодного кода», и по правилам дня время на
неё не тратится.

## 5. `src/media/`: только загрузка ассетов и инструменты

Все потребители `media/` — это разбор конкретного файла:

- **Картинки.** `image.c` (`ImageDecode`, `:46`) диспетчеризует в
  `PngDecode`/`GifDecode`/`JpegDecode`. Его зовёт `texture_build.c:213`
  (сборка текстурного пака), `tools/texc` и `texc_test.c`. Пак строится в
  `TexturePackBuildFrom` (`texture_pack.c:85`), а тот — из
  `TexturePackLoadActiveFrom`, которую рендерер зовёт только в
  `CreateBlockTextureReplacement` (`renderer_d3d12.c:750`, вызов на
  `:756`) из `RendererPrepareWorldFrom` (`:1538`) и
  `RendererReloadTexturePackFrom` (`:2693`). Ни `BeginFrame`, ни
  `EndFrame` пак не трогают.
- **Звук.** `sound.c` (`SoundDecodeSamples`, `:94`) — только из
  `audio_pack.c:282` (`AudioClipLoadMemory`), `tools/soundc` и тестов.
  Микшер (`audio_mixer.c`) работает с уже готовыми клипами и в `media/` не
  заходит.
- **Свои форматы.** `la_encode.c` (`SoundEncode`) — `audio_pack.c:455`;
  `lt_encode.c` (`LtEncode`) — `texture_build.c:327`. Оба — запись при
  сборке пака или в инструменте.
- **`inflate.c`** — единственный потребитель `png_decode.c:494`, то есть
  распаковка конкретного PNG.

`ChunkStreamingPump` (`chunk_streaming.c:1280`) подключает только
`scene/math.h`, `world/world.h`, `render/renderer.h`,
`mesh/chunk_mesher.h`, `platform/system.h` и зовёт `RendererCreateMesh`
(`:1250`, `:1353`) — `media/` там нет. Стриминг подгружает только меши, а
не текстуры/звук.

## 6. Про предел стека 4 КиБ

Отдельного правила сборки про 4 КиБ на кадр в дереве нет (ни в
`cmake/`, ни в `CMakeLists.txt`), но к горячим функциям этих трёх
каталогов ограничение и не относится: покадровые входы `input_windows.c`
и `UiBegin`/геттеры имеют тривиальные локальные кадры без массивов.
Крупное в UI — `RendererUiQuad quads[UI_MAX_DRAW_QUADS]` — лежит в
`UiContext`, то есть в объекте вызывающего, а не на стеке; `UiText`
рекурсии не имеет. Тяжёлые локальные буферы есть только у офлайн-
декодеров (`jpeg_decode.c:1111` держит `JpegDecoder decoder`), но они
исполняются при разборе файла, а не в кадре, и под кадровое правило не
подпадают.

## 7. Проверка

- `ctest --preset windows-msvc-release --no-tests=error` — **30/30** на
  базе и после разведки.
- `git diff` пуст, `git status --short` пуст; зонды и батники лежат в
  `build/peer/` и в git не попадают.
- Публичный API/ABI, CMake, заголовки не трогались; глобальных/TLS-кэшей
  не добавлялось.

## 8. Вывод

Среди `src/ui/`, `src/input/` и `src/media/` ровно один файл стоит на
кадровом пути — `src/input/input_windows.c`. Его покадровые функции уже
сведены к минимуму (17 нс на полную проверку ввода за кадр, 0,015 %
бюджета), избыточной работы в них нет, и по правилу «без доказанного
выигрыша не менять» правка не вносилась. `src/ui/` — публичный API,
которого в дереве никто не вызывает, поэтому он холодный; его
гипотетическая цена за кадр измерена (меню 1,53 мкс), но реального
прогона с UI нет. `src/media/` — статическая библиотека форматов,
работающая только при загрузке паков и в офлайн-инструментах; ни
`ChunkStreamingPump`, ни кадровые функции рендерера её не зовут. Итоговое
дерево не изменено.
