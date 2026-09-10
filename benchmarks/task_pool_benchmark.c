// Ручной benchmark накладных расходов пула задач. В ALL не входит и в CTest
// не регистрируется: его запускают осознанно и читают глазами.
//
// Меряется не пропускная способность, а стоимость самого dispatch. Цветной
// решатель физики за один шаг проходит десяток групп подряд, и каждая группа
// заканчивается барьером: следующая не может начаться, пока не закончилась
// предыдущая. Поэтому важна не сумма работы, а цена одного круга
// "опубликовать, разбудить, раздать диапазоны, дождаться всех".
//
// Стоимость callback отделена явно: тот же самый callback с тем же самым
// разбиением на диапазоны прогоняется прямым циклом без пула, и обе цифры
// печатаются рядом. Разница между ними и есть накладной расход пула.
// Полезной нагрузкой управляет work: ноль означает одну запись на индекс,
// то есть почти пустой callback, на котором виден чистый накладной расход.
//
// Это микробенчмарк одной подсистемы. Выводы о FPS по нему делать нельзя:
// для этого есть laiue_physics_benchmark и спавнер игры.

#include "platform/system.h"
#include "task/task_pool.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Столько индексов хватает на самый широкий случай: 512 диапазонов по 32.
#define TASK_BENCH_MAX_INDICES 16384u
// Столько групп с барьерами решатель проходит за один шаг симуляции.
#define TASK_BENCH_GROUPS 12u
// Из повторов берётся минимум: посторонняя нагрузка машины умеет только
// добавлять время, поэтому минимум ближе к настоящей стоимости, чем среднее.
#define TASK_BENCH_SAMPLES 9u
// Диапазонов на выборку: столько работы, чтобы выборка не утонула в
// разрешении часов, и одинаково для всех случаев, чтобы они были сравнимы.
#define TASK_BENCH_RANGES_PER_SAMPLE 196608u
#define TASK_BENCH_MIN_DISPATCHES 48u
#define TASK_BENCH_MAX_DISPATCHES 3072u

static uint32_t benchCells[TASK_BENCH_MAX_INDICES];
static volatile uint64_t benchSink;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
    {
        digits[length++] = '0';
    }
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

// Три знака после запятой: разница между вариантами здесь измеряется
// десятыми долями микросекунды, и целые микросекунды её стирают.
static void WriteFixed(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.000");
        return;
    }
    if (value > 1000000000.0)
    {
        value = 1000000000.0;
    }
    uint64_t whole = (uint64_t)value;
    WriteUnsigned(whole);
    WriteText(".");
    uint64_t fraction = (uint64_t)((value - (double)whole) * 1000.0);
    if (fraction > 999u)
    {
        fraction = 999u;
    }
    if (fraction < 100u)
    {
        WriteText("0");
    }
    if (fraction < 10u)
    {
        WriteText("0");
    }
    WriteUnsigned(fraction);
}

typedef struct TouchJob
{
    uint32_t *cells;
    uint32_t epoch;
    uint32_t work;
} TouchJob;

// Callback пишет в свой диапазон и ничего не читает за его пределами: это
// та же дисциплина, что у решателя внутри одного цвета. Запись видна снаружи
// через benchSink, поэтому цикл не может быть выброшен оптимизатором.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void TouchRange(void *context, uint32_t begin, uint32_t end)
{
    TouchJob *job = context;
    uint32_t work = job->work;
    uint32_t epoch = job->epoch;
    for (uint32_t index = begin; index < end; ++index)
    {
        uint32_t value = index ^ epoch;
        for (uint32_t step = 0u; step < work; ++step)
        {
            value = value * 1664525u + 1013904223u;
        }
        job->cells[index] = value;
    }
}

// Указатель берётся через volatile: иначе whole-program optimization
// подставит TouchRange прямо в эталонный цикл, и эталон станет мерить не то
// же самое, что пул. Пул вызывает callback только косвенно, и эталон обязан
// вызывать его так же, иначе разница между ними перестаёт быть накладным
// расходом пула.
static LaiueTaskRangeFunction volatile benchFunction = TouchRange;

// Прямой прогон без пула, с тем же разбиением на диапазоны: столько стоит
// сам callback. Из пула эту стоимость не вычесть иначе, она зависит и от
// grain, и от объёма работы на индекс.
static void RunSerial(LaiueTaskRangeFunction function, TouchJob *job, uint32_t count,
                      uint32_t grain)
{
    for (uint32_t begin = 0u; begin < count;)
    {
        uint32_t remaining = count - begin;
        uint32_t length = remaining < grain ? remaining : grain;
        function(job, begin, begin + length);
        begin += length;
    }
}

static uint32_t DispatchesPerSample(uint32_t ranges)
{
    uint32_t dispatches = TASK_BENCH_RANGES_PER_SAMPLE / ranges;
    if (dispatches < TASK_BENCH_MIN_DISPATCHES)
    {
        dispatches = TASK_BENCH_MIN_DISPATCHES;
    }
    if (dispatches > TASK_BENCH_MAX_DISPATCHES)
    {
        dispatches = TASK_BENCH_MAX_DISPATCHES;
    }
    // Группы идут сериями по TASK_BENCH_GROUPS, как у решателя за шаг.
    dispatches -= dispatches % TASK_BENCH_GROUPS;
    return dispatches == 0u ? TASK_BENCH_GROUPS : dispatches;
}

static void ConsumeCells(uint32_t count)
{
    uint64_t total = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        total += benchCells[index];
    }
    benchSink += total;
}

// executor == NULL означает прямой прогон без пула.
static double BestSeconds(const LaiueTaskExecutor *executor, uint32_t count, uint32_t grain,
                          uint32_t work, uint32_t dispatches)
{
    TouchJob job = {.cells = benchCells, .epoch = 0u, .work = work};
    LaiueTaskRangeFunction function = benchFunction;
    double best = 0.0;
    for (uint32_t sample = 0u; sample < TASK_BENCH_SAMPLES; ++sample)
    {
        double begin = PlatformMonotonicSeconds();
        for (uint32_t dispatch = 0u; dispatch < dispatches; ++dispatch)
        {
            job.epoch = dispatch + 1u;
            if (executor != NULL)
            {
                executor->run(executor->context, count, grain, function, &job);
            }
            else
            {
                RunSerial(function, &job, count, grain);
            }
        }
        double elapsed = PlatformMonotonicSeconds() - begin;
        ConsumeCells(count);
        if (sample == 0u || elapsed < best)
        {
            best = elapsed;
        }
    }
    return best;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void ReportCase(const char *name, uint32_t participants, uint32_t ranges, uint32_t grain,
                       uint32_t work, double poolSeconds, double serialSeconds, uint32_t dispatches)
{
    double perDispatch = poolSeconds * 1000000.0 / (double)dispatches;
    double serialPerDispatch = serialSeconds * 1000000.0 / (double)dispatches;
    WriteText("task_pool ");
    WriteText(name);
    WriteText(" p=");
    WriteUnsigned(participants);
    WriteText(" ranges=");
    WriteUnsigned(ranges);
    WriteText(" grain=");
    WriteUnsigned(grain);
    WriteText(" work=");
    WriteUnsigned(work);
    WriteText(" pool_us=");
    WriteFixed(perDispatch);
    WriteText(" serial_us=");
    WriteFixed(serialPerDispatch);
    WriteText(" overhead_ns_per_range=");
    double overhead = (perDispatch - serialPerDispatch) * 1000.0 / (double)ranges;
    if (overhead < 0.0)
    {
        WriteText("-");
        overhead = -overhead;
    }
    WriteFixed(overhead);
    WriteText("\n");
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void MeasureCase(const LaiueTaskExecutor *executor, const char *name, uint32_t participants,
                        uint32_t ranges, uint32_t grain, uint32_t work)
{
    uint32_t count = ranges * grain;
    if (count > TASK_BENCH_MAX_INDICES)
    {
        WriteText("task_pool case exceeds the index budget\n");
        LaiueTestRuntimeExit(1);
    }
    uint32_t dispatches = DispatchesPerSample(ranges);
    double poolSeconds = BestSeconds(executor, count, grain, work, dispatches);
    double serialSeconds = BestSeconds(NULL, count, grain, work, dispatches);
    ReportCase(name, participants, ranges, grain, work, poolSeconds, serialSeconds, dispatches);
}

static void MeasureParticipants(uint32_t participants)
{
    LaiueTaskPool *pool = LaiueTaskPoolCreate(participants);
    if (pool == NULL)
    {
        WriteText("task pool creation failed\n");
        LaiueTestRuntimeExit(1);
    }
    LaiueTaskExecutor executor = {.structSize = (uint32_t)sizeof(executor)};
    if (!LaiueTaskPoolGetExecutor(pool, &executor))
    {
        WriteText("task pool executor failed\n");
        LaiueTestRuntimeExit(1);
    }

    // Барьер: ровно один диапазон на участника и почти пустой callback.
    // Здесь видна цена круга "разбудить всех и дождаться всех" без раздачи.
    MeasureCase(&executor, "barrier", participants, participants, 1u, 0u);

    // Раздача: диапазонов много, работы в них нет. Здесь видна цена
    // атомарного курсора и лишних пробуждений.
    const uint32_t claimRanges[] = {32u, 128u, 512u, 2048u};
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        MeasureCase(&executor, "claim", participants, claimRanges[index], 1u, 0u);
    }

    // Форма решателя: grain 32, как у подготовки контактов и интегратора,
    // и небольшая арифметика на индекс.
    const uint32_t solverRanges[] = {32u, 128u, 512u};
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        MeasureCase(&executor, "solver", participants, solverRanges[index], 32u, 8u);
    }

    LaiueTaskPoolDestroy(pool);
}

LAIUE_TEST_ENTRY(TaskPoolBenchmarkEntryPoint)
{
    WriteText("task_pool benchmark: logical_processors=");
    WriteUnsigned(LaiueTaskLogicalProcessorCount());
    WriteText("\n");
    const uint32_t participants[] = {1u, 2u, 4u, 8u};
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        MeasureParticipants(participants[index]);
    }
    WriteText("task_pool benchmark: done sink=");
    WriteUnsigned(benchSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
