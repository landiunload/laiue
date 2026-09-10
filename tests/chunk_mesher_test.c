// Проверка мешера чанка независимым оракулом.
//
// Мешер выдаёт прямоугольники: greedy-проход сливает соседние грани одного
// материала в один квад. Сравнивать его с самим собой бессмысленно, поэтому
// эталон строится совсем другим способом — простым обходом блоков через
// публичный WorldGetBlock. Для каждого непустого блока и каждого из шести
// направлений грань обязана существовать ровно тогда, когда сосед в этом
// направлении пуст. Ни разбиения на прямоугольники, ни битовых масок, ни
// WorldFillRegion эталон не использует.
//
// Затем каждый выданный квад раскрывается обратно в элементарные грани, и
// два множества сверяются поштучно: пропущенная грань, лишняя грань,
// задвоенная грань и неверный материал ловятся по отдельности.
//
// Порядок выдачи проверяется отдельно хешем: он часть формата геометрии
// (вершинный шейдер разворачивает квады по SV_VertexID), и перестановка
// квадов обязана считаться изменением, даже если множество граней то же.

#include "mesh/chunk_mesher.h"
#include "platform/system.h"
#include "render/chunk_geometry.h"
#include "test_runtime.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

#define MESHER_BLOCKS ((uint32_t)CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)
// Порядок из chunk_geometry.h: +X, -X, +Y, -Y, +Z, -Z.
#define MESHER_FACE_COUNT 6u

static uint32_t mesherChecks;
static uint32_t mesherFailures;

// Ожидаемый материал грани; ноль означает, что грани здесь быть не должно.
static uint8_t mesherExpected[MESHER_FACE_COUNT][MESHER_BLOCKS];

static void MesherWrite(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void MesherWriteUnsigned(uint64_t value)
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
    MesherWrite(text);
}

static void MesherWriteHex(uint64_t value)
{
    static const char digits[17] = "0123456789abcdef";
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        text[2u + index] = digits[(value >> ((15u - index) * 4u)) & 15u];
    }
    text[18] = '\0';
    MesherWrite(text);
}

static void MesherExpect(bool condition, const char *message)
{
    ++mesherChecks;
    if (condition)
    {
        return;
    }
    ++mesherFailures;
    MesherWrite("Chunk mesher check failed: ");
    MesherWrite(message);
    MesherWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t BlockIndex(int32_t x, int32_t y, int32_t z)
{
    return ((uint32_t)x * CHUNK_SIZE + (uint32_t)y) * CHUNK_SIZE + (uint32_t)z;
}

// Смещение соседа для каждой грани, в том же порядке, что и номера граней.
static void FaceOffset(uint32_t face, int32_t outOffset[3])
{
    static const int32_t offsets[MESHER_FACE_COUNT][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    outOffset[0] = offsets[face][0];
    outOffset[1] = offsets[face][1];
    outOffset[2] = offsets[face][2];
}

// === Наполнения мира ===

typedef enum FillKind
{
    FILL_EMPTY,
    FILL_SOLID_OPEN,   // сплошной чанк, вокруг пусто
    FILL_SOLID_CLOSED, // сплошной чанк и сплошной halo: видимых граней нет
    FILL_SINGLE_CENTER,
    FILL_SINGLE_CORNERS, // по блоку в каждом из восьми углов чанка
    FILL_SLAB,
    FILL_CHECKER,
    FILL_SPARSE,
    FILL_MATERIALS, // материалы 1..255 вперемешку
    FILL_HALO,      // блоки у границы и соседи за ней
    FILL_EDGE_LINES,
    FILL_MATERIAL_BANDS,      // длинные полосы, разорванные только материалом
    FILL_BAND_GAPS,           // те же полосы, разорванные ещё и пропусками
    FILL_HIGH_MATERIAL_BANDS, // длинные полосы материалов с установленным старшим битом
    FILL_DENSITY_127,
    FILL_DENSITY_128,
    FILL_DENSITY_129,
    FILL_DENSITY_255,
    FILL_DENSITY_256,
    FILL_DENSITY_257,
    FILL_DENSITY_4095,
    FILL_DENSITY_4096,
    FILL_DENSITY_4097,
    FILL_CORE_EMPTY_HALO, // внутри пусто, но регион с halo смешанный
    FILL_MATRIX_X_255,
    FILL_MATRIX_X_256,
    FILL_MATRIX_X_257,
    FILL_MATRIX_Y_255,
    FILL_MATRIX_Y_256,
    FILL_MATRIX_Y_257,
    FILL_COLUMN_BITS, // в каждой Z-колонне свой 64-битный узор: транспонирование без структуры
    FILL_KIND_COUNT
} FillKind;

static const char *FillName(FillKind kind)
{
    static const char *names[FILL_KIND_COUNT] = {
        "empty",        "solid_open",     "solid_closed", "single_center",       "single_corners",
        "slab",         "checker",        "sparse",       "materials",           "halo",
        "edge_lines",   "material_bands", "band_gaps",    "high_material_bands", "density_127",
        "density_128",  "density_129",    "density_255",  "density_256",         "density_257",
        "density_4095", "density_4096",   "density_4097", "core_empty_halo",     "matrix_x_255",
        "matrix_x_256", "matrix_x_257",   "matrix_y_255", "matrix_y_256",        "matrix_y_257",
        "column_bits",
    };
    return names[kind];
}

// Номер полосы материала вдоль одной оси. Границы стоят вокруг кратных
// шестнадцати: материал сверяется окнами по шестнадцать блоков, и ошибка на
// границе окна видна только тогда, когда материал меняется рядом с ней.
static uint32_t MaterialBand(int64_t value)
{
    static const int64_t edges[13] = {1, 2, 15, 16, 17, 31, 32, 33, 47, 48, 49, 62, 63};
    uint32_t band = 0u;
    for (uint32_t index = 0u; index < 13u; ++index)
    {
        if (value >= edges[index])
        {
            ++band;
        }
    }
    return band;
}

// Соседние блоки одной полосы одинаковы, соседние полосы всегда разные: шаг
// по каждой оси свой и в сумму укладывается без переносов.
static BlockType BandBlock(int64_t x, int64_t y, int64_t z)
{
    return (BlockType)(1u + MaterialBand(x) + MaterialBand(y) * 3u + MaterialBand(z) * 5u);
}

// Каждая Z-колонна получает свой 64-битный узор из хеша (x, y). Тогда битовые
// матрицы, которые мешер транспонирует при построении поперечных колонн и
// плоскостей, нерегулярны во всех шестидесяти четырёх разрядах: крайние окна
// обмена проверяются не только на краях однородных полос.
static uint64_t ColumnBits(int64_t x, int64_t y)
{
    uint64_t h = (uint64_t)(uint32_t)(x * 0x9E3779B1ll);
    h ^= (uint64_t)(uint32_t)(y * 0x85EBCA77ll) << 32;
    h *= 0x100000001B3ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}

static BlockType FillBlock(FillKind kind, int64_t x, int64_t y, int64_t z)
{
    // Считается по абсолютным координатам внутри чанка, включая halo -1 и
    // CHUNK_SIZE: наполнение обязано быть определено и за границей.
    bool inside = x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE;
    switch (kind)
    {
    case FILL_EMPTY:
        return BLOCK_AIR;
    case FILL_SOLID_OPEN:
        return inside ? (BlockType)3u : BLOCK_AIR;
    case FILL_SOLID_CLOSED:
        return (BlockType)3u;
    case FILL_SINGLE_CENTER:
        return x == 32 && y == 32 && z == 32 ? (BlockType)7u : BLOCK_AIR;
    case FILL_SINGLE_CORNERS:
    {
        bool cornerX = x == 0 || x == CHUNK_SIZE - 1;
        bool cornerY = y == 0 || y == CHUNK_SIZE - 1;
        bool cornerZ = z == 0 || z == CHUNK_SIZE - 1;
        return inside && cornerX && cornerY && cornerZ ? (BlockType)5u : BLOCK_AIR;
    }
    case FILL_SLAB:
        return inside && z >= 10 && z < 14 ? (BlockType)2u : BLOCK_AIR;
    case FILL_CHECKER:
        return inside && ((x + y + z) & 1) == 0 ? (BlockType)4u : BLOCK_AIR;
    case FILL_SPARSE:
        return inside && (x % 3) == 0 && (y % 3) == 0 && (z % 3) == 0 ? (BlockType)6u : BLOCK_AIR;
    case FILL_MATERIALS:
        // Все 255 значений материала встречаются, и соседние блоки почти
        // всегда разного материала: слить нечего, но и спутать нельзя.
        return inside ? (BlockType)(1u + (uint32_t)((x * 7 + y * 13 + z * 31) % 255)) : BLOCK_AIR;
    case FILL_MATERIAL_BANDS:
        // Сплошной чанк: грани лежат шестью плоскостями по 64x64, и рвёт их
        // только материал. Ровно тот случай, ради которого длинная полоса
        // разбирается группами.
        return inside ? BandBlock(x, y, z) : BLOCK_AIR;
    case FILL_HIGH_MATERIAL_BANDS:
        // FILL_MATERIALS использует все байты, но полосы там одноэлементные.
        // Здесь SIMD-сравнение обязано корректно размножать 0x80 и 0xff,
        // включая прижатые к концу ряда окна и смену материала рядом с ними.
        return inside ? (BlockType)((BandBlock(x, y, z) & 1u) != 0u ? 0x80u : 0xffu) : BLOCK_AIR;
    case FILL_DENSITY_127:
    case FILL_DENSITY_128:
    case FILL_DENSITY_129:
    case FILL_DENSITY_255:
    case FILL_DENSITY_256:
    case FILL_DENSITY_257:
    case FILL_DENSITY_4095:
    case FILL_DENSITY_4096:
    case FILL_DENSITY_4097:
    {
        // Границы sparse/dense выбора для одной матрицы 64x64 и граница
        // заполнения целой плоскости y=0. Соседние случаи различаются
        // одной записью, но могут выбирать разные пути построения колонн.
        static const uint32_t counts[] = {127u, 128u, 129u, 255u, 256u, 257u, 4095u, 4096u, 4097u};
        uint32_t count = counts[kind - FILL_DENSITY_127];
        uint32_t index = (uint32_t)((y * CHUNK_SIZE + x) * CHUNK_SIZE + z);
        return inside && index < count ? (BlockType)13u : BLOCK_AIR;
    }
    case FILL_CORE_EMPTY_HALO:
        return inside ? BLOCK_AIR : (BlockType)11u;
    case FILL_MATRIX_X_255:
    case FILL_MATRIX_X_256:
    case FILL_MATRIX_X_257:
    case FILL_MATRIX_Y_255:
    case FILL_MATRIX_Y_256:
    case FILL_MATRIX_Y_257:
    {
        bool alongX = kind <= FILL_MATRIX_X_257;
        int64_t plane = alongX ? y : x;
        int64_t column = alongX ? x : y;
        uint32_t count = 255u + ((uint32_t)(kind - FILL_MATRIX_X_255) % 3u);
        // Полная дальняя плоскость переводит чанк в dense-путь (>4096),
        // а ближняя матрица отдельно пересекает его внутренний порог 256.
        bool occupied =
            plane == CHUNK_SIZE - 1 || (plane == 0 && (uint32_t)(column * CHUNK_SIZE + z) < count);
        return inside && occupied ? (BlockType)17u : BLOCK_AIR;
    }
    case FILL_COLUMN_BITS:
    {
        uint64_t bits = ColumnBits(x, y);
        return inside && ((bits >> (uint32_t)z) & 1ull) != 0ull ? (BlockType)23u : BLOCK_AIR;
    }
    case FILL_BAND_GAPS:
    {
        // Те же полосы, но с дырами. Дыры по z рвут полосы у граней ±X и ±Y,
        // дыры по x — у граней ±Z, и обе стоят и внутри окна, и на его краю.
        bool hole = z == 3 || z == 16 || z == 17 || z == 33 || z == 48 || x == 15 || x == 32;
        return inside && !hole ? BandBlock(x, y, z) : BLOCK_AIR;
    }
    case FILL_HALO:
        // Оболочка толщиной в блок внутри чанка плюс сплошной слой снаружи по
        // -X и +Z: часть граничных граней обязана быть срезана соседями.
        if (!inside)
        {
            return x == -1 || z == CHUNK_SIZE ? (BlockType)8u : BLOCK_AIR;
        }
        return x == 0 || x == CHUNK_SIZE - 1 || y == 0 || y == CHUNK_SIZE - 1 || z == 0 ||
                       z == CHUNK_SIZE - 1
                   ? (BlockType)1u
                   : BLOCK_AIR;
    default:
        // Рёбра чанка: три взаимно перпендикулярные линии от одного угла.
        if (!inside)
        {
            return BLOCK_AIR;
        }
        return (y == 0 && z == 0) || (x == 0 && z == 0) || (x == 0 && y == 0) ? (BlockType)9u
                                                                              : BLOCK_AIR;
    }
}

static void ApplyFill(World *world, FillKind kind, int64_t baseX, int64_t baseY, int64_t baseZ)
{
    for (int64_t x = -1; x <= CHUNK_SIZE; ++x)
    {
        for (int64_t y = -1; y <= CHUNK_SIZE; ++y)
        {
            for (int64_t z = -1; z <= CHUNK_SIZE; ++z)
            {
                BlockType block = FillBlock(kind, x, y, z);
                if (block != BLOCK_AIR)
                {
                    WorldSetBlock(world, baseX + x, baseY + y, baseZ + z, block);
                }
            }
        }
    }
}

// === Эталон ===

// Обход блоков без единой оптимизации: грань есть тогда и только тогда,
// когда блок непуст, а сосед в этом направлении пуст.
static uint32_t BuildExpected(World *world, int64_t baseX, int64_t baseY, int64_t baseZ)
{
    for (uint32_t face = 0u; face < MESHER_FACE_COUNT; ++face)
    {
        for (uint32_t index = 0u; index < MESHER_BLOCKS; ++index)
        {
            mesherExpected[face][index] = 0u;
        }
    }
    uint32_t total = 0u;
    for (int32_t x = 0; x < CHUNK_SIZE; ++x)
    {
        for (int32_t y = 0; y < CHUNK_SIZE; ++y)
        {
            for (int32_t z = 0; z < CHUNK_SIZE; ++z)
            {
                BlockType block = WorldGetBlock(world, baseX + x, baseY + y, baseZ + z);
                if (block == BLOCK_AIR)
                {
                    continue;
                }
                for (uint32_t face = 0u; face < MESHER_FACE_COUNT; ++face)
                {
                    int32_t offset[3];
                    FaceOffset(face, offset);
                    BlockType neighbour = WorldGetBlock(
                        world, baseX + x + offset[0], baseY + y + offset[1], baseZ + z + offset[2]);
                    if (neighbour != BLOCK_AIR)
                    {
                        continue;
                    }
                    mesherExpected[face][BlockIndex(x, y, z)] = (uint8_t)block;
                    ++total;
                }
            }
        }
    }
    return total;
}

static uint64_t HashQuads(const ChunkQuad *quads, uint32_t count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t index = 0u; index < count; ++index)
    {
        uint32_t words[2] = {quads[index].positionAndFace, quads[index].extents};
        for (uint32_t word = 0u; word < 2u; ++word)
        {
            for (uint32_t byte = 0u; byte < 4u; ++byte)
            {
                hash ^= (words[word] >> (byte * 8u)) & 0xffu;
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    return hash;
}

// Раскрывает квады обратно в элементарные грани и гасит ими эталон.
static void ConsumeQuads(const ChunkQuad *quads, uint32_t count, const char *name)
{
    (void)name;
    for (uint32_t index = 0u; index < count; ++index)
    {
        uint32_t position = quads[index].positionAndFace;
        uint32_t extents = quads[index].extents;
        uint32_t startX = position & 0x7fu;
        uint32_t startZ = (position >> 7) & 0x7fu;
        uint32_t startY = (position >> 14) & 0x7fu;
        uint32_t face = (position >> 21) & 7u;
        uint32_t material = (position >> 24) & 0xffu;
        uint32_t extentX = extents & 0x7fu;
        uint32_t extentZ = (extents >> 7) & 0x7fu;
        uint32_t extentY = (extents >> 14) & 0x7fu;

        MesherExpect(face < MESHER_FACE_COUNT, "номер грани в пределах шести направлений");
        MesherExpect(material != 0u, "материал квада не может быть воздухом");
        MesherExpect(extentX != 0u && extentY != 0u && extentZ != 0u,
                     "нулевой размер квада не имеет смысла");
        MesherExpect(startX + extentX <= (uint32_t)CHUNK_SIZE &&
                         startY + extentY <= (uint32_t)CHUNK_SIZE &&
                         startZ + extentZ <= (uint32_t)CHUNK_SIZE,
                     "квад обязан лежать внутри чанка");
        // Квад плоский: ровно один размер равен единице по нормали грани.
        uint32_t normalExtent = face < 2u ? extentX : (face < 4u ? extentY : extentZ);
        MesherExpect(normalExtent == 1u, "квад обязан быть плоским по своей нормали");

        for (uint32_t x = startX; x < startX + extentX; ++x)
        {
            for (uint32_t y = startY; y < startY + extentY; ++y)
            {
                for (uint32_t z = startZ; z < startZ + extentZ; ++z)
                {
                    uint32_t slot = BlockIndex((int32_t)x, (int32_t)y, (int32_t)z);
                    MesherExpect(mesherExpected[face][slot] != 0u,
                                 "лишняя или задвоенная грань в выдаче мешера");
                    MesherExpect(mesherExpected[face][slot] == (uint8_t)material,
                                 "материал грани не совпадает с материалом блока");
                    mesherExpected[face][slot] = 0u;
                }
            }
        }
    }
}

static void CheckAllConsumed(void)
{
    for (uint32_t face = 0u; face < MESHER_FACE_COUNT; ++face)
    {
        for (uint32_t index = 0u; index < MESHER_BLOCKS; ++index)
        {
            MesherExpect(mesherExpected[face][index] == 0u, "мешер потерял грань");
        }
    }
}

// Эталонные хеши порядка выдачи. Координаты в кваде локальные, а наполнение
// задано локально же, поэтому хеш не зависит ни от места чанка в мире, ни от
// переноса начала мира — одно и то же наполнение обязано давать одно и то же
// число во всех прогонах. Перестановка квадов, другое разбиение полосы на
// прямоугольники или другой материал меняют хеш, даже если множество
// элементарных граней осталось прежним.
static uint64_t MesherGoldenHash(FillKind kind)
{
    static const uint64_t hashes[FILL_KIND_COUNT] = {
        UINT64_C(0x14650fb0739d0383), // empty
        UINT64_C(0x8f553e3ccd54660a), // solid_open
        UINT64_C(0x14650fb0739d0383), // solid_closed
        UINT64_C(0x6b86c3af2cc21c83), // single_center
        UINT64_C(0x91f085b55ec9b19b), // single_corners
        UINT64_C(0xb8e21339d13adfc2), // slab
        UINT64_C(0xc1b1780b8e7fe783), // checker
        UINT64_C(0x5b0e346486f80f1b), // sparse
        UINT64_C(0x9de9fd5d98aa41e0), // materials
        UINT64_C(0x4006bb8c8917f757), // halo
        UINT64_C(0x32e3763eaf7472ca), // edge_lines
        UINT64_C(0x75c346b444561787), // material_bands
        UINT64_C(0x581c9a27b5e18fb3), // band_gaps
        UINT64_C(0x133de3c107640c2f), // high_material_bands: frozen pre-optimization DLL
        UINT64_C(0xb6bb538159c265c4), // density_127
        UINT64_C(0x040878aa55c4d32f), // density_128
        UINT64_C(0x9adc39bf0c2ec607), // density_129
        UINT64_C(0x2a3ca55a066183a6), // density_255
        UINT64_C(0x4dd3d335a8f54375), // density_256
        UINT64_C(0x995ae8f95723a3d1), // density_257
        UINT64_C(0x3955ee3179d0e28a), // density_4095
        UINT64_C(0x4c84c320bb4532d1), // density_4096
        UINT64_C(0x2bdfdd9da68a50eb), // density_4097
        UINT64_C(0x14650fb0739d0383), // core_empty_halo
        UINT64_C(0x0ed325687ce3a74c), // matrix_x_255
        UINT64_C(0xdcfa0ffa68b75537), // matrix_x_256
        UINT64_C(0x22a041ae202cff33), // matrix_x_257
        UINT64_C(0x12f53bcba3c4a974), // matrix_y_255
        UINT64_C(0xaaa34ef7ddb69b18), // matrix_y_256
        UINT64_C(0xa9e3f67bf8eb4e11), // matrix_y_257
        UINT64_C(0x23217a5fc6f604c2), // column_bits
    };
    return hashes[kind];
}

// === Прогон одного случая ===

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void RunCaseWithScratch(FillKind kind, int64_t chunkX, int64_t chunkY, int64_t chunkZ,
                               bool rebase, ChunkMesherScratch *scratch)
{
    World *world = WorldCreate(NULL);
    MesherExpect(world != NULL, "мир создан");

    int64_t baseX = chunkX * CHUNK_SIZE;
    int64_t baseY = chunkY * CHUNK_SIZE;
    int64_t baseZ = chunkZ * CHUNK_SIZE;
    ApplyFill(world, kind, baseX, baseY, baseZ);

    if (rebase)
    {
        // Перенос начала мира на целое число чанков: локальные координаты
        // уезжают, геометрия обязана остаться той же.
        MesherExpect(WorldRebase(world, CHUNK_SIZE, -CHUNK_SIZE, CHUNK_SIZE), "перенос мира");
        baseX -= CHUNK_SIZE;
        baseY += CHUNK_SIZE;
        baseZ -= CHUNK_SIZE;
        chunkX -= 1;
        chunkY += 1;
        chunkZ -= 1;
    }

    uint32_t expectedFaces = BuildExpected(world, baseX, baseY, baseZ);

    ChunkQuad *quads = NULL;
    uint32_t quadCount = 0u;
    MesherExpect(BuildChunkMesh(world, scratch, chunkX, chunkY, chunkZ, &quads, &quadCount),
                 "мешинг завершился успехом");
    MesherExpect(quadCount == 0u || quads != NULL, "непустая выдача обязана иметь массив");
    MesherExpect(quadCount != 0u || quads == NULL, "пустая выдача обязана быть без массива");

    ConsumeQuads(quads, quadCount, FillName(kind));
    CheckAllConsumed();

    uint64_t hash = HashQuads(quads, quadCount);

    // Повторный мешинг того же чанка обязан дать тот же порядок квадов.
    ChunkQuad *again = NULL;
    uint32_t againCount = 0u;
    MesherExpect(BuildChunkMesh(world, scratch, chunkX, chunkY, chunkZ, &again, &againCount),
                 "повторный мешинг завершился успехом");
    MesherExpect(againCount == quadCount, "повторный мешинг дал другое число квадов");
    MesherExpect(HashQuads(again, againCount) == hash, "повторный мешинг изменил порядок квадов");

    MesherWrite("mesh ");
    MesherWrite(FillName(kind));
    MesherWrite(rebase ? " rebased quads=" : " quads=");
    MesherWriteUnsigned(quadCount);
    MesherWrite(" faces=");
    MesherWriteUnsigned(expectedFaces);
    MesherWrite(" hash=");
    MesherWriteHex(hash);
    MesherWrite("\n");
    MesherExpect(hash == MesherGoldenHash(kind), "порядок или разбиение квадов изменились");

    if (again != NULL)
    {
        PlatformFree(again);
    }
    if (quads != NULL)
    {
        PlatformFree(quads);
    }
    WorldDestroy(world);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void RunCase(FillKind kind, int64_t chunkX, int64_t chunkY, int64_t chunkZ, bool rebase)
{
    ChunkMesherScratch *scratch = ChunkMesherScratchCreate();
    MesherExpect(scratch != NULL, "рабочие буферы мешера созданы");
    RunCaseWithScratch(kind, chunkX, chunkY, chunkZ, rebase, scratch);
    ChunkMesherScratchDestroy(scratch);
}

static void TestScratchDensityTransitions(void)
{
    ChunkMesherScratch *scratch = ChunkMesherScratchCreate();
    MesherExpect(scratch != NULL, "буферы для переключения плотности созданы");
    static const FillKind order[] = {
        FILL_DENSITY_4097, FILL_SINGLE_CENTER, FILL_CORE_EMPTY_HALO, FILL_DENSITY_257,
        FILL_DENSITY_255,  FILL_DENSITY_129,   FILL_DENSITY_127,     FILL_DENSITY_4096,
        FILL_DENSITY_4095, FILL_MATRIX_X_257,  FILL_MATRIX_X_255,    FILL_MATRIX_Y_257,
        FILL_MATRIX_Y_255,
    };
    for (uint32_t index = 0u; index < sizeof(order) / sizeof(order[0]); ++index)
    {
        // Один scratch переживает плотный и разреженный путь, затем ранний
        // выход для пустого ядра: старые маски не должны давать лишних граней.
        RunCaseWithScratch(order[index], 0, 0, 0, false, scratch);
    }
    ChunkMesherScratchDestroy(scratch);
}

// Пустой и сплошной чанк должны возвращать пустую выдачу, а не мусор: мешер
// выходит на них раньше, ещё до построения масок.
static void TestUniformShortcuts(void)
{
    World *world = WorldCreate(NULL);
    ChunkMesherScratch *scratch = ChunkMesherScratchCreate();
    MesherExpect(world != NULL && scratch != NULL, "мир и буферы для однородных случаев");

    ChunkQuad *quads = (ChunkQuad *)(void *)&mesherChecks;
    uint32_t quadCount = 12345u;
    MesherExpect(BuildChunkMesh(world, scratch, 0, 0, 0, &quads, &quadCount), "пустой чанк");
    MesherExpect(quads == NULL && quadCount == 0u, "пустой чанк обязан обнулить выход");

    ApplyFill(world, FILL_SOLID_CLOSED, 0, 0, 0);
    quads = (ChunkQuad *)(void *)&mesherChecks;
    quadCount = 12345u;
    MesherExpect(BuildChunkMesh(world, scratch, 0, 0, 0, &quads, &quadCount), "сплошной чанк");
    MesherExpect(quads == NULL && quadCount == 0u, "сплошной чанк обязан обнулить выход");

    ChunkMesherScratchDestroy(scratch);
    WorldDestroy(world);
}

LAIUE_TEST_ENTRY(ChunkMesherTestEntryPoint)
{
    ChunkMesherScratchDestroy(NULL);
    TestUniformShortcuts();

    for (uint32_t kind = 0u; kind < (uint32_t)FILL_KIND_COUNT; ++kind)
    {
        RunCase((FillKind)kind, 0, 0, 0, false);
    }
    // Чанк не в начале координат, в том числе с отрицательными индексами.
    RunCase(FILL_MATERIALS, 3, -2, 1, false);
    RunCase(FILL_HALO, -1, -1, -1, false);
    RunCase(FILL_CHECKER, 5, 5, -5, false);
    RunCase(FILL_MATERIAL_BANDS, -3, 2, -1, false);
    RunCase(FILL_BAND_GAPS, 2, -4, 3, false);
    RunCase(FILL_HIGH_MATERIAL_BANDS, -3, 2, -1, false);
    RunCase(FILL_DENSITY_4095, -3, 2, -1, false);
    RunCase(FILL_DENSITY_4096, -3, 2, -1, false);
    RunCase(FILL_DENSITY_4097, -3, 2, -1, false);
    for (uint32_t kind = (uint32_t)FILL_DENSITY_127; kind <= (uint32_t)FILL_DENSITY_257; ++kind)
    {
        RunCase((FillKind)kind, -3, 2, -1, false);
    }
    // И то же самое после переноса начала мира.
    RunCase(FILL_MATERIALS, 3, -2, 1, true);
    RunCase(FILL_HALO, 0, 0, 0, true);
    RunCase(FILL_EDGE_LINES, -2, 1, 2, true);
    RunCase(FILL_BAND_GAPS, 0, 0, 0, true);
    RunCase(FILL_HIGH_MATERIAL_BANDS, 2, -4, 3, true);
    RunCase(FILL_DENSITY_4095, 2, -4, 3, true);
    RunCase(FILL_DENSITY_4096, 2, -4, 3, true);
    RunCase(FILL_DENSITY_4097, 2, -4, 3, true);
    for (uint32_t kind = (uint32_t)FILL_DENSITY_127; kind <= (uint32_t)FILL_DENSITY_257; ++kind)
    {
        RunCase((FillKind)kind, 2, -4, 3, true);
    }
    TestScratchDensityTransitions();
    for (uint32_t kind = (uint32_t)FILL_MATRIX_X_255; kind <= (uint32_t)FILL_MATRIX_Y_257; ++kind)
    {
        RunCase((FillKind)kind, -3, 2, -1, false);
        RunCase((FillKind)kind, 2, -4, 3, true);
    }
    // Нерегулярные Z-колонны отдельно: тот же узор, но в чанке со сдвигом и
    // после переноса начала мира — транспонирование не должно зависеть от места.
    RunCase(FILL_COLUMN_BITS, -3, 2, -1, false);
    RunCase(FILL_COLUMN_BITS, 2, -4, 3, true);

    MesherWrite("chunk-mesher checks=");
    MesherWriteUnsigned(mesherChecks);
    MesherWrite(" failures=");
    MesherWriteUnsigned(mesherFailures);
    MesherWrite("\n");
    if (mesherFailures != 0u)
    {
        LaiueTestRuntimeExit(1);
    }
    LAIUE_TEST_SUCCESS();
}
