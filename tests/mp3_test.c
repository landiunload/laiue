// Декодер MP3: данные стандарта, разбор настоящего файла и отказы.
//
// Таблицы Хаффмана и окно синтеза — не код, а данные формата, и
// проверяются они как данные: каждое дерево обязано быть полным
// префиксным кодом, каждое значение окна — точным кратным 2^-16. Эта
// проверка не зависит ни от интернета, ни от сторонних реализаций, из
// которых таблицы были сверены при сборке заголовка.
//
// Ожидаемые сэмплы сняты сторонним декодером и лежат в mp3_fixtures.h,
// поэтому тест сверяет наш разбор с чужим, а не сам с собой.

#include "media/mp3_decode.h"
#include "media/mp3_tables.h"
#include "media/sound.h"

#include "platform/system.h"
#include "test_runtime.h"
#include "mp3_fixtures.h"

#include <stdbool.h>
#include <stdint.h>

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("mp3 test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

// Обходит дерево и проверяет два независимых свойства.
//
// Первое — сумма Крафта: у полного префиксного кода она равна единице.
// Считается в целых числах, умноженной на 2^19, чтобы проверка была
// точной, а не «почти единица».
//
// Второе — набор значений. Таблица Хаффмана Layer III покрывает
// квадрат: каждая пара (x, y) при x, y меньше стороны встречается ровно
// один раз. Одной суммы Крафта мало — подмена листа внутренним узлом с
// двумя листьями её не меняет, а набор значений ломает сразу.
//
// Чего проверка не ловит: перестановку двух кодов одинаковой длины.
// Правильность значений устанавливалась при сборке заголовка сверкой
// двух независимых реализаций, а здесь стережётся только целость.
#define MP3_KRAFT_SCALE (1u << 19)

// expectedSide — сторона квадрата у таблиц больших значений. У count1
// значений шестнадцать и все они лежат в младшем полубайте: там
// проверяется отрезок 0..15, а не квадрат.
static void CheckTree(uint32_t root, uint32_t expectedSide, bool count1, const char *label)
{
    uint32_t stack[32];
    uint32_t depth[32];
    uint8_t seen[256];
    for (uint32_t index = 0; index < 256u; ++index) seen[index] = 0u;

    uint32_t count = 0u;
    uint64_t kraft = 0u;
    uint32_t leaves = 0u;
    stack[count] = root;
    depth[count] = 0u;
    count += 1u;

    while (count != 0u)
    {
        count -= 1u;
        uint32_t point = stack[count];
        uint32_t level = depth[count];
        Expect(point < sizeof(MP3_HUFFMAN_NODES) / sizeof(MP3_HUFFMAN_NODES[0]), label);

        const Mp3HuffmanNode *node = &MP3_HUFFMAN_NODES[point];
        if (node->child[0] < 0)
        {
            Expect(node->child[1] >= 0 && node->child[1] < 256, label);
            uint32_t value = (uint32_t)node->child[1];
            Expect(seen[value] == 0u, label);
            seen[value] = 1u;
            kraft += (uint64_t)MP3_KRAFT_SCALE >> level;
            leaves += 1u;
            continue;
        }
        Expect(level < 19u, label);
        Expect(node->child[0] > 0 && node->child[1] > 0, label);
        Expect(count + 2u <= 32u, label);
        stack[count] = (uint32_t)node->child[0];
        depth[count] = level + 1u;
        count += 1u;
        stack[count] = (uint32_t)node->child[1];
        depth[count] = level + 1u;
        count += 1u;
    }

    Expect(kraft == MP3_KRAFT_SCALE, label);
    if (count1)
    {
        Expect(leaves == 16u, label);
        for (uint32_t value = 0; value < 16u; ++value) Expect(seen[value] != 0u, label);
        return;
    }
    Expect(leaves == expectedSide * expectedSide, label);
    for (uint32_t x = 0; x < expectedSide; ++x)
    {
        for (uint32_t y = 0; y < expectedSide; ++y)
        {
            Expect(seen[(x << 4) | y] != 0u, label);
        }
    }
}

LAIUE_TEST_ENTRY(Mp3TestEntryPoint)
{
    // === Данные стандарта ===
    // Сторона квадрата у каждой таблицы задана стандартом.
    static const uint8_t SIDE[32] = {
        0u, 2u, 3u, 3u, 0u, 4u, 4u,  6u,  6u,  6u,  8u,  8u,  8u,  16u, 0u,  16u,
        16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u, 16u,
    };
    uint32_t checked = 0u;
    for (uint32_t table = 0; table < 32u; ++table)
    {
        uint32_t root = MP3_HUFFMAN_ROOT[table];
        if (root == MP3_HUFFMAN_NONE)
        {
            Expect(SIDE[table] == 0u, "an unused table select must have no tree");
            continue;
        }
        Expect(SIDE[table] != 0u, "a used table select must have a side");
        CheckTree(root, SIDE[table], false,
                  "a huffman table must cover its square exactly once");
        checked += 1u;
    }
    Expect(checked == 29u, "every table select but the three unused ones must have a tree");
    for (uint32_t select = 0; select < 2u; ++select)
    {
        // У count1 четыре знака упакованы в один полубайт: значений
        // ровно шестнадцать, и каждое обязано встретиться однажды.
        CheckTree(MP3_COUNT1_ROOT[select], 0u, true,
                  "a count1 table must carry every one of its sixteen values");
    }

    // Окно синтеза стандарт задаёт точными кратными 2^-16. Дробь,
    // которая туда не укладывается, означает испорченную таблицу.
    for (uint32_t index = 0; index < 512u; ++index)
    {
        float scaled = MP3_SYNTH_WINDOW[index] * 65536.0f;
        float rounded = scaled >= 0.0f ? (float)(int32_t)(scaled + 0.5f)
                                       : (float)(int32_t)(scaled - 0.5f);
        float difference = scaled > rounded ? scaled - rounded : rounded - scaled;
        Expect(difference < 0.001f, "the synthesis window must hold exact multiples of 2^-16");
    }

    // Границы полос обязаны расти и закрывать всю гранулу.
    for (uint32_t rate = 0; rate < 3u; ++rate)
    {
        Expect(MP3_BAND_LONG[rate][0] == 0u && MP3_BAND_LONG[rate][22] == 576u,
               "long bands must cover the granule");
        for (uint32_t band = 0; band < 22u; ++band)
        {
            Expect(MP3_BAND_LONG[rate][band] < MP3_BAND_LONG[rate][band + 1u],
                   "long bands must grow");
        }
        Expect(MP3_BAND_SHORT[rate][0] == 0u && MP3_BAND_SHORT[rate][13] == 192u,
               "short bands must cover one window");
        for (uint32_t band = 0; band < 13u; ++band)
        {
            Expect(MP3_BAND_SHORT[rate][band] < MP3_BAND_SHORT[rate][band + 1u],
                   "short bands must grow");
        }
    }

    // === Разбор настоящего файла ===
    Expect(Mp3Matches(MP3_SOUND_FILE, (uint32_t)sizeof(MP3_SOUND_FILE)),
           "the sample must be recognised as mpeg audio");
    Expect(SoundProbe(MP3_SOUND_FILE, (uint32_t)sizeof(MP3_SOUND_FILE)) == SOUND_FORMAT_MP3,
           "the shared probe must pick the mp3 decoder");

    SoundInfo info;
    Expect(SoundInspect(MP3_SOUND_FILE, (uint32_t)sizeof(MP3_SOUND_FILE), &info) == SOUND_OK,
           "the sample must be inspected");
    Expect(info.sampleRate == MP3_SOUND_RATE, "the sample rate must survive");
    Expect(info.channelCount == MP3_SOUND_CHANNELS, "the channel count must survive");
    // Длина обязана совпасть точно: задержка кодировщика и хвост
    // сняты по тегу, а не приблизительно.
    Expect(info.sampleCount == (uint32_t)(sizeof(MP3_SOUND_PCM) / sizeof(MP3_SOUND_PCM[0])),
           "the trimmed length must match the reference decoder exactly");

    int16_t *samples = PlatformAllocate((size_t)info.sampleCount * sizeof(int16_t), false);
    void *scratch = PlatformAllocate(info.scratchBytes, false);
    Expect(samples != NULL && scratch != NULL, "decode buffers could not be allocated");
    Expect(SoundDecodeSamples(MP3_SOUND_FILE, (uint32_t)sizeof(MP3_SOUND_FILE), &info, samples,
                              info.sampleCount, scratch, info.scratchBytes) == SOUND_OK,
           "the sample must decode");

    // Допуск в две единицы покрывает разницу округления: обратное
    // преобразование и банк фильтров у каждой реализации свои. Ошибка в
    // таблице или в перестановке блоков сдвигает сэмплы на тысячи.
    int32_t worst = 0;
    int64_t energy = 0;
    for (uint32_t index = 0; index < info.sampleCount; ++index)
    {
        int32_t difference = (int32_t)samples[index] - (int32_t)MP3_SOUND_PCM[index];
        if (difference < 0) difference = -difference;
        if (difference > worst) worst = difference;
        energy += (int64_t)MP3_SOUND_PCM[index] * MP3_SOUND_PCM[index];
    }
    Expect(worst <= 2, "the decoded samples must match the reference decoder");
    // Проверка не должна проходить на тишине: образец обязан звучать.
    Expect(energy > (int64_t)info.sampleCount * 10000, "the reference sample must carry sound");

    PlatformFree(scratch);
    PlatformFree(samples);

    // === Отказы ===
    Expect(!Mp3Matches("RIFF....WAVE", 12u), "a wav must not be taken for mpeg audio");
    Expect(!Mp3Matches("LAS1", 4u), "the own container must not be taken for mpeg audio");

    uint8_t *damaged = PlatformAllocate(sizeof(MP3_SOUND_FILE), false);
    Expect(damaged != NULL, "the scratch copy could not be allocated");

    // Заголовок первого кадра лежит по первому синхрослову.
    uint32_t frame = 0u;
    while (frame + 4u < (uint32_t)sizeof(MP3_SOUND_FILE))
    {
        if (MP3_SOUND_FILE[frame] == 0xFFu && (MP3_SOUND_FILE[frame + 1u] & 0xE0u) == 0xE0u) break;
        frame += 1u;
    }
    Expect(frame + 4u < (uint32_t)sizeof(MP3_SOUND_FILE), "the sample must contain a frame header");

    static const struct
    {
        uint32_t offset;
        uint8_t clear;
        uint8_t set;
        Mp3Status expected;
        const char *reason;
    } refusals[] = {
        // Версия 2: MPEG-2 с половинными частотами и своими таблицами.
        {1u, 0x18u, 0x10u, MP3_UNSUPPORTED_FEATURE, "mpeg-2 must be refused by name"},
        // Слой II вместо III.
        {1u, 0x06u, 0x04u, MP3_UNSUPPORTED_FEATURE, "layer ii must be refused by name"},
        // Свободный формат: длина кадра из заголовка не следует.
        {2u, 0xF0u, 0x00u, MP3_UNSUPPORTED_FEATURE, "free format must be refused by name"},
    };

    for (uint32_t index = 0; index < sizeof(refusals) / sizeof(refusals[0]); ++index)
    {
        for (uint32_t byte = 0; byte < (uint32_t)sizeof(MP3_SOUND_FILE); ++byte)
        {
            damaged[byte] = MP3_SOUND_FILE[byte];
        }
        uint32_t at = frame + refusals[index].offset;
        damaged[at] = (uint8_t)((damaged[at] & ~refusals[index].clear) | refusals[index].set);

        Mp3Info rejected;
        Mp3Status status = Mp3Inspect(damaged, (uint32_t)sizeof(MP3_SOUND_FILE), &rejected);
        Expect(status == refusals[index].expected, refusals[index].reason);
    }

    // Обрезанный файл: заголовок целый, кадров нет.
    Mp3Info shortened;
    Expect(Mp3Inspect(MP3_SOUND_FILE, frame + 8u, &shortened) != MP3_OK,
           "a file cut inside its first frame must be reported");

    // Мусор без единого синхрослова.
    for (uint32_t byte = 0; byte < 64u; ++byte) damaged[byte] = (uint8_t)(byte * 7u + 3u);
    Mp3Info garbage;
    Expect(Mp3Inspect(damaged, 64u, &garbage) == MP3_NOT_RECOGNISED,
           "a file without a frame header must be refused");

    // Проверка не должна проходить вхолостую: после пяти отказов целый
    // файл обязан по-прежнему разбираться.
    Mp3Info again;
    Expect(Mp3Inspect(MP3_SOUND_FILE, (uint32_t)sizeof(MP3_SOUND_FILE), &again) == MP3_OK,
           "a valid file must still be accepted after the refusals");
    PlatformFree(damaged);

    LaiueTestRuntimeWrite("mp3 test passed\n");
    LAIUE_TEST_SUCCESS();
}
