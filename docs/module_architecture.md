# Автономные технологические модули

LAIUE теперь имеет два слоя расширения:

* `laiue::bootstrap` — маленький статический архив с runtime-loader,
  реестром сервисов, диагностикой и платформенным dynamic-library boundary;
* `laiue::engine` и старый `LaiueModLoadV1` — совместимый агрегат для
  существующих приложений и паков. Он не является обязательным для нового
  приложения.

Новый native-модуль экспортирует только `LaiueModuleGetApiV1` из
`mod/module_api.h`. В DLL нельзя вызывать тяжёлую инициализацию из `DllMain`.
`create → start → stop → destroy` вызывается загрузчиком, а сбой запуска
откатывает уже созданные модули. Загрузка принимает только пути из профиля;
каталоги автоматически не сканируются и найденные файлы не исполняются.

## Сервисы и зависимости

Модуль объявляет `requiresServices` и `providesServices` в дескрипторе. В
новом ABI требование содержит имя и `minimumVersion`; таблица сервиса содержит
версию и размер. Сервис разрешается один раз при запуске, а в рабочем цикле
модуль хранит указатель на таблицу, поэтому поиск по строке не попадает в
горячий путь. Поля `optionalServices`/`optionalCount` (в reserved-расширении
с magic marker) описывают provider,
который выбирается и запускается раньше consumer-а, если он есть, но его
отсутствие не делает граф ошибочным. Размер и offsets V1 не меняются, поэтому
старые descriptor-ы без этого расширения остаются совместимыми.

Граф запуска строится по фактически опубликованным сервисам. Одинаковые
идентификаторы, повторная публикация сервиса, отсутствующая зависимость и
цикл являются диагностируемыми ошибками. Порядок среди готовых узлов стабилен
и определяется ID. Остановка идёт строго в обратном порядке успешного запуска.

`LaiueModuleBinaryV1.flags` позволяет явно отметить отсутствующий optional
artifact. Такой файл пропускается; обязательный или присутствующий, но
повреждённый artifact завершает транзакцию. Горячей выгрузки кода нет: перед
закрытием библиотеки приложение прекращает callbacks и worker jobs.

## Границы контрактов

Публичные header-only контракты не импортируют друг друга:

* `graphics/graphics_api.h` — буферы, текстуры, draw items и кадр; в нём нет
  voxel/chunk/pack-типов;
* `voxel/voxel_api.h` — sparse block provider, запросы collision и meshing;
* `character/character_api.h` — 128 Hz кинематический контроллер и integer
  cell/local coordinates; физический rigid-body модуль ему не нужен.

`voxel_render` может связывать первые два контракта, но это отдельный слой.
Существующие `renderer.h` и `scene` пока остаются compatibility-реализациями
старого агрегата; новые модули не должны включать их в ABI. Загрузчики
ресурсных паков, включая `audio_pack`, работают только через собственные
versioned service tables и не имеют отдельной legacy-копии.

## Перенесённые providers

`laiue_numeric` уже экспортирует новый entry point и service
`laiue.numeric`. Таблица оборачивает существующую `InfiniteCoord`-арифметику и
переводит результаты `bool` в фиксированный `uint32_t`, поэтому потребитель не
импортирует numeric DLL напрямую. `laiue_task` аналогично публикует
`laiue.jobs` с opaque task pool и пакетным executor. Для статических/mobile/
console профилей оба target'а предоставляют module-specific static accessor;
глобальный `LaiueModuleGetApiV1` остаётся только у dynamic artifact.

`laiue_content` публикует `laiue.assets`: создание opaque catalog, выбор
активного пака и безопасное построение путей. Он не требует renderer или audio;
декодирование и конкретное использование ресурсов остаются у потребителя.

`laiue_character` публикует `laiue.character`: фиксированный 128 Hz
кинематический AABB-контроллер с integer cell/local координатами и callback
sweep-а. Он работает без `physics.dll`; collision provider остаётся у игры или
воксельного адаптера, а нормализация отрицательных и бесконечных координат
выполняется внутри провайдера без плавающей арифметики.

`laiue_voxel` публикует `laiue.voxel`: разреженное хранилище override-блоков
с opaque world handle, snapshot-подобным provider table и batch-friendly
enumeration. Оно не генерирует ландшафт и намеренно не строит mesh: meshing и
связка с graphics остаются отдельными технологиями.

`laiue_voxel_raycast` принимает чтение блока как callback. В dynamic-профиле
его DLL получает `laiue.world` через `queryService` и не имеет импортов другой
LAIUE DLL; отсутствие мира поэтому диагностируется загрузчиком как обычная
ошибка графа, а не как падение загрузчика ОС. Статический путь сохраняет
совместимую обёртку `VoxelRaycast(World*)`.

Остальные providers используют тот же entry point и lifecycle ABI:

* `laiue.world` публикует операции бесконечного мира и явно требует
  `laiue.numeric`;
* `laiue.physics` публикует детерминированный rigid/compound step, scratch и
  contact-cache API, требует `laiue.numeric` и `laiue.jobs`;
* `laiue.mesher` публикует scratch и greedy chunk meshing поверх generic
  `ChunkMesherWorldSource` callback. Он не требует и не импортирует
  `laiue.world`: world, voxel provider или тестовый источник адаптируются
  вызывающей стороной;
* `laiue.graphics` публикует backend-neutral renderer table, а `laiue.scene`
  требует этот provider и `laiue.scene_math`, а `laiue.voxel_render` требует
  graphics и остальные перечисленные providers вместо поиска функций в
  глобальном диспетчере;
* `laiue.scene_math` и `laiue.voxel_raycast` являются самостоятельными
  providers без renderer-зависимости; последний требует только `laiue.world`;
* `laiue.window` и `laiue.input` отделены от renderer и публикуются только в
  профилях, где соответствующий OS backend собран.
* `laiue.ui` публикует backend-neutral draw lists и требует
  `laiue.graphics` только на runtime-графе; его DLL не импортирует renderer.
* `laiue.audio` содержит PCM-микшер и offscreen-путь, а `laiue.audio.output`
  отдельно публикует системный вывод. Первый объявляет второй optional:
  удаление output DLL оставляет микшер и игру работоспособными, а запрос
  системного устройства возвращает понятную ошибку и не подменяется звуком.

Таким образом, отсутствие physics, voxel-render, UI, audio или window artifact
не делает bootstrap недействительным: профиль получает только диагностику
неразрешённого required service, а независимые providers продолжают работать.

Остальные адаптеры пока сохраняют явные link-зависимости на свои provider DLL,
но эти зависимости перечислены в дескрипторах и проверяются до `start`. Их
перевод на те же runtime service callbacks выполняется по одному модулю, чтобы
не менять физику, meshing и renderer одновременно.

Интеграционные тесты загружают реальные `laiue_numeric`, `laiue_task`,
`laiue_content`, `laiue_character` и `laiue_voxel` DLL, вызывают сервисные таблицы и
проверяют исчезновение сервисов после остановки.

## Манифест

`mod/module_manifest.h` содержит валидатор описания нативного artifact:
`id`, author, module version, platform, leaf binary name, provides/requires и
optional flag. Валидатор не открывает файл. После явного выбора artifact
приложение может дополнительно вызвать `LaiueModuleManifestValidateApi`, чтобы
проверить совпадение манифеста и descriptor, возвращённого DLL.

Это доверенный native code, а не sandbox. Для Windows/Linux/macOS/ARM64 и
консолей собирается отдельный artifact. На mobile/console profile может
использовать статический registry вместо dynamic loading.

## Минимальная игра

`examples/walk` собирается с `LAIUE_BUILD_EXAMPLES=ON` и линкуется только с
bootstrap. Она создаёт бесконечный слой grass/earth/stone, хранит координату как
cell + local, и выполняет локальный character tick с прыжком без `physics.dll`.
Оконный, input, graphics и UI providers подключаются поверх этих контрактов;
headless пример специально подтверждает, что базовая игра запускается без них.
