#pragma once

#include "api.h"
#include "numeric/infinite_coord.h"
#include "physics/voxel_body.h"
#include "physics/rigid_broadphase.h"
#include "task/task_pool.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Твёрдое тело-коробка с вращением.
 *
 * VoxelBody рядом — контроллер персонажа: он не вращается, движется по одной
 * оси за раз и намеренно ничего не знает о массе. Здесь наоборот: полноценная
 * динамика с ориентацией, тензором инерции и импульсами в точках контакта.
 *
 * Скорость и угловая скорость хранятся числами произвольной точности в
 * фиксированной точке, а не в double. Причина не в точности, а в отсутствии
 * потолка: тело, которое разгоняют без конца, обязано разгоняться без конца.
 * Позиция по той же причине тоже произвольной точности — иначе первая же
 * невероятная скорость превратила бы её в бесконечность за один шаг.
 *
 * Локальные величины (ориентация, импульсы, геометрия контакта) остаются
 * double: они ограничены по своей природе. Мост между двумя мирами —
 * насыщающее преобразование, поэтому огромная скорость не отравляет решатель,
 * а лишь выводит тело из режима столкновений (см. VOXEL_RIGID_BALLISTIC_BLOCKS).
 */

// Масштаб фиксированной точки: единица скорости — блок в секунду.
#define VOXEL_RIGID_VELOCITY_SHIFT 32u
// Шаг симуляции — 1/128 секунды. Степень двойки не ради скорости, а ради
// точности: умножение на шаг становится сдвигом и не теряет ни бита.
#define VOXEL_RIGID_STEP_SHIFT 7u

// Верхняя граница представления: дерево требует до 2*N-1 uint32-индексов.
// Дополнительно StepScratchBytes обязан вернуть
// ненулевой размер: конкретная раскладка scratch может раньше упереться в
// uint32. Это предел представления буфера, а не обещание скорости симуляции.
#define VOXEL_RIGID_MAX_BODIES (UINT32_MAX / 2u)
// Общий бюджет контактов = число тел * это значение. Плотные пересечения
// могут превысить бюджет: Step вернёт false, а не пропустит ограничения.
#define VOXEL_RIGID_CONTACTS_PER_BODY 16u

// Legacy ballistic escape: за этим перемещением контакты отключены, но
// bigint-интеграция продолжается. Это НЕ физическое отсутствие столкновений
// и НЕ CCD; приложение не должно полагаться на контакты в таком режиме.
#define VOXEL_RIGID_BALLISTIC_BLOCKS 1048576.0

// Сколько клеток мира тело вправе накрыть своим AABB. Столкновение с миром
// разбирает всю эту окрестность, поэтому тело большего размера с блоками не
// сталкивается вовсе (с другими телами — по-прежнему да). Это предел
// размера тела, а не его скорости.
// Для compound большой envelope разбирается по детям: каждый ребёнок обязан
// уложиться в этот предел. Если не помещается и ребёнок, шаг вернёт false.
#define VOXEL_RIGID_MAX_WORLD_CELLS 4096u

typedef struct VoxelRigidBodyDescription
{
    // Полурёбра коробки. Все три положительны.
    double halfExtent[3];
    // Начальная позиция центра масс в локальных координатах.
    double position[3];
    double mass;
    // Упругость [0, 1] и трение [0, 1].
    double restitution;
    double friction;
} VoxelRigidBodyDescription;

typedef struct VoxelRigidBody
{
    // Позиция центра масс: блоки, умноженные на 2^VOXEL_RIGID_VELOCITY_SHIFT.
    InfiniteCoord position[3];
    // Скорость: блоков в секунду в том же масштабе.
    InfiniteCoord linearVelocity[3];
    // Угловая скорость: радиан в секунду в том же масштабе.
    InfiniteCoord angularVelocity[3];
    // Единичный кватернион (x, y, z, w).
    double orientation[4];
    double halfExtent[3];
    double inverseMass;
    // Диагональ обратного тензора инерции в системе тела.
    double inverseInertia[3];
    double restitution;
    double friction;
    // Ненулевой уникальный идентификатор: контакты обязаны быть
    // воспроизводимыми, а порядок обхода массива — нет.
    uint64_t stableId;
    bool active;
    // Покой позволяет не решать устойчивые тела на каждом fixed tick.
    uint32_t sleepCounter;
    bool sleeping;
} VoxelRigidBody;

typedef struct VoxelRigidStepSettings
{
    // Ускорение свободного падения, блоков в секунду за секунду.
    double gravity[3];
    // Итераций решателя контактов за шаг. Меньше трёх куча не держит.
    uint32_t solverIterations;
    // Доля проникновения, устраняемая за шаг [0, 1]. Единица толкает резко
    // и раскачивает стопку, ноль оставляет тела утопленными.
    double penetrationCorrection;
    // Проникновение, которое считается допустимым: без него тела дрожат,
    // бесконечно выталкивая друг друга из численного шума.
    double penetrationSlop;
    // Порог сна в блоках/секунду и радианах/секунду. Нулевой порог
    // отключает переход в сон; sleepFrames задаёт устойчивость порога.
    double sleepLinearSpeed;
    double sleepAngularSpeed;
    uint32_t sleepFrames;
} VoxelRigidStepSettings;

// Диагностика последнего шага в переданном scratch-буфере. Счётчики не
// участвуют в решении и позволяют игре отличить стоимость broadphase/SAT от
// стоимости собственного рендера без скрытых аллокаций и глобального state.
typedef struct VoxelRigidStepStats
{
    uint32_t activeBodyCount;
    uint32_t awakeBodyCount;
    uint32_t candidatePairCount;
    uint32_t contactCount;
} VoxelRigidStepStats;

// Необязательный persistent cache импульсов для warm-start решателя.
// storage принадлежит вызывающему, не пересекается со scratch, телами или
// дескриптором cache и живёт между
// шагами; скрытых выделений памяти нет. Остальные поля менять вручную нельзя.
typedef struct VoxelRigidContactCache
{
    void *storage;
    uint32_t storageBytes;
    uint32_t bodyCapacity;
    uint32_t contactCount;
    uint32_t matchedContactCount;
} VoxelRigidContactCache;

typedef enum VoxelRigidProfileStage
{
    VOXEL_RIGID_PROFILE_ORDER,
    VOXEL_RIGID_PROFILE_FORCES,
    VOXEL_RIGID_PROFILE_BOUNDS,
    VOXEL_RIGID_PROFILE_BROADPHASE,
    VOXEL_RIGID_PROFILE_WAKE,
    VOXEL_RIGID_PROFILE_WORLD_CONTACTS,
    VOXEL_RIGID_PROFILE_BODY_CONTACTS,
    VOXEL_RIGID_PROFILE_PREPARE,
    VOXEL_RIGID_PROFILE_WARM_START,
    VOXEL_RIGID_PROFILE_SCHEDULE,
    VOXEL_RIGID_PROFILE_SOLVE,
    VOXEL_RIGID_PROFILE_INTEGRATE,
    VOXEL_RIGID_PROFILE_SLEEP,
    VOXEL_RIGID_PROFILE_STORE,
    VOXEL_RIGID_PROFILE_STAGE_COUNT
} VoxelRigidProfileStage;

// Diagnostic output only, never part of replay. Initialize structSize before use.
// With no clockSeconds callback timings are zero, but schedule counters work.
typedef struct VoxelRigidStepProfile
{
    uint32_t structSize;
    double seconds[VOXEL_RIGID_PROFILE_STAGE_COUNT];
    uint32_t solverBatchCount;
    uint32_t solverMaxBatchSize;
    uint32_t solverOverflowContacts;
} VoxelRigidStepProfile;

typedef enum VoxelRigidSolverOrder
{
    VOXEL_RIGID_SOLVER_CANONICAL = 0,
    VOXEL_RIGID_SOLVER_COLORED = 1
} VoxelRigidSolverOrder;

// Per-call extensible options. NULL executor runs serially. A supplied executor
// must synchronously finish all ranges, without re-entering this step or changing
// its inputs. Physics normalizes FP on each worker range. World callbacks remain
// on the calling thread. Options, executor and profile must not alias simulation
// buffers. Clock callbacks are observational and must not mutate physics state.
// Colored order is deterministic across worker counts but changes trajectories
// relative to the canonical solver: the order is part of replay configuration.
typedef struct VoxelRigidStepOptions
{
    uint32_t structSize;
    VoxelRigidContactCache *contactCache;
    VoxelRigidBroadphase *broadphase;
    const LaiueTaskExecutor *executor;
    VoxelRigidSolverOrder solverOrder;
    VoxelRigidStepProfile *profile;
    double (*clockSeconds)(void *context);
    void *clockContext;
} VoxelRigidStepOptions;

LAIUE_PHYSICS_API void VoxelRigidStepSettingsDefault(VoxelRigidStepSettings *outSettings);

// Готовит тело. Возвращает false при неверном описании или нехватке памяти;
// в этом случае тело остаётся пригодным для Release.
LAIUE_PHYSICS_API bool VoxelRigidBodyInitialize(VoxelRigidBody *body, uint64_t stableId,
                                                const VoxelRigidBodyDescription *description);
LAIUE_PHYSICS_API void VoxelRigidBodyRelease(VoxelRigidBody *body);
// Пробуждает тело после изменения мира или другого внешнего состояния.
// Контакт с активным телом пробуждает связанную спящую группу автоматически.
// После удаления опоры вызывающий обязан явно инвалидировать сон через Wake.
LAIUE_PHYSICS_API void VoxelRigidBodyWake(VoxelRigidBody *body);

// Позиция центра масс в локальных координатах. false означает, что тело
// улетело за пределы double: рисовать и сталкивать его уже нельзя.
LAIUE_PHYSICS_API bool VoxelRigidBodyLocalPosition(const VoxelRigidBody *body,
                                                   double outPosition[3]);
// Матрица поворота 3x3 по столбцам, пригодная для инстанса рендера.
LAIUE_PHYSICS_API void VoxelRigidBodyOrientationMatrix(const VoxelRigidBody *body,
                                                       float outMatrix[9]);

LAIUE_PHYSICS_API bool VoxelRigidBodyAddLinearVelocity(VoxelRigidBody *body, const double delta[3]);
LAIUE_PHYSICS_API bool VoxelRigidBodyAddAngularVelocity(VoxelRigidBody *body,
                                                        const double delta[3]);
// Составляющие скоростей с насыщением до конечного double. В отличие от
// PointVelocity это именно скорость центра масс и она не зависит от позиции.
LAIUE_PHYSICS_API bool VoxelRigidBodyLinearVelocity(const VoxelRigidBody *body,
                                                    double outVelocity[3]);
LAIUE_PHYSICS_API bool VoxelRigidBodyAngularVelocity(const VoxelRigidBody *body,
                                                     double outVelocity[3]);
// Модуль скорости с насыщением: для интерфейса и решений вызывающего.
LAIUE_PHYSICS_API double VoxelRigidBodyLinearSpeed(const VoxelRigidBody *body);
LAIUE_PHYSICS_API double VoxelRigidBodyAngularSpeed(const VoxelRigidBody *body);

// Сдвигает тело вместе с началом локальных координат (rebasing).
LAIUE_PHYSICS_API bool VoxelRigidBodyTranslateBlocks(VoxelRigidBody *body,
                                                     const int64_t blockShift[3]);

// Скорость поверхности тела в точке: то, что подхватывает стоящий на нём.
LAIUE_PHYSICS_API bool VoxelRigidBodyPointVelocity(const VoxelRigidBody *body,
                                                   const double point[3], double outVelocity[3]);

// Сколько памяти нужно шагу. Physics ничего не выделяет сам: буфер даёт
// вызывающий, как и всюду в этом модуле.
LAIUE_PHYSICS_API uint32_t VoxelRigidBodyStepScratchBytes(uint32_t bodyCount);
// Читать только после успешного Step с теми же scratch и bodyCount.
LAIUE_PHYSICS_API bool VoxelRigidBodyReadStepStats(const void *scratch, uint32_t bodyCount,
                                                   uint32_t scratchBytes,
                                                   VoxelRigidStepStats *outStats);

// Один шаг симуляции для всего набора тел. collision обязан отдавать
// свойства блоков; queryDynamicColliders не используется — тела берутся из
// массива.
//
// Мир — не набор отдельных кубиков: сплошные клетки под AABB тела сначала
// склеиваются в крупные коробки, и с каждой строится тот же граневой
// manifold, что и между двумя телами. Поэтому опорой служат грани и рёбра
// тела, а не только его углы, а стыки соседних блоков пола ничего не
// цепляют — у слитой плиты боковых граней просто нет. Выталкивание идёт
// только через грань, за которой пусто: тело не выдавливается внутрь
// соседнего блока и не проваливается сквозь пол толщиной в один слой. Возвращает false при неверных аргументах, малом буфере или
// переполнении контактов или нехватке памяти внутри чисел произвольной
// точности. Каждая операция
// арифметики остаётся транзакционной и не оставляет повреждённых лимбов;
// при отказе после начала шага уже обработанные тела могут быть продвинуты,
// поэтому вызывающий, которому нужен all-or-nothing шаг, хранит свой снимок.
LAIUE_PHYSICS_API bool VoxelRigidBodyStep(VoxelRigidBody *bodies, uint32_t bodyCount,
                                          const VoxelCollisionSource *collision,
                                          const VoxelRigidStepSettings *settings, void *scratch,
                                          uint32_t scratchBytes);

// Размер включает запас на произвольное выравнивание storage. Initialize
// очищает cache; false оставляет переданный дескриптор нулевым, кроме случая
// пересечения storage с самим дескриптором (отказ без записи). Reset сохраняет
// storage/capacity, но забывает контакты. bodyCapacity должна покрывать bodyCount.
LAIUE_PHYSICS_API uint32_t VoxelRigidContactCacheBytes(uint32_t bodyCapacity);
LAIUE_PHYSICS_API bool VoxelRigidContactCacheInitialize(VoxelRigidContactCache *cache,
                                                        void *storage, uint32_t bodyCapacity,
                                                        uint32_t storageBytes);
LAIUE_PHYSICS_API void VoxelRigidContactCacheReset(VoxelRigidContactCache *cache);

// Тот же fixed step с начальными импульсами предыдущего успешного шага.
// Контакт сопоставляется один раз по stableId, локальным якорям обоих тел
// (допуск 0.01 блока) и нормали (dot >= 0.995). Для мира нужен только якорь
// тела, поэтому общий сдвиг тела и мира при rebasing не требует Reset.
// Reset обязателен после телепортации, изменения мира/формы/массы/материала/
// настроек решателя или повторного использования stableId. Обычные импульсы
// и пробуждение не требуют Reset. Cache — часть состояния воспроизведения:
// одинаковые тела без одинаковой истории cache не обещают побитового совпадения.
// Для снимка внутри того же процесса сохраняют дескриптор и его storage;
// переносимый формат сериализации этим API не задаётся. Смена capacity/reset
// также должна происходить на одинаковых тиках replay. После частичного отказа
// Step необходимо восстановить полный снимок либо остановить симуляцию.
LAIUE_PHYSICS_API bool VoxelRigidBodyStepCached(VoxelRigidBody *bodies, uint32_t bodyCount,
                                                const VoxelCollisionSource *collision,
                                                const VoxelRigidStepSettings *settings,
                                                void *scratch, uint32_t scratchBytes,
                                                VoxelRigidContactCache *contactCache);

// Persistent broadphase with the same contact/impulse order as Step/StepCached.
// contactCache is optional; broadphase must be initialized for bodyCount.
// Every awake step synchronizes current bounds (including inactive/removed/
// reordered bodies and rebasing). Sleeping bodies remain discoverable.
// Resetting/rebuilding this index changes performance, not the physical state;
// unlike the impulse cache, index topology is not part of the replay contract.
// All buffers/descriptors must be separate from each other and input objects.
LAIUE_PHYSICS_API bool VoxelRigidBodyStepIndexed(VoxelRigidBody *bodies, uint32_t bodyCount,
                                                 const VoxelCollisionSource *collision,
                                                 const VoxelRigidStepSettings *settings,
                                                 void *scratch, uint32_t scratchBytes,
                                                 VoxelRigidContactCache *contactCache,
                                                 VoxelRigidBroadphase *broadphase);

// Same fixed-step input and scratch ownership as Step. The caller initializes
// options.structSize. Old entry points retain canonical ordering and behavior.
LAIUE_PHYSICS_API bool VoxelRigidBodyStepEx(VoxelRigidBody *bodies, uint32_t bodyCount,
                                            const VoxelCollisionSource *collision,
                                            const VoxelRigidStepSettings *settings, void *scratch,
                                            uint32_t scratchBytes,
                                            const VoxelRigidStepOptions *options);

// Составное тело: набор коробок, жёстко связанных с одним VoxelRigidBody.
// center — центр дочерней коробки относительно центра масс тела, halfExtent —
// её полурёбра. children ориентируются вместе с телом. boxes не должны
// перекрываться, иначе объём и тензор инерции были бы посчитаны дважды.
typedef struct VoxelRigidCompoundBox
{
    double center[3];
    double halfExtent[3];
} VoxelRigidCompoundBox;

// Описание формы тела на время шага. boxes — массив из boxCount элементов,
// каждый center отсчитывается от COM тела. inverseInertia — полный
// симметричный обратный тензор инерции в координатах тела, row-major 3x3.
// boxes == NULL и boxCount == 0 означают обычную коробку тела.
typedef struct VoxelRigidCompoundShape
{
    const VoxelRigidCompoundBox *boxes;
    uint32_t boxCount;
    double inverseInertia[9];
} VoxelRigidCompoundShape;

// Число детей не имеет отдельного лимита игры. Оно представлено uint32_t;
// допустимость конкретного шага определяется размером буферов и суммарным
// бюджетом через VoxelRigidBodyStepCompoundScratchBytes, а не размером формы.

// Считает COM, консервативный envelope относительно COM и обратный тензор
// инерции для набора неперекрывающихся коробок при однородной плотности и
// заданной массе. Массы распределяются по объёму. mass обязана быть
// положительной конечной. При отказе (неверный вход, перекрытие, вырожденный
// тензор) возвращает false и не трогает out-параметры.
LAIUE_PHYSICS_API bool VoxelRigidCompoundMassProperties(const VoxelRigidCompoundBox *boxes,
                                                        uint32_t boxCount, double mass,
                                                        double outCenter[3],
                                                        double outHalfExtent[3],
                                                        double outInverseInertia[9]);

// Размер scratch для StepCompoundEx. primitiveCount — суммарное число
// примитивов: каждое обычное box-тело считается за один, составное тело — за
// boxCount. Общий бюджет контактов шага = primitiveCount * 16, поэтому форма
// с сотней детей получает честный бюджет, а не 16 точек на тело. Значения
// primitiveCount < bodyCount отклоняются. Обычный VoxelRigidBodyStepScratchBytes
// остаётся размером обычной сцены и равен этой функции при primitiveCount ==
// bodyCount. При primitiveCount > bodyCount scratch также хранит мировую
// геометрию детей и BVH, вычисляемые один раз за тик без persistent-указателей.
LAIUE_PHYSICS_API uint32_t VoxelRigidBodyStepCompoundScratchBytes(uint32_t bodyCount,
                                                                  uint32_t primitiveCount);

// Тот же fixed step, что и StepEx, но с составными формами. shapes — массив из
// bodyCount описаний; элемент с boxCount == 0 означает обычную коробку тела и
// сохраняет прежний быстрый путь. boxes == NULL тогда и только тогда, когда
// boxCount == 0. Шаг сам вычисляет primitiveCount по shapes и требует не меньше
// VoxelRigidBodyStepCompoundScratchBytes(bodyCount, primitiveCount) байт;
// прежний scratch обычной сцены остаётся действительным, когда все формы
// обычные. Дочерние коробки одного тела не сталкиваются между собой. Порядок
// контактов детерминирован и не зависит от числа worker. Переполнение общего
// бюджета контактов — явный false без усечения. Тензор обязан быть конечным,
// симметричным, положительно определённым, а envelope тела — покрывать детей;
// иначе шаг отклоняется до первой мутации. Рекомендуется contact cache с
// bodyCapacity >= primitiveCount. При меньшем cache шаг откажет, если реальных
// контактов больше bodyCapacity * 16; это может случиться после гравитации и
// пробуждения. Отказ не усекает контакты, но не обещает отката всего шага.
LAIUE_PHYSICS_API bool VoxelRigidBodyStepCompoundEx(VoxelRigidBody *bodies, uint32_t bodyCount,
                                                    const VoxelCollisionSource *collision,
                                                    const VoxelRigidStepSettings *settings,
                                                    void *scratch, uint32_t scratchBytes,
                                                    const VoxelRigidStepOptions *options,
                                                    const VoxelRigidCompoundShape *shapes);
