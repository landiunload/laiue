# Проверка гонок в параллельных протоколах (parallel work)

Работа велась в изолированном дереве `C:\Users\landi\projects\laiue-review`,
ветка `peer/review`. Зона — чтение `src/` и добавление тестов в `tests/`.
Публичный API/ABI, CMake, `src/` не менялись: доказанных гонок не найдено,
поэтому чинить было нечего (см. §7 — что осталось незакрытым и почему это
не гонка).

Итог по точкам 1–5: **во всех пяти протоколах при их заявленных контрактах
гонки нет**. Ниже — по каждой точке рассмотренные сценарии и обоснование
через конкретные пары операций и барьеры/замки, добавленные тесты и их
фальсификация.

## 0. Метод и сборки

- Прочитаны диффы всех сегодняшних правок: `46623fd..HEAD`,
  `2f2781d..HEAD` и отдельные коммиты `cdd7b40`, `2aa5cc7`, `249b4ed`,
  `ed49ff4`, `78d5ce3`, `2149852`, `947476d`, `3c1ef0f`, `c86d809`,
  `b9328b0`, `d79d29c`; для каждого — `git show -p`.
- Стрессы с настоящими потоками: `tests/world_provider_test.c`,
  `tests/audio_api_test.c`, `tests/chunk_streaming_stress_test.c`.
- Сборки и прогоны:

  | пресет | результат |
  | --- | --- |
  | `windows-msvc-release` | 30/30 |
  | `windows-msvc-debug` (со включённой `VerifyStreamingIntegrity`) | 30/30 |
  | `windows-clang-release` | 30/30 |

  Санитайзеров потоков на Windows-сборке без CRT нет; опора на рассуждение,
  стресс с повторами и `clang` (иная раскладка/переупорядочивание).

## 1. `editedChunkCount` и быстрый путь `WorldGetBlock`

Файл `src/world/world_infinite.c`.

### Что происходит

- Писатель (`WorldGetOrCreateChunk:334-338`, `WorldApplyBlockBatch:923-927`):
  под `PlatformRwLockAcquireExclusive(&world->tableLock)` пишет
  `keys[slot]`, `chunks[slot]`, `occupied[slot]`, `++world->count`, затем
  `WorldPublishEditedChunkCount` — `PlatformAtomicStoreU32Release(
  &world->editedChunkCount, world->count)` (`:298-301`), затем
  `ReleaseSRWLockExclusive`.
- Читатель (`WorldGetBlock:614`): `PlatformAtomicLoadU32Acquire(
  &world->editedChunkCount)`. При нуле — сразу `WorldBaseBlock` (быстрый
  путь), без замка. При ненулевом — `AcquireSRWLockShared`, `WorldFindEntry`,
  `ChunkGetDelta`, `ReleaseSRWLockShared`.

### Рассмотренные сценарии

1. **Первая правка видна следующему запросу того же потока.** Правка и
   запрос идут по программе подряд; release-запись и acquire-загрузка
   одного и того же объекта — simple release/acquire. На x86 загрузка
   выровненного 32-битного слова видит свою же предшествующую запись
   (store-to-load forwarding) и другие ядра видят записи в порядке
   когерентности. На ARM64/POSIX ветви — `InterlockedCompareExchange` /
   `__atomic_load_n(ACQUIRE)`, то есть полноценные acquire.
2. **Первая правка видна следующему запросу другого потока.** Если запрос
   начат после возврата правки и между потоками есть синхронизация
   (join/мьютекс/флаг), release/acquire даёт ребро happens-before: увидев
   ненулевой счётчик, читатель идёт медленным путём и под разделяемым
   замком видит чанк. Если синхронизации нет, запрос может «не увидеть»
   правку — это законная линеаризация чтения раньше записи, а не гонка.
3. **Параллельная первая правка и чтение.** Писатель держит
   исключительный замок; читатель с нулём замок не берёт и возвращает
   провайдера. Провайдер неизменяем (копия структуры в `WorldCreate`),
   поэтому гонки за память нет. Возврат провайдера — корректный ответ для
   чтения, упорядоченного до коммита правки.
4. **`WorldRebase`.** Счётчик не трогается (растёт только `count`), быстрый
   путь координат не читает вовсе. После переноса либо `count == 0` (правок
   нет — провайдер), либо `count > 0` (медленный путь под обновлённым
   `chunkOrigin`). Контракт `world.h:9-11` требует паузы пользователей
   локальных координат на время `WorldRebase`.
5. **Может ли счётчик снова стать нулём.** Нет: `world->count` только
   инкрементируется (`WorldGetOrCreateChunk`, `WorldApplyBlockBatch`), ни
   одного `--count`/`count = 0` в файле нет. `WorldTrySetBlock` со
   `block == base` при отсутствующей записи чанк не создаёт и счётчик не
   публикует — и это верно, правки нет.
6. **Уничтожение мира во время запроса из рабочего потока.** Это не
   изменилось сегодня: `WorldDestroy` освобождает структуру без замка и
   раньше; контракт (`world.h`) не обещает безопасность `WorldDestroy`
   против одновременных вызовов, только «обычные чтения и мутации
   потокобезопасны». Быстрый путь не добавляет новых долгоживущих
   указателей, только читает `provider` (неизменяем) — то есть контракт
   не ослаблен.

### Вывод

Гонки нет. Упорядочены: (запись чанка, `count`) → release-store
`editedChunkCount` → (release замка) … (acquire-load `editedChunkCount` →
shared-замок → чтение чанка). Быстрый путь при нуле не разделяет
изменяемых данных ни с кем.

### Тест и фальсификация

`tests/world_provider_test.c:TestFastPathConcurrentVisibility` — 4
читателя в цикле зовут `WorldGetBlock`; главный поток делает первую правку
и публикует `phase` через `PlatformAtomicStoreU32Release`; читатель,
увидевший `phase` через `PlatformAtomicLoadU32Acquire`, обязан увидеть
правку, иначе инкремент `badReads`. Фальсификация: удаление
`WorldPublishEditedChunkCount(world)` из `WorldGetOrCreateChunk` — тест
падает ровно на своём сообщении
`a reader that saw the published edit still read the provider`. После
возврата — 30/30. Существующий `TestFastPathAfterEmpty` остаётся
однопоточной проверкой «следующий запрос видит первую правку».

## 2. Acquire/release на x86 вместо `Interlocked*`

Файл `src/platform/system_windows.c:226-265`. На `_M_IX86 || _M_X64`
загрузка/запись стали обычными `__iso_volatile_load32`/`store32` с
`_ReadWriteBarrier`; на других архитектурах — по-прежнему
`InterlockedCompareExchange`/`InterlockedExchange`; POSIX —
`__atomic_load_n(ACQUIRE)`/`__atomic_store_n(RELEASE)`.

### Почему этого достаточно (и чего не хватает)

- Release-store: `_ReadWriteBarrier()` **перед** записью мешает
  компилятору переставить накопленные записи за неё; TSO аппаратно
  сохраняет порядок store→store. Достаточно для «опубликовать данные,
  затем индекс».
- Acquire-load: `_ReadWriteBarrier()` **после** загрузки мешает
  компилятору поднять последующие обращения выше; TSO сохраняет
  load→load. Достаточно для «прочитать индекс, затем данные».
- Что убрано: полный барьер `LOCK`-префикса, то есть **StoreLoad**.
  На x86 удалены только `Interlocked*` (CAS/Exchange) — они были
  полными барьерами. StoreLoad не нужен ни одному acquire/release-паттерну
  сам по себе; его требует схема Деккера «записал флаг A, прочитал флаг B».

### Все вызовы (23 загрузки + 16 записей = 39; в задаче названо 38)

Loader: `world_infinite.c:614`; `content_catalog.c:158,178`; `audio_alsa.c:119`;
`chunk_streaming.c:544`; `audio_mixer.c:183,201,216,225,232,246,409,412,416,500,509,594,661,702,758`;
`audio_wasapi.c:183,191`; `platform_threading_test.c:97`.
Storer: `world_infinite.c:300`; `content_catalog.c:169`;
`audio_alsa.c:140`;
`audio_mixer.c:190,203,222,235,252,320,349,379,414,493,672,677`;
`audio_wasapi.c:230`.

Разбор по видам протокола:

| протокол | вызовы | нужен ли StoreLoad |
| --- | --- | --- |
| один флаг остановки | `audio_alsa.c:119,140`, `audio_wasapi.c:183,191,230` | нет: только release-запись + acquire-проверка одного флага; на остановке ещё и `SetEvent`/`PlatformThreadJoin` |
| счётчик/гейт | `world_infinite.c` | нет: пара release/acquire одного объекта |
| одноразовая инициализация | `content_catalog.c:158,165,169,178` | нет: claim делает `PlatformAtomicCompareExchangeU32` (полный барьер на обеих платформах), публикация — release/acquire |
| номер эпохи | `chunk_streaming.c:544` (+ инкремент через `PlatformAtomicIncrementU32`) | нет: RMW-инкремент и acquire-загрузка |
| счётчики статистики | `platform_threading_test.c:97`, `audio_mixer.c:414,500,509` | нет: у `activeVoices` publish/acquire; у master volume read/acquire |
| SPSC-очередь команд | `audio_mixer.c:183,190,246,252` | нет: у каждого индекса ровно один писатель; индексы публикуются release и читаются acquire |
| состояния голосов | `audio_mixer.c:201,216,222,225,232,235,320,349,379,594,661,672,677,702,758` | нет: state — единственное общее поле слота; producer-owned поля публикуются release-записью state, consumer-owned читаются после acquire |

Единственные места, где запись в один атомик соседствует с чтением
другого (потенциальная схема Деккера):

1. `audio_mixer.c:672` (`state = PENDING`, release) → `PushCommand`
   (`:183`, acquire-чтение `commandRead`). Взаимного исключения между
   этими двумя локациями не требуется: `commandRead` читается только для
   проверки заполненности кольца, а сам `PENDING` публикуется другим
   объектом. Если кольцо полно, `:677` возвращает `FREE`. Порядок
   `PENDING` → payload → `commandWrite` обеспечивает release-store
   `commandWrite`; consumer, разобрав команду, видит `PENDING` под acquire.
2. `audio_mixer.c:203` (STOP_ALL-цикл) — запись `FINISHED` для слота `i`
   и acquire-загрузка состояния слота `i+1`. Это один и тот же поток
   (поток вывода), взаимного исключения нет.
3. `audio_mixer.c:320,349,379` (`FINISHED`) → `RenderFrames:412/416`
   (acquire-чтение того же `slot->state`, затем `activeVoices`/
   `masterVolumeBits`). Один поток; к тому же это та же ячейка.
4. `audio_mixer.c:414` (`activeVoices`) → `:416` (`masterVolumeBits`) —
   независимые публикации, ребра между ними не нужно.

Ни одного места, где поток A пишет флаг A и ждёт увидеть чужой флаг B,
пока поток B пишет флаг B и ждёт увидеть флаг A, нет. Значит, удаление
StoreLoad корректно.

### Вывод

Ни один вызывающий не полагался на полный барьер. Смена оправдана.

## 3. `chunk_streaming.c`

Файл `src/scene/chunk_streaming.c`.

### 3.1. `heldResult`

Владелец — только главный поток (`Pump`, `Pause`, `Destroy`), рабочие
потоки кладут результаты под `queueLock` и `heldResult` не трогают.

| событие | что делает `heldResult` | `requestQueued` |
| --- | --- | --- |
| `Pump`: рендер вернул NULL (`:1310-1317`) | сохраняет результат, `hasHeldResult = true` | ставит `true`, чтобы сканирование не заказывало заново |
| следующий `Pump` (`TryUploadHeldResult`) успех | отдаёт меш, освобождает квады, сбрасывает | `false` (`:1203`) |
| ревизия записи сменилась или запись ушла | освобождает квады, сбрасывает `hasHeldResult` (`:1174-1183`) | не трогает; при смене ревизии её уже переставил `TryEnqueueRequest` |
| `ChunkStreamingPause` (`:644-652`) | освобождает квады, сбрасывает | сбрасывает у всех записей (`:663-675`), при `PENDING` без меша ставит `hasUnqueuedPending` |
| `ChunkStreamingDestroy` (`:981-986`) | освобождает квады | поток уничтожается |
| вытеснение записи (`EraseEntry`) | на следующем `Pump` `FindEntry` вернёт NULL → результат отпускается | запись исчезла, флаг не нужен |
| `ResumeAfterOriginChange` | **не трогает** | новые записи получают его из `QueueMissingChunks` |

`requestQueued = true` не остаётся «навсегда»: он сбрасывается успешной
загрузкой, сменой ревизии, `Pause` или исчезновением записи. Даже если
`InvalidateBlock` не смог поставить заявку, `hasUnqueuedPending` заставит
`Pump` дозаказать.

**Найденное расхождение, но не гонка.** `ResumeAfterOriginChange` не
освобождает `heldResult`, хотя `docs/streaming_throughput_parallel_work.md`
(§6) утверждает обратное. В штатном сценарии это безопасно: перед сменой
origin приложение обязано поставить стриминг на паузу (`world.h:9-11`), а
`ChunkStreamingPause` уже освободил `heldResult`. Если `Resume` вызвать без
`Pause` (так делает только `RunOriginChangeScenario` в стресс-тесте, где
мир пуст и `heldResult` не появляется), старый результат теоретически
может быть применён к записи нового кадра. Это дефект дисциплины вызова и
утечка по документации, а не гонка; «на всякий случай» не чинилось.

### 3.2. `pauseGeneration`

- Рабочий (`WorkerThreadProcedure:512-528`) заводит локальный
  `reportedPauseGeneration`. В цикле ожидания он один раз на **номер**
  паузы увеличивает `pausedWorkerCount` и будит всех.
- Двойная пауза: `Pause` (`:622-625`) на каждую паузу делает
  `++pauseGeneration` и `pausedWorkerCount = 0`; рабочие, уже стоящие в
  `Wait` с прошлым номером, при пробуждении видят новый номер и
  отчитываются снова. Завершение гарантировано.
- Пауза во время завершения (`StopWorkerThreads:588-593`): ставит
  `shutdownRequested = true`, `pauseRequested = false`, `WakeAll`; рабочие
  выходят из `Wait`, видят shutdown и возвращаются, не увеличивая
  `pausedWorkerCount`. `Pause` и `Stop` — управляющие вызовы одного
  главного потока, одновременно не идут.
- Возобновление во время паузы: `ResumeWorkerThreads` (`:605-612`) сбрасывает
  `pauseRequested` и `pausedWorkerCount`, но **не** номер; следующая пауза
  увеличивает номер, и отчёт не теряется. Именно это чинил коммит
  `c86d809`; сценарий «Pause сразу после Resume при пустой очереди»
  покрыт `RunPauseAfterResumeScenario`.
- `Destroy` во время паузы: сначала `StopWorkerThreads` будит и присоединяет
  рабочих, потом освобождается память; `heldResult`/`results` освобождаются
  (`:971-986`).

`pauseGeneration` читается и пишется под `queueLock`, гонки нет.

### 3.3. `EraseEntry` и результаты в очереди

- `EraseEntry` (`:304-342`) сдвигает кластер, копируя запись целиком, и
  чинит `drawItems[..].entryIndex` у переехавшей записи (`:328-333`).
  Удаляемая запись уже снята со списка отрисовки в `EvictChunk` ДО удаления.
- Результат в очереди не хранит ни индекс, ни указатель: только
  `x,y,z,revision` (`ChunkMeshResult:63-71`). `Pump` ищет запись по
  координатам (`FindEntry:1274`) и требует `state == PENDING &&
  revision == result.revision` (`:1275`). Сдвиг кластера сохраняет и
  координаты, и ревизию, поэтому переезд результат не ломает; удаление
  даёт `FindEntry == NULL` и результат отбрасывается; смена ревизии даёт
  несовпадение и отбрасывание.
- Заявка и результат согласованы через `requestQueued`/`unfinishedWork`:
  `unfinishedWork` не превышает ёмкость кольца (`TryEnqueueRequest:467`),
  поэтому очередь результатов переполниться не может.
- **Теоретическая дыра (не воспроизведена):** `revision` нового слота
  сбрасывается в 0 (`InsertEntry:281`), поэтому после вытеснения и
  повторной вставки того же ключа два результата могут иметь одинаковую
  пару `(координаты, revision)`. Отмена по `centerEpoch` проверяется
  рабочим один раз в начале сборки (`:543-544`), поэтому сборка, начатая
  до смены центра и завершившаяся после, не отменяется. Если между
  вытеснением и вставкой содержимое чанка изменилось, результат более
  ранней сборки может перезаписать более поздний. Условие узкое (колебание
  центра внутри одной сборки, больше результатов, чем одно на ключ) и
  требует инструментирования, чтобы доказать; в штатном кадре сборка
  дешевле кадра. Тест не падает, потому что пустой мир даёт нулевые меши.
  Это не гонка за память, а недостающая уникальность ключа заявки.

### Тест

`RunConcurrentCenterScenario` (2 сценария, radius 2/3): пока рабочие
потоки строят, главный двигает центр (соседний шаг и телепорт),
инвалидирует блоки и дважды за шаг зовёт `Pump`; затем `StressSettle` и
независимая сверка множества ключей/списка отрисовки. Фальсификация
общая с остальным файлом: поломка `EraseEntry` (убрать сдвиг кластера)
ловится независимым эталоном (`live chunk count differs from the expected
cube`). Сам сценарий внутри 400 ходов поломку `EraseEntry` не поймал —
его ловит более длинный случайный сценарий; поэтому сценарий ценен как
проверка целостности таблицы при живой очереди (в Debug к нему
подключена `VerifyStreamingIntegrity`), а не как замена случайному.

## 4. `audio_mixer.c`

Файл `src/audio/audio_mixer.c`.

### 4.1. Быстрая ветвь и запись состояния

`MixVoice` при `step == 1.0` и целой позиции (`:304`) в конце клипа делает
ровно то же, что общая ветвь (`:317-322` против `:375-381`):
`slot->clip = NULL` → `PlatformAtomicStoreU32Release(&slot->state,
VOICE_FINISHED)` → `slot->position = ...` → `return`. Порядок и значения
совпадают; запись `position` после release-записи безопасна, потому что
`position` — собственность потока вывода: приложение читает только `state`
и `slotClips`. Повторное использование слота приложением возможно только
после того, как оно увидит `FINISHED` acquire-загрузкой, а обработка новой
команды START произойдёт в начале следующего `RenderFrames`, то есть после
возврата из `MixVoice`; перекрытия записей `position` нет.

### 4.2. SPSC-очередь

Писатель (`PushCommand`, под `producerLock`): `commands[write] = *command`
(обычные записи) → `PlatformAtomicStoreU32Release(&commandWrite, next)`.
Читатель (`DrainCommands`): `PlatformAtomicLoadU32Acquire(&commandWrite)` →
чтение payload → `PlatformAtomicStoreU32Release(&commandRead, read)`.
Один писатель `commandWrite`, один читатель `commandRead`; release/acquire
на каждом индексе. Полный барьер не нужен (см. §2).

### 4.3. Состояния голоса

`state` — единственное поле слота, которое читают и пишут оба потока;
переходы `FREE→PENDING` (приложение под `producerLock`),
`PENDING→ACTIVE`, `ACTIVE/PENDING→FINISHED` (поток вывода). Данные голоса
(`clip/position/step/gains/looping`) пишет только поток вывода после
acquire-наблюдения `PENDING/ACTIVE`; приложение их не читает. Поля
приложения (`slotClips`, `clipSampleRate`, `generation`) публикуются
release-записью `PENDING` и защищены `producerLock` для межаппликационных
гонок. Устаревшие команды отсекаются сравнением `generation`
(`ApplyCommand:211`), а её запись предшествует публикации команды, поэтому
consumer видит актуальное значение. Гонки нет.

### Тест и фальсификация

`tests/audio_api_test.c:CheckConcurrentMixer` — поток вывода крутит
`AudioDeviceRenderFrames`, главный поток 20 000 раз заказывает, меняет и
останавливает голоса, отбирает сэмпл `activeVoices` и после `StopAll`
проверяет, что все недавние дескрипторы неактивны. Фальсификация: в
`ApplyCommand` для `COMMAND_STOP_ALL` заменить `VOICE_FINISHED` на
`VOICE_ACTIVE` — тест падает на
`a voice handle survived stop-all as active` (без усиленной проверки
дескрипторов падал бы только тест `activeVoices`, что тоже является
проверкой протокола). После возврата — 30/30.

## 5. Параллельная узкая фаза `rigid_body.c`

Файлы `src/physics/rigid_body.c`, `docs/tree_narrowphase_parallel_work.md`.

### 5.1. Разделение записей

Двухпроходная схема (`GridNarrowphaseRange`/`TreeNarrowphaseRange`):

1. проход счёта: каждый диапазон по `ordered` пишет только «свои» тела —
   `narrowphasePoints[first*8..]`, `wakeQueue[first]`,
   `caches[first].candidatePairs`, `narrowVisits[first]`;
2. последовательный канонический префикс по `scratch->order` раздаёт
   каждому телу непересекающийся отрезок `contacts[wakeQueue[first]..)`;
3. проход заполнения пишет `contacts[output..]` в свой отрезок и
   `caches[first].candidatePairs` для контроля.

`first` для каждого `ordered` уникален, отрезки не пересекаются, поэтому
одну и ту же запись два рабочих не пишут. `scratch->next` для дерева —
стек диапазона (`RIGID_TREE_NARROW_CANDIDATES = 128`), счётчик посещений —
копия описателя `VoxelRigidBroadphase` на стеке (`localTree`), сумма
`narrowVisits` собирается последовательно. `stats->candidatePairCount`
пишется только в последовательных префиксах/постпроходах.

### 5.2. Флаги `hasBodyContact`

Ставятся последовательным постпроходом после заполнения
(`CollectTreeContactsParallel:3426-3440`, `CollectGridContactsParallel:3201-3215`),
после барьера `ExecuteRange` (task_pool `Run` ждёт `remainingWorkers == 0`).
Те же флаги читает интегрирование (`:4361`) уже после узкой фазы. В
параллельных диапазонах они не пишутся. Гонки нет.

### 5.3. Переполнение

- Контакты: `AppendContact` при заполнении ставит `contactOverflow` и
  завершает шаг ошибкой; параллельный путь заранее проверяет
  `count > contactCapacity - total` и возвращает false.
- Цвета решателя: при `color == RIGID_SOLVER_COLOR_COUNT` серия целиком
  уходит в `overflowContacts += end - begin` (`:4201-4204`), пишется в
  слот `counts[64]`/`batchOffsets[65]` (массивы имеют размер
  `COLOR_COUNT+1`/`+2`) и решается **последовательно** после цветных
  батчей (`:4287-4288`). Счётчик считается по числу контактов, а не серий,
  и не теряется. Гонки нет: `BuildSolverBatches` вызывается на потоке
  вызывающего.

### Тесты

Новых не добавлялось: покрытие уже есть в `tests/rigid_parallel_test.c`
(`TestDenseParallelTree`, `TestWideBodyParallelTree` — путь перегенерации
манифольдов, `TestColorOverflow` — переполнение цвета и независимость
`overflow_contacts` от исполнителя). Полный `ctest` (30/30) их гоняет.

## 6. Итог

- Точки 1–5: **доказанных гонок нет**, `src/` не менялся.
- Добавлены тесты:
  `TestFastPathConcurrentVisibility` (world),
  `CheckConcurrentMixer` (audio),
  `RunConcurrentCenterScenario` (streaming).
- Фальсификации (каждая поломка временно вносилась в `src/`, сборка,
  прогон, возврат):
  | поломка | что поймало |
  | --- | --- |
  | убрать `WorldPublishEditedChunkCount` в `WorldGetOrCreateChunk` | `a reader that saw the published edit still read the provider` |
  | `COMMAND_STOP_ALL`: `FINISHED → ACTIVE` | `a voice handle survived stop-all as active` |
  | `EraseEntry`: убрать сдвиг кластера | `live chunk count differs from the expected cube` (случайный сценарий; независимый эталон) |
- Зафиксировано, что не является гонкой, но заслуживает внимания:
  1. `ResumeAfterOriginChange` не освобождает `heldResult` вопреки
     документации; безопасно только потому, что штатно ему предшествует
     `ChunkStreamingPause`. Чинить — за рамками «только доказанные гонки».
  2. `revision` обнуляется при повторной вставке записи; теоретически
     возможна перезапись свежего результата старым при колебании центра
     внутри одной сборки. Не воспроизведено; для доказательства нужна
     уникальная метка заявки (например, `centerEpoch` в паре ключа).

## 7. Что проверить не удалось и почему

- **TSan/санитайзеры потоков** на Windows-сборке без CRT недоступны;
  выводы о гонках — из рассуждения о барьерах и стресса с повторами, а не
  из инструмента.
- **ARM64 и Linux/macOS исполнением не проверялись** (машина x64,
  заголовков glibc/ARM64-запуска нет). Для ARM64 ветвь с обычной
  загрузкой/записью не компилируется (там `#else` с `Interlocked*`), так
  что переносимость не пострадала; POSIX-ветвь на `__atomic` не
  исполнялась.
- **Реальный рендерер** в стрессах стриминга не поднимался: мир пуст, и
  ветки `RendererCreateMesh`/`heldResult` с настоящей геометрией
  проверяются только фиктивным нулевым рендерером в
  `tests/voxel_raycast_test.c`. Перезапись результата после сдвига
  записи с настоящим мешем отдельным тестом не закрыта.
- **Оптимизации `-O0`/`-O2`** MSVC `_ReadWriteBarrier` дают одинаковый
  машинный порядок (проверено тем, что Release и Debug оба зелёные), но
  отдельного дизассемблирования `.text` не делалось.
