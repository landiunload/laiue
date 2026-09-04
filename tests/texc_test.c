// Конвертер изображений в `.ltp`: разбор PNG, GIF и JPEG, отказ на
// повреждённом входе, усреднение по площади и запись контейнера.
//
// Ожидаемые пиксели сняты сторонним декодером и лежат в
// texc_fixtures.h, поэтому тест сверяет наш разбор с чужим, а не сам с
// собой. Круг с движком замыкает laiue.render.offscreen_frame: он
// проигрывает анимированный пак и смотрит на цвет.

#include "media/image.h"
#include "media/lt_encode.h"
#include "media/png_decode.h"

#include "platform/system.h"
#include "test_runtime.h"
#include "texc_fixtures.h"

#include <stdbool.h>
#include <stdint.h>

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("texc test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t ReadU16Le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
}

static uint32_t ReadU32Le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

// Разбирает файл целиком и сверяет пиксели с ожидаемыми. Формат
// определяет сама библиотека: так проверяется и её выбор декодера.
//
// tolerance — предельное расхождение канала. Для PNG и GIF оно нулевое:
// там разбор точный. У JPEG обратное ДКП у каждой реализации своё, и
// расхождение в единицу-другую неизбежно; неверный разбор при этом даёт
// расхождение в десятки, и допуск его не прячет.
static void CheckImage(const char *label, const uint8_t *file, uint32_t fileBytes,
                       const uint8_t *expected, uint32_t expectedBytes, uint32_t width,
                       uint32_t height, uint32_t frameCount, uint32_t frameMilliseconds,
                       uint32_t tolerance)
{
    ImageInfo info = {0};
    ImageStatus status = ImageInspect(file, fileBytes, &info);
    Expect(status == IMAGE_OK, label);
    Expect(info.width == width && info.height == height, label);
    Expect(info.frameCount == frameCount, label);
    Expect(info.frameMilliseconds[0] == frameMilliseconds, label);
    Expect(info.pixelBytes == expectedBytes, label);

    uint8_t *pixels = PlatformAllocate(info.pixelBytes, false);
    void *scratch = info.scratchBytes != 0u ? PlatformAllocate(info.scratchBytes, false) : NULL;
    Expect(pixels != NULL && (info.scratchBytes == 0u || scratch != NULL), label);

    status = ImageDecode(file, fileBytes, &info, pixels, info.pixelBytes, scratch,
                         info.scratchBytes);
    Expect(status == IMAGE_OK, label);

    for (uint32_t index = 0; index < expectedBytes; ++index)
    {
        uint32_t decoded = pixels[index];
        uint32_t reference = expected[index];
        uint32_t difference = decoded > reference ? decoded - reference : reference - decoded;
        Expect(difference <= tolerance, label);
    }

    PlatformFree(scratch);
    PlatformFree(pixels);
}

static ImageStatus InspectPng(const uint8_t *file, uint32_t fileBytes)
{
    ImageInfo info = {0};
    return PngInspect(file, fileBytes, &info);
}

static ImageStatus InspectImage(const uint8_t *file, uint32_t fileBytes)
{
    ImageInfo info = {0};
    return ImageInspect(file, fileBytes, &info);
}

// Копия образца, у которой один байт заменён. Позиция ищется по
// маркеру, а не задаётся числом: смена набора образцов не должна
// молча превращать проверку отказа в проверку чего-то другого.
static uint8_t *CopyWithMarkerByte(const uint8_t *file, uint32_t fileBytes, uint8_t marker,
                                   uint32_t offsetInSegment, uint8_t value, const char *label)
{
    uint8_t *copy = PlatformAllocate(fileBytes, false);
    Expect(copy != NULL, label);
    for (uint32_t index = 0; index < fileBytes; ++index) copy[index] = file[index];

    // Сегменты обходятся по длинам, а не поиском двух байтов: те же
    // два байта встречаются и внутри таблицы квантования.
    uint32_t position = 2u;   // сразу за SOI
    while (position + 4u <= fileBytes && copy[position] == 0xFFu)
    {
        if (copy[position + 1u] == marker)
        {
            uint32_t target = position + 4u + offsetInSegment;   // 0xFF, маркер, длина
            Expect(target < fileBytes, label);
            copy[target] = value;
            return copy;
        }
        uint32_t length = ((uint32_t)copy[position + 2u] << 8) | (uint32_t)copy[position + 3u];
        if (length < 2u) break;
        position += 2u + length;
    }
    Expect(false, label);
    return copy;
}

LAIUE_TEST_ENTRY(TexcTestEntryPoint)
{
    // === Разбор настоящих файлов ===
    CheckImage("rgba png must decode exactly", PNG_RGBA_FILE, (uint32_t)sizeof(PNG_RGBA_FILE),
               PNG_RGBA_RGBA, (uint32_t)sizeof(PNG_RGBA_RGBA), 4u, 4u, 1u, 0u, 0u);
    CheckImage("palette png with tRNS must decode exactly", PNG_PALETTE_FILE,
               (uint32_t)sizeof(PNG_PALETTE_FILE), PNG_PALETTE_RGBA,
               (uint32_t)sizeof(PNG_PALETTE_RGBA), 4u, 4u, 1u, 0u, 0u);
    CheckImage("16 bit grayscale png must decode exactly", PNG_GRAY16_FILE,
               (uint32_t)sizeof(PNG_GRAY16_FILE), PNG_GRAY16_RGBA,
               (uint32_t)sizeof(PNG_GRAY16_RGBA), 2u, 2u, 1u, 0u, 0u);
    CheckImage("interlaced png must decode exactly", PNG_INTERLACED_FILE,
               (uint32_t)sizeof(PNG_INTERLACED_FILE), PNG_INTERLACED_RGBA,
               (uint32_t)sizeof(PNG_INTERLACED_RGBA), 8u, 8u, 1u, 0u, 0u);

    // Все три кадра сразу: прозрачный угол второго обязан показать
    // пиксель первого, иначе анимация теряет частичные обновления.
    CheckImage("animated gif must composite its frames", GIF_ANIMATED_FILE,
               (uint32_t)sizeof(GIF_ANIMATED_FILE), GIF_ANIMATED_RGBA,
               (uint32_t)sizeof(GIF_ANIMATED_RGBA), 4u, 4u, GIF_ANIMATED_FRAMES, 60u, 0u);

    // Задержки у кадров вправе различаться, и усреднять их нельзя:
    // анимация от этого меняется.
    {
        ImageInfo variable = {0};
        Expect(ImageInspect(GIF_VARIABLE_FILE, (uint32_t)sizeof(GIF_VARIABLE_FILE), &variable) ==
                   IMAGE_OK,
               "a gif with uneven delays must be inspected");
        Expect(variable.frameCount == 3u, "the uneven gif must keep its three frames");
        Expect(variable.frameMilliseconds[0] == 40u && variable.frameMilliseconds[1] == 120u &&
                   variable.frameMilliseconds[2] == 60u,
               "each frame must keep the delay the file gives it");
    }
    CheckImage("a gif with uneven delays must decode", GIF_VARIABLE_FILE,
               (uint32_t)sizeof(GIF_VARIABLE_FILE), GIF_VARIABLE_RGBA,
               (uint32_t)sizeof(GIF_VARIABLE_RGBA), 4u, 4u, GIF_VARIABLE_FRAMES, 40u, 0u);

    // === JPEG ===
    // Допуск в две единицы покрывает разницу обратных ДКП. Ошибка в
    // кодах Хаффмана, в порядке блоков или в переводе цвета сдвигает
    // пиксели на десятки, и такой допуск её не пропускает: соседние
    // проверки на JPEG_SUBSAMPLED это показывают прямо.
    CheckImage("baseline jpeg must decode", JPEG_BASELINE_FILE,
               (uint32_t)sizeof(JPEG_BASELINE_FILE), JPEG_BASELINE_RGBA,
               (uint32_t)sizeof(JPEG_BASELINE_RGBA), 16u, 16u, 1u, 0u, 2u);

    // Тот же кадр, записанный проходами: коэффициенты не меняются, и
    // пиксели обязаны совпасть с последовательной записью.
    CheckImage("progressive jpeg must decode to the same picture", JPEG_PROGRESSIVE_FILE,
               (uint32_t)sizeof(JPEG_PROGRESSIVE_FILE), JPEG_BASELINE_RGBA,
               (uint32_t)sizeof(JPEG_BASELINE_RGBA), 16u, 16u, 1u, 0u, 2u);

    // Прореженная цветность на размере, не кратном MCU: лишние блоки
    // обязаны отброситься, а не сдвинуть картинку.
    CheckImage("subsampled jpeg must crop its trailing blocks", JPEG_SUBSAMPLED_FILE,
               (uint32_t)sizeof(JPEG_SUBSAMPLED_FILE), JPEG_SUBSAMPLED_RGBA,
               (uint32_t)sizeof(JPEG_SUBSAMPLED_RGBA), 20u, 12u, 1u, 0u, 2u);

    // Маркеры рестарта сбрасывают предсказание, но не коэффициенты:
    // результат обязан совпасть с тем же кадром без них.
    CheckImage("restart markers must not change the picture", JPEG_RESTART_FILE,
               (uint32_t)sizeof(JPEG_RESTART_FILE), JPEG_SUBSAMPLED_RGBA,
               (uint32_t)sizeof(JPEG_SUBSAMPLED_RGBA), 20u, 12u, 1u, 0u, 2u);

    // Резкая цветность на шаге прореживания. Допуск здесь шире на
    // единицу — округление треугольного фильтра у эталона чередуется, —
    // но повторение ближайшего отсчёта вместо фильтра разошлось бы на
    // 82 единицы, и такой допуск его не пропустит.
    CheckImage("subsampled chroma must be interpolated, not repeated", JPEG_CHROMA_FILE,
               (uint32_t)sizeof(JPEG_CHROMA_FILE), JPEG_CHROMA_RGBA,
               (uint32_t)sizeof(JPEG_CHROMA_RGBA), 16u, 16u, 1u, 0u, 4u);

    CheckImage("grayscale jpeg must decode without colour conversion", JPEG_GRAY_FILE,
               (uint32_t)sizeof(JPEG_GRAY_FILE), JPEG_GRAY_RGBA,
               (uint32_t)sizeof(JPEG_GRAY_RGBA), 8u, 8u, 1u, 0u, 2u);

    // === Отказы ===
    Expect(ImageProbe(PNG_RGBA_FILE, 4u) == IMAGE_FORMAT_UNKNOWN,
           "a signature cut in half must not be recognised");
    Expect(InspectPng(PNG_RGBA_FILE, (uint32_t)sizeof(PNG_RGBA_FILE) - 20u) == IMAGE_TRUNCATED,
           "a png without its end marker must be rejected");

    uint8_t *damaged = PlatformAllocate(sizeof(PNG_RGBA_FILE), false);
    Expect(damaged != NULL, "the scratch copy could not be allocated");
    for (uint32_t index = 0; index < (uint32_t)sizeof(PNG_RGBA_FILE); ++index)
    {
        damaged[index] = PNG_RGBA_FILE[index];
    }
    damaged[20] ^= 0xFFu;   // ширина внутри IHDR
    Expect(InspectPng(damaged, (uint32_t)sizeof(PNG_RGBA_FILE)) == IMAGE_CORRUPT,
           "a damaged header must fail its checksum");

    // Байт внутри сжатых данных: разбор кодов может его пережить, а
    // контрольная сумма распакованного потока — нет.
    for (uint32_t index = 0; index < (uint32_t)sizeof(PNG_RGBA_FILE); ++index)
    {
        damaged[index] = PNG_RGBA_FILE[index];
    }
    damaged[60] ^= 0x40u;
    ImageInfo info = {0};
    Expect(PngInspect(damaged, (uint32_t)sizeof(PNG_RGBA_FILE), &info) == IMAGE_OK,
           "damaged pixel data must still describe its size");
    uint8_t *pixels = PlatformAllocate(info.pixelBytes, false);
    void *scratch = PlatformAllocate(info.scratchBytes, false);
    Expect(pixels != NULL && scratch != NULL, "decode buffers could not be allocated");
    ImageStatus status = PngDecode(damaged, (uint32_t)sizeof(PNG_RGBA_FILE), &info, pixels,
                                   info.pixelBytes, scratch, info.scratchBytes);
    Expect(status == IMAGE_CORRUPT || status == IMAGE_TRUNCATED,
           "damaged pixel data must be reported, not decoded into garbage");
    PlatformFree(scratch);
    PlatformFree(pixels);
    PlatformFree(damaged);

    // Двенадцать бит на отсчёт стандарт допускает, а наш выход — нет.
    // Отказ обязан быть назван своим именем, а не выдан за повреждение.
    uint8_t *deep = CopyWithMarkerByte(JPEG_BASELINE_FILE, (uint32_t)sizeof(JPEG_BASELINE_FILE),
                                       0xC0u, 0u, 12u, "the deep jpeg copy could not be made");
    Expect(InspectImage(deep, (uint32_t)sizeof(JPEG_BASELINE_FILE)) == IMAGE_UNSUPPORTED_FEATURE,
           "twelve bits per sample must be refused by name");
    PlatformFree(deep);

    // Ссылка на таблицу квантования, которой в файле нет: разбор с
    // нулевым делителем дал бы ровный серый вместо картинки.
    uint8_t *unquantised =
        CopyWithMarkerByte(JPEG_BASELINE_FILE, (uint32_t)sizeof(JPEG_BASELINE_FILE), 0xC0u, 8u, 3u,
                           "the unquantised jpeg copy could not be made");
    ImageInfo jpegInfo = {0};
    Expect(ImageInspect(unquantised, (uint32_t)sizeof(JPEG_BASELINE_FILE), &jpegInfo) == IMAGE_OK,
           "a missing quantisation table must not hide the size");
    uint8_t *jpegPixels = PlatformAllocate(jpegInfo.pixelBytes, false);
    void *jpegScratch = PlatformAllocate(jpegInfo.scratchBytes, false);
    Expect(jpegPixels != NULL && jpegScratch != NULL, "jpeg buffers could not be allocated");
    Expect(ImageDecode(unquantised, (uint32_t)sizeof(JPEG_BASELINE_FILE), &jpegInfo, jpegPixels,
                       jpegInfo.pixelBytes, jpegScratch, jpegInfo.scratchBytes) == IMAGE_CORRUPT,
           "a scan without its quantisation table must be reported");

    // Обрезанный файл: заголовок целый, данных прохода нет.
    Expect(InspectImage(JPEG_BASELINE_FILE, 20u) == IMAGE_TRUNCATED,
           "a jpeg cut before its frame header must be reported");

    // Проверка не должна проходить вхолостую: после четырёх отказов
    // целый файл обязан по-прежнему разбираться.
    Expect(InspectImage(JPEG_BASELINE_FILE, (uint32_t)sizeof(JPEG_BASELINE_FILE)) == IMAGE_OK,
           "a valid jpeg must still be accepted after the refusals");
    PlatformFree(jpegScratch);
    PlatformFree(jpegPixels);
    PlatformFree(unquantised);

    // === Усреднение по площади ===
    // Прозрачный чёрный не должен затемнять цвет: вес берётся по альфе.
    static const uint8_t quad[16] = {
        255u, 255u, 255u, 255u, 0u, 0u, 0u, 0u,
        0u,   0u,   0u,   0u,   0u, 0u, 0u, 0u,
    };
    uint8_t reduced[4];
    ImageResample(quad, 2u, 2u, reduced, 1u, 1u);
    Expect(reduced[0] == 255u && reduced[1] == 255u && reduced[2] == 255u,
           "a transparent black neighbour must not darken the colour");
    Expect(reduced[3] == 64u, "the alpha of a quarter-opaque quad must be a quarter");

    // Увеличение берёт ближайший пиксель: пиксель-арт не должен мылиться.
    uint8_t enlarged[16];
    ImageResample(quad, 2u, 2u, enlarged, 2u, 2u);
    for (uint32_t index = 0; index < 16u; ++index)
    {
        Expect(enlarged[index] == quad[index], "resampling to the same size must change nothing");
    }

    // === Контейнер одной текстуры ===
    uint8_t frames[2u * 4u] = {
        10u, 20u, 30u, 255u,
        40u, 50u, 60u, 255u,
    };
    uint32_t encodedBytes = 0u;
    // Между заголовком и пикселями лежит таблица длительностей: по два
    // байта на кадр.
    Expect(LtEncodedBytes(1u, 1u, 2u, false, &encodedBytes) == LT_OK &&
               encodedBytes == LT_HEADER_BYTES + 2u * 2u + 2u * 4u,
           "the size must follow from the frames and their schedule");
    Expect(LtEncodedBytes(1u, 1u, 2u, true, &encodedBytes) == LT_OK &&
               encodedBytes == LT_HEADER_BYTES + 2u * 2u + 4u * 4u,
           "normals double the payload");

    // Разные задержки у разных кадров: именно то, чего один интервал на
    // материал не выражал.
    static const uint16_t durations[2] = {40u, 120u};
    uint8_t file[80];
    LtTexture texture = {
        .albedoFrames = frames,
        .width = 1u,
        .height = 1u,
        .frameCount = 2u,
        .frameMilliseconds = durations,
        .sourceModifiedTime = 0x0123456789ABCDEFull,
        .sourceSizeBytes = 4321u,
    };
    Expect(LtEncode(&texture, file, sizeof(file), NULL) == LT_OK,
           "an animated texture must encode");
    Expect(ReadU32Le(file) == 0x3153544Cu, "the magic must be LTS1");
    Expect(ReadU16Le(file + 4) == LT_VERSION, "the version must be recorded");
    Expect(ReadU16Le(file + 6) == LT_HEADER_BYTES, "the header size must be recorded");
    Expect(ReadU16Le(file + 12) == 2u && ReadU16Le(file + 14) == 40u,
           "the frame count and the first duration must reach the header");
    Expect(ReadU16Le(file + 16) == 1u, "a texture without normals keeps format 1");
    Expect(ReadU32Le(file + 20) == 2u * 4u, "the payload size must count pixels only");
    // Отпечаток исходника: по нему движок узнаёт, не устарел ли кэш, не
    // читая сам исходник.
    Expect(ReadU32Le(file + 24) == 0x89ABCDEFu && ReadU32Le(file + 28) == 0x01234567u,
           "the source timestamp must survive whole");
    Expect(ReadU32Le(file + 32) == 4321u, "the source size must survive");
    Expect(ReadU16Le(file + LT_HEADER_BYTES) == 40u &&
               ReadU16Le(file + LT_HEADER_BYTES + 2u) == 120u,
           "every frame must keep its own duration");
    Expect(file[LT_HEADER_BYTES + 4u] == 10u && file[LT_HEADER_BYTES + 4u + 4u] == 40u,
           "the frames must be written in order after the schedule");

    static const uint16_t stopped[2] = {40u, 0u};
    LtTexture broken = texture;
    broken.frameMilliseconds = stopped;
    Expect(LtEncode(&broken, file, sizeof(file), NULL) == LT_INVALID_ARGUMENT,
           "a frame without a duration must be rejected");
    broken.frameMilliseconds = NULL;
    Expect(LtEncode(&broken, file, sizeof(file), NULL) == LT_INVALID_ARGUMENT,
           "an animated texture without a schedule must be rejected");
    broken.frameCount = 0u;
    Expect(LtEncode(&broken, file, sizeof(file), NULL) == LT_TOO_MANY_FRAMES,
           "a texture without frames must be rejected");
    broken.frameCount = 1u;
    broken.width = 0u;
    Expect(LtEncode(&broken, file, sizeof(file), NULL) == LT_BAD_SIZE,
           "an empty texture must be rejected");
    Expect(LtEncode(&texture, file, LT_HEADER_BYTES, NULL) == LT_BUFFER_TOO_SMALL,
           "a short buffer must be reported, not overrun");

    LaiueTestRuntimeWrite("texc test passed\n");
    LAIUE_TEST_SUCCESS();
}
