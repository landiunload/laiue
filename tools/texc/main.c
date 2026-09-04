// texc — офлайн-подготовка одной текстуры в формат движка `.lt`.
//
// Текстурпак `.ltp` — каталог, и класть в него можно прямо PNG или GIF:
// движок читает их сам. Этот инструмент нужен, когда загрузку хочется
// ускорить: `.lt` уже разобран, и на старте остаётся только скопировать
// пиксели. Анимация из GIF переносится вместе с длительностью кадра.

#include "media/lt_encode.h"

#include "media/image.h"
#include "platform/system.h"

#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#define TEXC_MAX_INPUT_BYTES 0x10000000u

typedef struct LoadedImage
{
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    uint16_t frameMilliseconds[IMAGE_MAX_FRAMES];
} LoadedImage;

typedef struct Message
{
    char text[256];
    uint32_t length;
} Message;

static void MessageReset(Message *message)
{
    message->length = 0u;
    message->text[0] = 0;
}

static void MessageAppend(Message *message, const char *text)
{
    while (*text != 0 && message->length + 1u < (uint32_t)sizeof(message->text))
    {
        message->text[message->length++] = *text++;
    }
    message->text[message->length] = 0;
}

static void MessageAppendNumber(Message *message, uint64_t value)
{
    char digits[20];
    uint32_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + (uint32_t)(value % 10u));
        value /= 10u;
    } while (value != 0u);

    while (count > 0u && message->length + 1u < (uint32_t)sizeof(message->text))
    {
        message->text[message->length++] = digits[--count];
    }
    message->text[message->length] = 0;
}

static void Report(const char *text)
{
    Message message;
    MessageReset(&message);
    MessageAppend(&message, "texc: ");
    MessageAppend(&message, text);
    MessageAppend(&message, "\n");
    PlatformWriteConsoleUtf8(message.text);
}

static void ReportUsage(void)
{
    PlatformWriteConsoleUtf8(
        "texc prepares one image as the .lt texture of the engine.\n"
        "\n"
        "  texc [options] <input.png|input.gif|input.jpg> <output.lt>\n"
        "\n"
        "  --size N            resample to N x N before writing\n"
        "  --normal <file>     add a normal and AO map to the same file\n"
        "  --help              show this text\n"
        "\n"
        "Reads PNG, GIF and JPEG. PNG covers every colour type, bit depth and\n"
        "Adam7; JPEG covers the sequential and the progressive mode. An animated\n"
        "GIF keeps its frames and its frame duration.\n"
        "\n"
        "A texture pack is a directory, and PNG, GIF or JPEG may live in it as\n"
        "they are: the engine reads them. Preparing .lt only skips decoding at\n"
        "load.\n");
}

static bool WideEquals(const wchar_t *left, const wchar_t *right)
{
    while (*left != 0 && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

static bool ParseNumber(const wchar_t *text, uint32_t *outValue)
{
    uint32_t value = 0u;
    if (*text == 0) return false;
    while (*text != 0)
    {
        if (*text < L'0' || *text > L'9') return false;
        if (value > 0xFFFFFFFFu / 10u) return false;
        value = value * 10u + (uint32_t)(*text - L'0');
        ++text;
    }
    *outValue = value;
    return true;
}

static ImageStatus DecodeFile(const wchar_t *path, LoadedImage *outImage)
{
    uint8_t *fileBytes = NULL;
    uint64_t fileSize = 0u;
    if (!PlatformReadEntireFile(path, TEXC_MAX_INPUT_BYTES, &fileBytes, &fileSize))
    {
        return IMAGE_TRUNCATED;
    }

    ImageInfo info = {0};
    ImageStatus status = ImageInspect(fileBytes, (uint32_t)fileSize, &info);
    if (status != IMAGE_OK)
    {
        PlatformFree(fileBytes);
        return status;
    }

    uint8_t *pixels = PlatformAllocate(info.pixelBytes, false);
    void *scratch = info.scratchBytes != 0u ? PlatformAllocate(info.scratchBytes, false) : NULL;
    if (pixels == NULL || (info.scratchBytes != 0u && scratch == NULL))
    {
        PlatformFree(pixels);
        PlatformFree(scratch);
        PlatformFree(fileBytes);
        return IMAGE_TOO_LARGE;
    }

    status = ImageDecode(fileBytes, (uint32_t)fileSize, &info, pixels, info.pixelBytes, scratch,
                         info.scratchBytes);
    PlatformFree(scratch);
    PlatformFree(fileBytes);
    if (status != IMAGE_OK)
    {
        PlatformFree(pixels);
        return status;
    }

    outImage->pixels = pixels;
    outImage->width = info.width;
    outImage->height = info.height;
    outImage->frameCount = info.frameCount;
    for (uint32_t frame = 0; frame < info.frameCount && frame < IMAGE_MAX_FRAMES; ++frame)
    {
        outImage->frameMilliseconds[frame] = info.frameMilliseconds[frame];
    }
    return IMAGE_OK;
}

// Приводит все кадры к квадрату заданной стороны. Если размер уже
// совпадает, буфер остаётся прежним и лишнего копирования нет.
static bool ResizeImage(LoadedImage *image, uint32_t width, uint32_t height)
{
    if (image->width == width && image->height == height) return true;

    uint32_t frameBytes = width * height * 4u;
    uint8_t *resized = PlatformAllocate((size_t)frameBytes * image->frameCount, false);
    if (resized == NULL) return false;

    uint32_t sourceFrameBytes = image->width * image->height * 4u;
    for (uint32_t frame = 0; frame < image->frameCount; ++frame)
    {
        ImageResample(image->pixels + (size_t)frame * sourceFrameBytes, image->width, image->height,
                      resized + (size_t)frame * frameBytes, width, height);
    }

    PlatformFree(image->pixels);
    image->pixels = resized;
    image->width = width;
    image->height = height;
    return true;
}

static int Convert(const wchar_t *inputPath, const wchar_t *normalPath, const wchar_t *outputPath,
                   uint32_t requestedSize)
{
    LoadedImage albedo = {0};
    ImageStatus status = DecodeFile(inputPath, &albedo);
    if (status != IMAGE_OK)
    {
        Report(ImageStatusText(status));
        return 1;
    }
    if (requestedSize != 0u && !ResizeImage(&albedo, requestedSize, requestedSize))
    {
        Report("the resized texture does not fit in memory");
        PlatformFree(albedo.pixels);
        return 1;
    }

    LoadedImage normal = {0};
    if (normalPath != NULL)
    {
        status = DecodeFile(normalPath, &normal);
        if (status != IMAGE_OK)
        {
            Report(ImageStatusText(status));
            PlatformFree(albedo.pixels);
            return 1;
        }
        // Карта нормалей приводится к геометрии albedo: у неё обязано
        // совпасть всё, кроме, может быть, исходного размера.
        if (!ResizeImage(&normal, albedo.width, albedo.height) ||
            normal.frameCount != albedo.frameCount)
        {
            Report("the normal map must match its albedo in frame count");
            PlatformFree(albedo.pixels);
            PlatformFree(normal.pixels);
            return 1;
        }
    }

    uint32_t encodedBytes = 0u;
    LtStatus ltStatus = LtEncodedBytes(albedo.width, albedo.height, albedo.frameCount,
                                       normalPath != NULL, &encodedBytes);
    if (ltStatus != LT_OK)
    {
        Report(LtStatusText(ltStatus));
        PlatformFree(albedo.pixels);
        PlatformFree(normal.pixels);
        return 1;
    }

    uint8_t *encoded = PlatformAllocate(encodedBytes, false);
    if (encoded == NULL)
    {
        Report("the encoded texture does not fit in memory");
        PlatformFree(albedo.pixels);
        PlatformFree(normal.pixels);
        return 1;
    }

    LtTexture texture = {
        .albedoFrames = albedo.pixels,
        .normalFrames = normalPath != NULL ? normal.pixels : NULL,
        .width = albedo.width,
        .height = albedo.height,
        .frameCount = albedo.frameCount,
        .frameMilliseconds = albedo.frameMilliseconds,
    };
    // Отпечаток исходника остаётся нулевым: результат инструмента —
    // авторское содержимое, а не кэш, и движок его не пересобирает.
    ltStatus = LtEncode(&texture, encoded, encodedBytes, NULL);
    PlatformFree(albedo.pixels);
    PlatformFree(normal.pixels);
    if (ltStatus != LT_OK)
    {
        Report(LtStatusText(ltStatus));
        PlatformFree(encoded);
        return 1;
    }

    // Запись атомарна: прерванный конвертер не оставляет в паке
    // полуфайл, который движок отверг бы как повреждённый.
    bool written = PlatformWriteFileAtomic(outputPath, encoded, encodedBytes);
    PlatformFree(encoded);
    if (!written)
    {
        Report("the output file could not be written");
        return 1;
    }

    Message message;
    MessageReset(&message);
    MessageAppend(&message, "texc: ");
    MessageAppendNumber(&message, albedo.width);
    MessageAppend(&message, "x");
    MessageAppendNumber(&message, albedo.height);
    MessageAppend(&message, ", ");
    MessageAppendNumber(&message, albedo.frameCount);
    MessageAppend(&message, albedo.frameCount == 1u ? " frame" : " frames");
    if (albedo.frameCount > 1u)
    {
        // Печатается длина цикла, а не длительность кадра: задержки
        // вправе различаться, и одно число про кадр было бы неправдой.
        uint32_t cycle = 0u;
        for (uint32_t frame = 0; frame < albedo.frameCount; ++frame)
        {
            cycle += albedo.frameMilliseconds[frame];
        }
        MessageAppend(&message, " over ");
        MessageAppendNumber(&message, cycle);
        MessageAppend(&message, " ms");
    }
    if (normalPath != NULL) MessageAppend(&message, ", with normals");
    MessageAppend(&message, ", ");
    MessageAppendNumber(&message, encodedBytes);
    MessageAppend(&message, " bytes\n");
    PlatformWriteConsoleUtf8(message.text);
    return 0;
}

static int Run(uint32_t argumentCount, const wchar_t *const *arguments)
{
    const wchar_t *inputPath = NULL;
    const wchar_t *outputPath = NULL;
    const wchar_t *normalPath = NULL;
    uint32_t requestedSize = 0u;

    for (uint32_t index = 1; index < argumentCount; ++index)
    {
        const wchar_t *argument = arguments[index];
        if (WideEquals(argument, L"--help") || WideEquals(argument, L"-h"))
        {
            ReportUsage();
            return 0;
        }
        if (WideEquals(argument, L"--size") || WideEquals(argument, L"--normal"))
        {
            bool wantsSize = WideEquals(argument, L"--size");
            if (index + 1u >= argumentCount)
            {
                Report("the option needs a value");
                return 2;
            }
            const wchar_t *value = arguments[++index];
            if (wantsSize)
            {
                if (!ParseNumber(value, &requestedSize) || requestedSize == 0u)
                {
                    Report("--size needs a positive number");
                    return 2;
                }
            }
            else
            {
                normalPath = value;
            }
            continue;
        }
        if (argument[0] == L'-' && argument[1] != 0)
        {
            Report("unknown option");
            return 2;
        }
        if (inputPath == NULL) inputPath = argument;
        else if (outputPath == NULL) outputPath = argument;
        else
        {
            Report("expected exactly one input and one output path");
            return 2;
        }
    }

    if (inputPath == NULL || outputPath == NULL)
    {
        ReportUsage();
        return 2;
    }
    return Convert(inputPath, normalPath, outputPath, requestedSize);
}

#if defined(_WIN32)

void TexcEntryPoint(void)
{
    int argumentCount = 0;
    // Разбором кавычек занимается система: собственный парсер отличался
    // бы от неё ровно в тех путях, которые пользователь и заключает в
    // кавычки.
    LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == NULL)
    {
        Report("the command line could not be read");
        ExitProcess(2u);
    }

    int code = Run((uint32_t)argumentCount, (const wchar_t *const *)arguments);
    LocalFree(arguments);
    ExitProcess((UINT)code);
}

#else

int main(int argc, char **argv)
{
    if (argc < 1) return 2;

    // POSIX отдаёт аргументы в UTF-8, а платформенный слой работает с
    // wchar_t: расширяются они здесь, у самой границы процесса.
    const wchar_t **arguments = PlatformAllocate((size_t)argc * sizeof(const wchar_t *), true);
    if (arguments == NULL)
    {
        Report("the arguments do not fit in memory");
        return 2;
    }

    int code = 2;
    bool converted = true;
    for (int index = 0; index < argc && converted; ++index)
    {
        uint32_t length = 0u;
        while (argv[index][length] != 0) ++length;

        wchar_t *wide = PlatformAllocate(((size_t)length + 1u) * sizeof(wchar_t), true);
        if (wide == NULL || !PlatformUtf8ToWide(argv[index], length, wide, length + 1u, NULL))
        {
            PlatformFree(wide);
            converted = false;
            break;
        }
        arguments[index] = wide;
    }

    if (converted) code = Run((uint32_t)argc, arguments);
    else Report("an argument is not valid UTF-8");

    for (int index = 0; index < argc; ++index)
    {
        PlatformFree((void *)arguments[index]);
    }
    PlatformFree(arguments);
    return code;
}

#endif
