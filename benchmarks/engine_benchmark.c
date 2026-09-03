// Ручной бенчмарк горячих путей движка. В обычную сборку и в CTest не
// входит: включается опцией LAIUE_BUILD_BENCHMARKS и запускается руками.
//
// Измеряются два пути, которые определяют кадр: построение меша чанка
// (мешер вызывается на каждый входящий чанк) и смешивание звука (микшер
// вызывается на каждый буфер вывода). Оба портированы недавно, и без
// базовой линии любые дальнейшие оптимизации были бы угадыванием.
//
// Методика повторяет игровой бенчмарк: медиана нечётного числа выборок
// вместо среднего, чтобы редкий провал планировщика не искажал итог, и
// контрольная сумма в volatile, чтобы компилятор не выбросил работу.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "mesh/chunk_mesher.h"
#include "platform/system.h"
#include "world/world.h"

// Харнесс без CRT общий с тестами: Windows собирает движок с
// /NODEFAULTLIB, поэтому ни printf, ни exit здесь недоступны.
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_COUNT 9u
#define MESH_ITERATIONS 200u
#define MIX_ITERATIONS 400u
#define MIX_FRAME_COUNT 480u
#define MIX_VOICE_COUNT 64u
#define CLIP_FRAMES 4800u
#define AUDIO_SAMPLE_RATE 48000u

static volatile uint64_t benchmarkSink;

// === Вывод без CRT ===
// Windows собирает движок с /NODEFAULTLIB, поэтому printf недоступен.

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u) digits[length++] = '0';
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0; index < length; ++index) text[index] = digits[length - index - 1u];
    text[length] = '\0';
    WriteText(text);
}

// Три знака после запятой: больше не несёт смысла при разбросе выборок.
static void WriteMilliseconds(double value)
{
    if (value < 0.0) value = 0.0;
    uint64_t thousandths = (uint64_t)(value * 1000.0 + 0.5);
    WriteUnsigned(thousandths / 1000u);
    WriteText(".");
    uint64_t fraction = thousandths % 1000u;
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static void ReportSample(const char *name, double milliseconds, uint64_t operations)
{
    WriteText(name);
    WriteText(": median ");
    WriteMilliseconds(milliseconds);
    WriteText(" ms for ");
    WriteUnsigned(operations);
    WriteText(" operations (");
    // Операций в секунду: сравнимая величина между машинами.
    double perSecond = milliseconds > 0.0 ? (double)operations / (milliseconds / 1000.0) : 0.0;
    WriteUnsigned((uint64_t)perSecond);
    WriteText(" per second)\n");
}

static int CompareDouble(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

// Сортировка вставками: девять элементов, а qsort из CRT недоступна.
static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && CompareDouble(&samples[insertion - 1u], &value) > 0)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
    return samples[count / 2u];
}

// === Мешинг ===

// Ландшафтоподобное заполнение: сплошной камень снизу, ступенчатая
// поверхность сверху. Пустой или сплошной чанк мерить бессмысленно —
// greedy meshing на них вырождается.
static void FillChunk(World *world)
{
    for (int64_t x = 0; x < CHUNK_SIZE; ++x)
    {
        for (int64_t y = 0; y < CHUNK_SIZE; ++y)
        {
            int64_t height = 20 + ((x * 7 + y * 13) % 24);
            for (int64_t z = 0; z < height; ++z)
            {
                BlockType material = (BlockType)(1u + (uint32_t)((x + y + z) % 4));
                WorldSetBlock(world, x, y, z, material);
            }
        }
    }
}

static bool RunMeshBenchmark(void)
{
    World *world = WorldCreate(NULL);
    if (world == NULL) return false;
    FillChunk(world);

    ChunkMesherScratch *scratch = ChunkMesherScratchCreate();
    if (scratch == NULL)
    {
        WorldDestroy(world);
        return false;
    }

    // Прогрев: первый проход платит за страницы и кеш.
    ChunkQuad *quads = NULL;
    uint32_t quadCount = 0u;
    if (BuildChunkMesh(world, scratch, 0, 0, 0, &quads, &quadCount) && quads != NULL)
    {
        PlatformFree(quads);
    }

    double samples[SAMPLE_COUNT];
    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0; iteration < MESH_ITERATIONS; ++iteration)
        {
            quads = NULL;
            quadCount = 0u;
            if (BuildChunkMesh(world, scratch, 0, 0, 0, &quads, &quadCount))
            {
                benchmarkSink += quadCount;
                if (quads != NULL) PlatformFree(quads);
            }
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }

    WriteText("chunk quads: ");
    WriteUnsigned(quadCount);
    WriteText("\n");
    ReportSample("mesh.build_chunk", Median(samples, SAMPLE_COUNT), MESH_ITERATIONS);

    ChunkMesherScratchDestroy(scratch);
    WorldDestroy(world);
    return true;
}

// === Смешивание ===

static bool RunMixBenchmark(void)
{
    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameCountHint = MIX_FRAME_COUNT,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    if (AudioDeviceCreate(&configuration, &device) != AUDIO_RESULT_OK) return false;

    int16_t *samplesBuffer = PlatformAllocate(CLIP_FRAMES * sizeof(int16_t), false);
    float *frames = PlatformAllocate(MIX_FRAME_COUNT * 2u * sizeof(float), true);
    if (samplesBuffer == NULL || frames == NULL)
    {
        if (samplesBuffer != NULL) PlatformFree(samplesBuffer);
        if (frames != NULL) PlatformFree(frames);
        AudioDeviceDestroy(device);
        return false;
    }
    for (uint32_t index = 0; index < CLIP_FRAMES; ++index)
    {
        samplesBuffer[index] = (int16_t)((index % 256u) * 64u - 8192);
    }

    AudioClipDescription description = {
        .samples = samplesBuffer,
        .frameCount = CLIP_FRAMES,
        .channelCount = 1u,
        .sampleRate = AUDIO_SAMPLE_RATE,
    };
    AudioClip *clip = NULL;
    if (AudioClipCreate(device, &description, &clip) != AUDIO_RESULT_OK)
    {
        PlatformFree(samplesBuffer);
        PlatformFree(frames);
        AudioDeviceDestroy(device);
        return false;
    }
    PlatformFree(samplesBuffer);

    // Голоса зациклены и с разной скоростью: интерполяция работает на
    // каждом сэмпле, как в настоящей сцене со множеством источников.
    uint32_t startedVoices = 0u;
    for (uint32_t index = 0; index < MIX_VOICE_COUNT; ++index)
    {
        AudioVoiceParameters parameters = {
            .volume = 0.5f,
            .pan = ((float)(index % 5u) - 2.0f) * 0.5f,
            .speed = 0.75f + (float)(index % 7u) * 0.1f,
            .looping = true,
        };
        if (AudioVoicePlay(device, clip, &parameters) != AUDIO_VOICE_NONE) ++startedVoices;
    }

    AudioDeviceRenderFrames(device, frames, MIX_FRAME_COUNT);   // прогрев

    double samples[SAMPLE_COUNT];
    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0; iteration < MIX_ITERATIONS; ++iteration)
        {
            AudioDeviceRenderFrames(device, frames, MIX_FRAME_COUNT);
            benchmarkSink += (uint64_t)(frames[0] * 1000.0f);
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }

    WriteText("active voices: ");
    WriteUnsigned(startedVoices);
    WriteText("\n");
    ReportSample("audio.mix_buffer", Median(samples, SAMPLE_COUNT), MIX_ITERATIONS);

    // Секунд звука в секунду процессорного времени: сколько запаса
    // остаётся у потока вывода на реальном буфере.
    double median = Median(samples, SAMPLE_COUNT);
    if (median > 0.0)
    {
        double audioSeconds =
            (double)MIX_ITERATIONS * (double)MIX_FRAME_COUNT / (double)AUDIO_SAMPLE_RATE;
        WriteText("audio.realtime_factor: ");
        WriteUnsigned((uint64_t)(audioSeconds / (median / 1000.0)));
        WriteText("x\n");
    }

    AudioDeviceStopAllVoices(device);
    AudioDeviceRenderFrames(device, frames, MIX_FRAME_COUNT);
    PlatformFree(frames);
    AudioClipDestroy(clip);
    AudioDeviceDestroy(device);
    return true;
}

LAIUE_TEST_ENTRY(EngineBenchmarkEntryPoint)
{
    WriteText("laiue engine benchmark\n");
    WriteText("voices=");
    WriteUnsigned(MIX_VOICE_COUNT);
    WriteText(" frames=");
    WriteUnsigned(MIX_FRAME_COUNT);
    WriteText(" samples=");
    WriteUnsigned(SAMPLE_COUNT);
    WriteText("\n\n");

    if (!RunMeshBenchmark())
    {
        WriteText("mesh benchmark could not run\n");
        LaiueTestRuntimeExit(1);
    }
    WriteText("\n");
    if (!RunMixBenchmark())
    {
        WriteText("mix benchmark could not run\n");
        LaiueTestRuntimeExit(1);
    }

    // Ссылка на sink не даёт компилятору выбросить измеряемую работу.
    if (benchmarkSink == UINT64_MAX) WriteText("");
    LAIUE_TEST_SUCCESS();
}
