// Ручной A/B-стенд runtime-части текстурпака (`src/render/texture_pack.c`):
// покадровый lookup кадра (`TexturePackFillSliceTable`), lookup субресурса
// (`TexturePackGetSubresource`), захват расписания, имена материалов,
// перечисление паков и загрузка через `TexturePackLoadActiveFrom`.
//
// В CTest и ALL не входит. Внутренний `src/render/texture_pack.c` (и нужный
// ему `src/render/texture_build.c`) компилируется прямо в исполняемый файл:
// runtime-функции не экспортируются из `laiue_render`, а новые экспорты
// запрещены. `LAIUE_STATIC=1` снимает dllimport с определений.
//
// Стенд печатает по строке на сценарий:
//   RESULT name=<name> iters=<n> ms=<ms> checksum=<hex>
// и в конце строку с пиковой памятью процесса. A/B-скрипт сравнивает
// checksum'ы (обязаны совпасть) и собирает ms по сценариям.
//
// Windows собирает движок без CRT, поэтому вывод идёт через общий с
// тестами `test_runtime.h`.

#include "content/content_catalog.h"
#include "media/lt_encode.h"
#include "platform/system.h"
#include "render/texture_pack_internal.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <psapi.h>
#endif

#define BENCH_ROOT L"r2_23_texture_pack_bench_v1"
#define BENCH_PACK L"Bench.ltp"
#define PATH_CAP LAIUE_PLATFORM_PATH_CAPACITY

// Материалов в загрузочном сценарии пака.
#define LOAD_MATERIALS 8u
#define LOAD_TEX_SIZE 64u
// Сколько `.ltp` папок перечисляем.
#define ENUM_PACKS 200u

#if defined(_MSC_VER) || defined(__clang__)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif

// === Вывод ===

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
    for (uint32_t index = 0; index < length; ++index)
    {
        char one[2] = {digits[length - index - 1u], '\0'};
        WriteText(one);
    }
}

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

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    for (uint32_t index = 0; index < 16u; ++index)
    {
        text[index] = digits[(value >> ((15u - index) * 4u)) & 0xFu];
    }
    text[16] = '\0';
    WriteText(text);
}

static void Report(const char *name, uint64_t iters, double milliseconds, uint64_t checksum)
{
    WriteText("RESULT name=");
    WriteText(name);
    WriteText(" iters=");
    WriteUnsigned(iters);
    WriteText(" ms=");
    WriteMilliseconds(milliseconds);
    WriteText(" checksum=");
    WriteHex(checksum);
    WriteText("\n");
}

// === Фикстуры расписания ===

static TexturePackAnimationSet g_set;
static uint32_t g_out[16];
static double g_times[1024];

// Строит расписание: materialCount материалов, каждый по framesPer кадров
// длительностью 40 мс (cycle = framesPer*40). framesPer == 1 — статика.
static void FillSet(TexturePackAnimationSet *set, uint32_t materialCount, uint32_t framesPer)
{
    memset(set, 0, sizeof(*set));
    set->materialCount = materialCount;
    uint32_t slice = 0u;
    for (uint32_t material = 0; material < materialCount; ++material)
    {
        set->animation[material].firstSlice = (uint16_t)slice;
        set->animation[material].frameCount = (uint16_t)framesPer;
        set->animation[material].cycleMilliseconds = framesPer > 1u ? framesPer * 40u : 0u;
        for (uint32_t frame = 0; frame < framesPer; ++frame)
        {
            set->sliceMilliseconds[slice + frame] = framesPer > 1u ? 40u : 0u;
        }
        slice += framesPer;
    }
    set->sliceCount = slice;
}

static void FillTimes(void)
{
    // Разные времена, включая значения больше цикла (путь с делением),
    // ноль, отрицательное и NaN — все ветки `TexturePackResolveSlice`.
    static const double seed[16] = {0.0,      0.001,   0.5,    1.234,  5.0,    12.5,
                                    100.0,    3600.0,  0.04,   0.32,   10.24,  25.6,
                                    -1.0,     0.0,     1e6,    4096.0};
    for (uint32_t index = 0; index < 1024u; ++index)
    {
        g_times[index] = seed[index & 15u] + (double)(index >> 4) * 0.007;
    }
}

// === Сценарии ===

static BENCH_NOINLINE void RunFill(const char *name, uint32_t materialCount, uint32_t framesPer,
                                   uint64_t iters)
{
    FillSet(&g_set, materialCount, framesPer);
    // Прогрев вне статистики: один прогон прогревает код и таблицу.
    TexturePackFillSliceTable(&g_set, g_times[0], g_out);
    double start = PlatformMonotonicSeconds();
    for (uint64_t index = 0; index < iters; ++index)
    {
        TexturePackFillSliceTable(&g_set, g_times[(uint32_t)index & 1023u], g_out);
    }
    double milliseconds = (PlatformMonotonicSeconds() - start) * 1000.0;
    uint64_t checksum = 1469598103934665603ull;
    for (uint32_t word = 0; word < 16u; ++word)
    {
        checksum ^= g_out[word] + 0x9e3779b97f4a7c15ull + (checksum << 6) + (checksum >> 2);
    }
    Report(name, iters, milliseconds, checksum);
}

static uint64_t ChainBytes(uint32_t size)
{
    uint64_t total = 0u;
    for (uint32_t level = size;; level >>= 1)
    {
        total += (uint64_t)level * level * 4u;
        if (level == 1u) break;
    }
    return total;
}

static uint32_t MipCount(uint32_t size)
{
    uint32_t count = 1u;
    while (size > 1u)
    {
        size >>= 1;
        ++count;
    }
    return count;
}

// Синтетический квадратный пак: sliceCount слоёв, полная mip-цепочка.
static TexturePackData *MakePack(uint32_t size, uint32_t sliceCount)
{
    TexturePackData *pack = PlatformAllocate(sizeof(*pack), true);
    if (pack == NULL) return NULL;
    uint64_t chain = ChainBytes(size);
    uint64_t bytes = chain * sliceCount;
    uint8_t *pixels = PlatformAllocate((size_t)bytes, false);
    uint8_t *normals = PlatformAllocate((size_t)bytes, false);
    if (pixels == NULL || normals == NULL)
    {
        PlatformFree(pixels);
        PlatformFree(normals);
        PlatformFree(pack);
        return NULL;
    }
    for (size_t index = 0; index < (size_t)bytes; ++index)
    {
        pixels[index] = (uint8_t)(index * 31u);
        normals[index] = (uint8_t)(index * 17u + 3u);
    }
    pack->width = (uint16_t)size;
    pack->height = (uint16_t)size;
    pack->sliceCount = (uint16_t)sliceCount;
    pack->mipCount = (uint16_t)MipCount(size);
    pack->pixelBytes = (uint32_t)bytes;
    pack->pixels = pixels;
    pack->normalPixels = normals;
    pack->allocation = pixels;   // normals освобождаются отдельно ниже
    return pack;
}

static void FreePack(TexturePackData *pack)
{
    if (pack == NULL) return;
    PlatformFree((void *)pack->normalPixels);
    PlatformFree(pack->allocation);
    PlatformFree(pack);
}

static BENCH_NOINLINE void RunSubresource(const char *name, uint32_t size, uint32_t sliceCount,
                                          uint64_t sweeps)
{
    TexturePackData *pack = MakePack(size, sliceCount);
    if (pack == NULL)
    {
        WriteText("ERROR scenario=");
        WriteText(name);
        WriteText(" reason=alloc\n");
        return;
    }
    uint64_t checksum = 0u;
    double start = PlatformMonotonicSeconds();
    for (uint64_t sweep = 0; sweep < sweeps; ++sweep)
    {
        for (uint32_t slice = 0; slice < sliceCount; ++slice)
        {
            for (uint32_t mip = 0; mip < pack->mipCount; ++mip)
            {
                TexturePackSubresource sub;
                if (TexturePackGetSubresource(pack, slice, mip, &sub))
                {
                    checksum += (uint64_t)(sub.pixels - pack->pixels) + sub.byteCount +
                                sub.width + sub.height + sub.rowBytes;
                }
                TexturePackSubresource normal;
                if (TexturePackGetNormalSubresource(pack, slice, mip, &normal))
                {
                    checksum += (uint64_t)(normal.pixels - pack->normalPixels) + normal.byteCount;
                }
            }
        }
    }
    double milliseconds = (PlatformMonotonicSeconds() - start) * 1000.0;
    Report(name, sweeps, milliseconds, checksum);
    FreePack(pack);
}

static BENCH_NOINLINE void RunCapture(const char *name, uint64_t iters)
{
    static TexturePackData pack;   // без пикселей: захвату они не нужны
    memset(&pack, 0, sizeof(pack));
    FillSet(&g_set, 32u, 8u);
    pack.materialCount = (uint16_t)g_set.materialCount;
    pack.sliceCount = (uint16_t)g_set.sliceCount;
    memcpy(pack.animation, g_set.animation, sizeof(pack.animation));
    memcpy(pack.sliceMilliseconds, g_set.sliceMilliseconds, sizeof(pack.sliceMilliseconds));

    TexturePackAnimationSet *out = PlatformAllocate(sizeof(*out), true);
    if (out == NULL)
    {
        WriteText("ERROR scenario=");
        WriteText(name);
        WriteText(" reason=alloc\n");
        return;
    }
    TexturePackCaptureAnimation(out, &pack);   // прогрев
    double start = PlatformMonotonicSeconds();
    for (uint64_t index = 0; index < iters; ++index)
    {
        // Меняем один слой, чтобы результат зависел от итерации и LTO не
        // вынес вызов из цикла.
        pack.sliceMilliseconds[0] = (uint16_t)index;
        TexturePackCaptureAnimation(out, &pack);
    }
    double milliseconds = (PlatformMonotonicSeconds() - start) * 1000.0;
    uint64_t checksum = 0u;
    for (uint32_t material = 0; material < TEXTURE_PACK_MAX_LAYERS; ++material)
    {
        checksum += out->animation[material].firstSlice + out->animation[material].frameCount +
                    out->animation[material].cycleMilliseconds;
    }
    for (uint32_t slice = 0; slice < TEXTURE_PACK_MAX_SLICES; ++slice)
    {
        checksum += out->sliceMilliseconds[slice];
    }
    checksum += out->sliceCount + out->materialCount;
    Report(name, iters, milliseconds, checksum);
    PlatformFree(out);
}

static wchar_t g_nameStorage[TEXTURE_PACK_MAX_LAYERS][LAIUE_CONTENT_NAME_CAPACITY];
static const wchar_t *g_names[TEXTURE_PACK_MAX_LAYERS];

static void InitNames(void)
{
    static const wchar_t stem[] = L"main/slot";
    for (uint32_t index = 0; index < TEXTURE_PACK_MAX_LAYERS; ++index)
    {
        for (uint32_t part = 0; part < 9u; ++part) g_nameStorage[index][part] = stem[part];
        g_nameStorage[index][9] = (wchar_t)(L'0' + (index / 10u) % 10u);
        g_nameStorage[index][10] = (wchar_t)(L'0' + index % 10u);
        g_nameStorage[index][11] = L'\0';
        g_names[index] = g_nameStorage[index];
    }
}

static BENCH_NOINLINE void RunMaterialNames(const char *name, uint64_t iters)
{
    TexturePackMaterialNames *names = PlatformAllocate(sizeof(*names), true);
    if (names == NULL)
    {
        WriteText("ERROR scenario=");
        WriteText(name);
        WriteText(" reason=alloc\n");
        return;
    }
    uint64_t checksum = 0u;
    double start = PlatformMonotonicSeconds();
    for (uint64_t index = 0; index < iters; ++index)
    {
        // Меняем число имён, чтобы результат зависел от итерации.
        uint32_t count = TEXTURE_PACK_MAX_LAYERS - (uint32_t)(index & 1u);
        if (!TexturePackMaterialNamesSet(names, g_names, count))
        {
            WriteText("ERROR scenario=");
            WriteText(name);
            WriteText(" reason=set\n");
            PlatformFree(names);
            return;
        }
        checksum += names->count + (uint64_t)names->pointers[count - 1u][0];
    }
    double milliseconds = (PlatformMonotonicSeconds() - start) * 1000.0;
    Report(name, iters, milliseconds, checksum);
    PlatformFree(names);
}

// === Файловые сценарии ===

typedef struct BenchPaths
{
    wchar_t executable[PATH_CAP];
    wchar_t root[PATH_CAP];
    wchar_t textures[PATH_CAP];
    wchar_t pack[PATH_CAP];
    wchar_t main[PATH_CAP];
    wchar_t file[PATH_CAP];
} BenchPaths;

static bool Join(wchar_t *destination, uint32_t capacity, const wchar_t *base, const wchar_t *part)
{
    uint32_t length = 0u;
    while (base[length] != 0)
    {
        if (length + 1u >= capacity) return false;
        destination[length] = base[length];
        ++length;
    }
    if (length + 1u >= capacity) return false;
    destination[length++] = L'/';
    for (uint32_t index = 0u; part[index] != 0; ++index)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = part[index];
    }
    destination[length] = 0;
    return true;
}

static bool Append(wchar_t *destination, uint32_t capacity, const wchar_t *suffix)
{
    uint32_t length = 0u;
    while (destination[length] != 0) ++length;
    for (uint32_t index = 0u; suffix[index] != 0; ++index)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = suffix[index];
    }
    destination[length] = 0;
    return true;
}

static BENCH_NOINLINE bool WriteLt(const wchar_t *path, uint32_t size, uint32_t seed)
{
    uint32_t frameBytes = size * size * 4u;
    uint8_t *pixels = PlatformAllocate(frameBytes, false);
    if (pixels == NULL) return false;
    for (uint32_t index = 0; index < frameBytes; ++index)
    {
        pixels[index] = (uint8_t)(index * 31u + seed * 7u);
    }
    uint32_t encodedBytes = 0u;
    if (LtEncodedBytes(size, size, 1u, false, &encodedBytes) != LT_OK)
    {
        PlatformFree(pixels);
        return false;
    }
    uint8_t *encoded = PlatformAllocate(encodedBytes, false);
    if (encoded == NULL)
    {
        PlatformFree(pixels);
        return false;
    }
    LtTexture texture = {
        .albedoFrames = pixels,
        .normalFrames = NULL,
        .width = size,
        .height = size,
        .frameCount = 1u,
        .frameMilliseconds = NULL,
        .sourceModifiedTime = 0u,
        .sourceSizeBytes = 0u,
    };
    bool ok = LtEncode(&texture, encoded, encodedBytes, NULL) == LT_OK &&
              PlatformWriteEntireFile(path, encoded, encodedBytes);
    PlatformFree(encoded);
    PlatformFree(pixels);
    return ok;
}

static BENCH_NOINLINE bool PrepareFiles(BenchPaths *paths)
{
    if (!PlatformExecutableDirectory(paths->executable, PATH_CAP)) return false;
    if (!Join(paths->root, PATH_CAP, paths->executable, BENCH_ROOT) ||
        !Join(paths->textures, PATH_CAP, paths->root, L"textures") ||
        !Join(paths->pack, PATH_CAP, paths->textures, BENCH_PACK) ||
        !Join(paths->main, PATH_CAP, paths->pack, L"main"))
    {
        return false;
    }
    PlatformCreateDirectory(paths->root);
    PlatformCreateDirectory(paths->textures);
    PlatformCreateDirectory(paths->pack);
    PlatformCreateDirectory(paths->main);
    // Пустые папки для сценария перечисления.
    for (uint32_t index = 0; index < ENUM_PACKS; ++index)
    {
        static const wchar_t digits[] = L"0123456789";
        wchar_t name[32];
        name[0] = L'P';
        name[1] = digits[(index / 100u) % 10u];
        name[2] = digits[(index / 10u) % 10u];
        name[3] = digits[index % 10u];
        name[4] = L'.';
        name[5] = L'l';
        name[6] = L't';
        name[7] = L'p';
        name[8] = 0;
        wchar_t path[PATH_CAP];
        if (!Join(path, PATH_CAP, paths->textures, name)) return false;
        PlatformCreateDirectory(path);
    }
    // Материалы пака: `main/slot<NN>.lt`.
    for (uint32_t index = 0; index < LOAD_MATERIALS; ++index)
    {
        wchar_t name[32];
        name[0] = L's';
        name[1] = L'l';
        name[2] = L'o';
        name[3] = L't';
        name[4] = (wchar_t)(L'0' + (index / 10u) % 10u);
        name[5] = (wchar_t)(L'0' + index % 10u);
        name[6] = 0;
        if (!Join(paths->file, PATH_CAP, paths->main, name) ||
            !Append(paths->file, PATH_CAP, L".lt") ||
            !WriteLt(paths->file, LOAD_TEX_SIZE, index + 1u))
        {
            return false;
        }
    }
    return true;
}

static const wchar_t *g_loadNames[LOAD_MATERIALS];
static const wchar_t *g_missingNames[LOAD_MATERIALS];

static BENCH_NOINLINE void RunLoad(const char *name, LaiueContentCatalog *catalog,
                                   const wchar_t *const *names, uint32_t count, uint64_t iters)
{
    // Прогрев и проверка статуса вне статистики.
    TexturePackData probe;
    TexturePackLoadStatus status = TexturePackLoadActiveFrom(catalog, names, count, &probe);
    // NO_ACTIVE_PACK — сценарий fallback: движок оставляет нейтральный слой,
    // и это состояние, а не отказ.
    if (status != TEXTURE_PACK_LOAD_OK && status != TEXTURE_PACK_LOAD_INCOMPLETE &&
        status != TEXTURE_PACK_LOAD_NO_ACTIVE_PACK)
    {
        WriteText("ERROR scenario=");
        WriteText(name);
        WriteText(" status=");
        WriteUnsigned((uint32_t)status);
        WriteText("\n");
        return;
    }
    uint64_t checksum = (uint64_t)status;
    TexturePackRelease(&probe);

    double start = PlatformMonotonicSeconds();
    for (uint64_t index = 0; index < iters; ++index)
    {
        TexturePackData pack;
        status = TexturePackLoadActiveFrom(catalog, names, count, &pack);
        checksum += (uint64_t)status + pack.pixelBytes + pack.sliceCount + pack.width;
        if (pack.pixels != NULL)
        {
            // Первый и последний байт пака: пиксели обязаны совпасть.
            checksum += pack.pixels[0] + pack.pixels[pack.pixelBytes - 1u];
        }
        TexturePackRelease(&pack);
    }
    double milliseconds = (PlatformMonotonicSeconds() - start) * 1000.0;
    Report(name, iters, milliseconds, checksum);
}

static BENCH_NOINLINE void RunEnumerate(const char *name, LaiueContentCatalog *catalog,
                                        uint64_t iters)
{
    uint64_t checksum = 0u;
    for (uint64_t index = 0; index < iters; ++index)
    {
        TexturePackList list;
        if (!TexturePackEnumerateFrom(catalog, &list))
        {
            WriteText("ERROR scenario=");
            WriteText(name);
            WriteText(" reason=enumerate\n");
            return;
        }
        checksum += list.count;
        if (list.count != 0u) checksum += (uint64_t)list.entries[list.count - 1u].name[0];
        TexturePackListRelease(&list);
    }
    double start = PlatformMonotonicSeconds();
    uint64_t timed = 0u;
    for (uint64_t index = 0; index < iters; ++index)
    {
        TexturePackList list;
        if (!TexturePackEnumerateFrom(catalog, &list))
        {
            WriteText("ERROR scenario=");
            WriteText(name);
            WriteText(" reason=enumerate timed\n");
            return;
        }
        timed += list.count;
        TexturePackListRelease(&list);
    }
    double milliseconds = (PlatformMonotonicSeconds() - start) * 1000.0;
    // checksum не зависит от числа прогонов: берём количество из последнего.
    checksum = timed;
    Report(name, iters, milliseconds, checksum);
}

// === Вход ===

LAIUE_TEST_ENTRY(R2TexturePackBenchmarkEntryPoint)
{
    BenchPaths *paths = PlatformAllocate(sizeof(*paths), true);
    if (paths == NULL)
    {
        WriteText("r2 texture pack benchmark: path allocation failed\n");
        LaiueTestRuntimeExit(1);
    }
    if (!PrepareFiles(paths))
    {
        WriteText("r2 texture pack benchmark: fixture preparation failed\n");
        PlatformFree(paths);
        LaiueTestRuntimeExit(1);
    }
    InitNames();
    FillTimes();

    for (uint32_t index = 0; index < LOAD_MATERIALS; ++index)
    {
        g_loadNames[index] = g_nameStorage[index];
    }
    for (uint32_t index = 0; index < LOAD_MATERIALS; ++index)
    {
        // Имя без файла: сценарий отсутствующего материала.
        static wchar_t missingStorage[LOAD_MATERIALS][LAIUE_CONTENT_NAME_CAPACITY];
        static const wchar_t stem[] = L"main/absent";
        for (uint32_t part = 0; part < 11u; ++part) missingStorage[index][part] = stem[part];
        missingStorage[index][11] = (wchar_t)(L'0' + (index / 10u) % 10u);
        missingStorage[index][12] = (wchar_t)(L'0' + index % 10u);
        missingStorage[index][13] = 0;
        g_missingNames[index] = missingStorage[index];
    }

    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    if (catalog == NULL ||
        !LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, BENCH_PACK))
    {
        WriteText("r2 texture pack benchmark: catalog setup failed\n");
        PlatformFree(paths);
        LaiueTestRuntimeExit(1);
    }
    // Каталог без активного пака: сценарий fallback. В статике, чтобы кадр
    // точки входа остался маленьким (no-CRT не линкует __chkstk).
    static wchar_t emptyRoot[PATH_CAP];
    LaiueContentCatalog *emptyCatalog = NULL;
    if (Join(emptyRoot, PATH_CAP, paths->executable, L"r2_23_texture_pack_bench_empty"))
    {
        PlatformCreateDirectory(emptyRoot);
        emptyCatalog = LaiueContentCatalogCreate(emptyRoot);
    }

    RunFill("fill_static_8", 8u, 1u, 3000000ull);
    RunFill("fill_static_64", 64u, 1u, 2000000ull);
    RunFill("fill_anim_8x16", 8u, 16u, 800000ull);
    RunFill("fill_anim_32x8", 32u, 8u, 500000ull);
    RunFill("fill_anim_64x4", 64u, 4u, 500000ull);
    RunSubresource("subresource_256x9", 256u, 256u, 2000ull);
    RunSubresource("subresource_4096x13", 4096u, 1u, 400000ull);
    RunCapture("capture_32x8", 1000000ull);
    RunMaterialNames("material_names_64", 60000ull);
    RunEnumerate("enumerate_200", catalog, 300ull);
    RunLoad("load_warm_8", catalog, g_loadNames, LOAD_MATERIALS, 200ull);
    RunLoad("load_missing_8", catalog, g_missingNames, LOAD_MATERIALS, 300ull);
    if (emptyCatalog != NULL)
    {
        RunLoad("load_fallback", emptyCatalog, g_loadNames, LOAD_MATERIALS, 300ull);
    }

    // Пиковая память процесса: измеренное значение, не аналитическая оценка.
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters;
    memset(&counters, 0, sizeof(counters));
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    {
        WriteText("MEM peak_working_set=");
        WriteUnsigned(counters.PeakWorkingSetSize);
        WriteText(" working_set=");
        WriteUnsigned(counters.WorkingSetSize);
        WriteText(" peak_pagefile=");
        WriteUnsigned(counters.PeakPagefileUsage);
        WriteText(" pagefile=");
        WriteUnsigned(counters.PagefileUsage);
        WriteText("\n");
    }
#endif

    LaiueContentCatalogDestroy(catalog);
    if (emptyCatalog != NULL) LaiueContentCatalogDestroy(emptyCatalog);
    PlatformFree(paths);
    WriteText("r2 texture pack benchmark done\n");
    LAIUE_TEST_SUCCESS();
}
