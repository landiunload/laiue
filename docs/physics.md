# Физика и сетевая репликация игрока

Это каноническое описание общего physics contract клиента и dedicated
server. Параметры ходьбы, прыжка, коллизий и стойки отдельно сведены в
[player_physics.md](player_physics.md); при изменении сетевого тика,
authoritative state, prediction или interpolation обновляется этот документ.

## Общий временной контракт

| Величина | Значение |
|---|---:|
| network simulation tick | 60 Гц |
| physics substep | 1/240 с |
| substeps на одну сетевую команду | 4 |
| authoritative player state | каждые 3 тика, 20 Гц |
| catch-up клиента | не более 8 тиков за render frame |
| catch-up сервера | не более 8 тиков за outer iteration |
| safety backlog сервера | 256 тиков, горизонт input FIFO |
| server input FIFO | 256 команд |
| client prediction history | 256 команд |
| remote interpolation history | 32 snapshot |
| interpolation delay | 6 server ticks, 100 мс |
| максимальная extrapolation | 2 server ticks, около 33 мс |

Клиент и сервер вызывают один `PlayerControllerSimulateFixedSteps` с четырьмя
substep. Функция исполняет целое число шагов и не читает frame accumulator,
поэтому replay не зависит от render FPS. Обычный `PlayerControllerUpdate`
остаётся frame-адаптером для одиночной игры; сетевой путь не подаёт в
authoritative simulation произвольный `deltaSeconds`.
Монотонный frame time хранится в `double` вплоть до simulation accumulator;
`fixedStepSeconds` также имеет тип `double`. Перевод времени в `float`
разрешён только для presentation-подсистем. Regression проверяет одинаковый
результат за десять секунд при 30, 60 и 144 FPS.

`jumpPressed` является edge-событием одной команды. `jumpHeld`, sprint,
crouch и направление действуют на всех четырёх substep. При открытом
локальном меню multiplayer не ставится на паузу: клиент продолжает 60 Гц
симуляцию и отправляет каноническую нейтральную команду, чтобы сервер сразу
остановил удерживаемое движение, но продолжил гравитацию и течение времени.

Accumulator клиента ограничен восемью network ticks. Сервер также исполняет
не более восьми catch-up ticks за одну outer iteration, но не отбрасывает
оставшийся хвост: следующая итерация продолжает его разбирать без sleep и без
дорогой snapshot preparation. Общий safety backlog ограничен 256 тиками —
тем же горизонтом, что input FIFO. Поэтому одиночный hitch не создаёт
постоянное отставание очереди команд, а длительная перегрузка остаётся
bounded и не превращается в бесконечный spiral of death.

## Канонический input и sequence

Каждая `NetworkInputCommand` получает ненулевой монотонный `uint32_t
sequence`. Ноль зарезервирован; после `UINT32_MAX` следующий wire sequence
равен 1. Сравнение выполняется wrap-safe serial arithmetic при активном окне
меньше `2^31`.

Перед отправкой и локальным prediction клиент обязан вызвать
`NetworkInputCanonicalize`. Эта функция применяет тот же encode/decode и то
же квантование движения и углов, что production transport. Клиент
предсказывает уже каноническую команду, поэтому сервер не получает слегка
отличающийся float-вектор.

Последовательность одного клиентского тика:

1. Считать intent и присвоить следующий sequence.
2. Канонизировать и проверить команду.
3. Успешно поставить её в send queue.
4. Исполнить ровно четыре physics substep.
5. Сохранить команду и полученное полное состояние в prediction history.

Если канонизация или send не удались, sequence не продвигается и локальная
физика этой команды не исполняется.

Сервер помещает команды каждого peer в отдельный FIFO на 256 элементов и
потребляет не больше одной новой команды за tick. Старый или повторный
sequence безопасно игнорируется. Прямой gap после уже принятой команды
является protocol error: reliable QUIC stream не может потерять
промежуточный input. Переполнение FIFO закрывает только виновного peer с
`NETWORK_DISCONNECT_OVERFLOW`. Если новой команды нет, held-состояние может
продолжиться, но `jumpPressed` повторно не применяется. На 15-м server tick
без принятого input команда становится нейтральной; граница проверяется
wrap-safe даже при переполнении счётчика tick.

## Authoritative state и reconciliation

Сервер единолично владеет физическим состоянием. `NetworkPlayerState`
содержит:

- `serverTick`, `peerId` и `lastProcessedInputSequence`;
- точную позицию;
- yaw/pitch для server-side interaction и представления других игроков;
- горизонтальную и вертикальную скорость;
- внешнюю скорость;
- jump buffer и coyote timer;
- collider/eye crouch progress и crouch request;
- остаток воздушных прыжков и grounded.

Конфигурация контроллера в snapshot не входит: стороны обязаны согласовать
один physics contract до `READY`. `PlayerControllerRestoreState` сначала
проверяет snapshot целиком, затем атомарно меняет controller/camera и
обнуляет его внутренний accumulator.

При получении собственного authoritative state клиент:

1. Игнорирует только state, у которого `serverTick` не новее уже
   применённого; свежий tick с тем же acknowledgement обязан применяться.
2. Восстанавливает состояние на `lastProcessedInputSequence`.
3. Удаляет подтверждённый префикс history.
4. Повторяет все более новые канонические команды по четыре substep.
5. Перезаписывает сохранённые predicted states результатами replay.

Если acknowledgement старше вытесненного окна из 256 команд, точный replay
невозможен. Клиент выполняет bounded hard restore и начинает новую историю.
Некорректный или неограниченный state завершает обработку как protocol error,
а не частично меняет controller.

Prediction и replay должны видеть ту же процедурную базу мира и те же
authoritative chunk revisions. Live block delta, пришедшая во время
snapshot, применяется после snapshot; разрыв revision вызывает bounded
resync чанка.

## Локальное визуальное сглаживание

Физическая `networkPhysicsCamera` отделена от presentation camera.
Reconciliation всегда исправляет physics state сразу; collision position
никогда не lerp-ится к серверу и не проходит сквозь блоки ради плавности.

Renderer интерполирует предыдущую и текущую 60 Гц physics position по
остатку accumulator. Небольшая ошибка reconciliation сохраняется только как
визуальное смещение и затухает с bounded rate. При расхождении больше двух
блоков presentation немедленно привязывается к physics state. Локальный
mouse look не откатывается сетевым yaw/pitch собственного игрока.

## Удалённые игроки

Для каждого remote peer хранится ring из 32 строго возрастающих
`serverTick` snapshot. Старые и повторные ticks игнорируются, сравнение
учитывает wrap `uint32_t`.

После `READY` сервер предпочитает QUIC DATAGRAM для `PLAYER_STATE`, поэтому
потерянный старый snapshot не блокирует более новый. Полный bounded frame
копируется callback-ом и разбирается только на main thread. Если расширение
не согласовано, MTU недостаточен либо заняты все заранее выделенные send
slots, конкретный state уходит по reliable control stream. Оба пути
используют одну freshness policy; roster и join/left всегда reliable.

Presentation семплируется на шесть тиков позади newest state. Позиция и
pitch интерполируются линейно, yaw идёт по кратчайшей дуге. При временном
отсутствии snapshot последняя скорость продолжается максимум два тика,
после чего pose замораживается. Перемещение дальше восьми блоков считается
teleport и атомарно начинает новую interpolation history.

Этот слой изменяет только отображаемую pose. Remote player не участвует в
локальной collision simulation и не становится источником authoritative
gameplay state. Сглаженная позиция и актуальная стойка передаются в
presentation-only renderer: все видимые peers рисуются одним общим
инстанс-мешем, а положение ног вычисляется из тех же параметров eye/collider,
что использует контроллер. Потеря render frame поэтому не меняет сетевую
историю и не влияет на collision.

## Initial и live snapshots

`NetworkSnapshotInfo.requiresReadyBarrier` разделяет два режима:

- initial snapshot имеет barrier, переводит соединение в `SYNCING_WORLD` и
  требует `SYNC_APPLIED` до разрешения input/edit;
- live interest/resync snapshot не меняет `READY`, не требует нового
  acknowledgement и не останавливает поток input.

Initial barrier включает seed, сохранённые изменения чанков, world time,
inventory, active drops и roster. Live snapshot обновляет interest window
или восстанавливает revision чанка, пока physics продолжает работать.

Snapshot и control stream упорядочены независимо. Клиент никогда не
уменьшает известную revision: snapshot `R`, пришедший после уже применённой
live delta `R+1`, считается устаревшим и не заменяет чанк. Та же проверка
повторяется в `WorldReplaceChunkDeltas` под world lock. После успешного
chunk-resync клиент снимает ровно один pending marker, запускает следующий
bounded resync и возвращает `READY` только когда вся цепочка завершена.

Один snapshot ограничен 4096 логическими чанками, одна часть — 128 правками,
а live deltas во время сборки помещаются в bounded event queue. Все peers
сначала дешёво помечаются как pending; затем round-robin scheduler с
экспоненциальным backoff проверяет готовность auxiliary QUIC stream и
разрешает не более одной дорогой preparation на authoritative server tick.
Во время server catch-up snapshot work не запускается. Текущая реализация
всё ещё формирует и ставит в QUIC queue один выбранный snapshot синхронно;
bounded размеры ограничивают память, но максимальная job до 4096 чанков пока
не имеет отдельного лимита chunks/bytes на tick.

## Моды и authority

Physics-changing мод не может считать remote client источником истины.
В сетевой сессии client `ModHost` не получает локальный `PlayerController`,
поэтому `applyImpulse`, air jumps и другие мутации не меняют predicted
authority в обход сервера. Результат server-side изменения должен попасть в
полный authoritative state и пройти обычный reconciliation.

`setBlock` также использует явную fail-closed policy. Offline-клиент может
менять свой мир локально; remote-клиент получает `DENY` и не способен
испортить collision copy только на своей стороне. Dedicated server применяет
правку callback-ом к authoritative `World`, берёт новую chunk revision и
рассылает `NetworkBlockDelta` всем заинтересованным peers. Startup-правка до
создания transport попадает в initial snapshot.

Текущий server `ModHost` не предоставляет моду per-player controller
context. Добавление multiplayer physics API для модов требует явного
server-side выбора игрока, versioned SDK/wire contract и regression tests;
временное повторение одной мутации независимо на клиенте и сервере
запрещено.

## Инварианты разработки

- Fixed tick не выполняет allocation, I/O и logging.
- MsQuic callback не вызывает physics, world или gameplay.
- Network prediction использует только канонический input и целые substep.
- Authoritative restore проверяет все числа, диапазоны, flags и timers до
  первой записи состояния.
- Коррекция physics и визуальное сглаживание остаются разными слоями.
- Server/input ticks сравниваются wrap-safe; обычные `<`/`>` для них
  запрещены.
- Bounded history/FIFO не расширяются allocation из simulation thread.
- Изменение physics formulas, state layout, квантования или tick policy
  получает determinism, protocol round-trip и reconciliation regression
  tests.

Основные проверки находятся в `determinism_test`,
`player_replication_test`, `server_input_queue_test`, `protocol_test` и
`quic_integration_test`. Server clock, snapshot scheduler, revision reorder и
mod block authority имеют отдельные regression tests. Replication regression
дополнительно прогоняет
задержку и jitter, повторные authoritative corrections, прыжки, crouch и
server-only impulse до точного схождения клиента с сервером. Тот же
production `PlayerFixedTickClock`, который использует клиент, проверяется
при 30/60/144 FPS, catch-up cap и временном send backpressure. Отдельная
граница заполняет все 256 записей prediction history, вытесняет старые и
проверяет `HISTORY_MISS` с обеих сторон сохранённого окна.

## Ограничения

- QUIC DATAGRAM используется только server-to-client для transient
  `PLAYER_STATE`. Input остаётся reliable: `jumpPressed` и другие edge-команды
  нельзя безопасно потерять без отдельного repeat/ack контракта. Peer без
  DATAGRAM использует reliable fallback и при packet loss может получить
  state позже из-за head-of-line.
- Сервер не делает rollback/lag compensation для interaction.
- Полный physics state пока рассылается тем же player-state сообщением, что
  и remote presentation state.
- Player profile и физическое состояние не сохраняются между независимыми
  подключениями.
- Официальный deterministic contract ограничен x86_64/SSE2; ARM64 пока не
  входит в build matrix.
