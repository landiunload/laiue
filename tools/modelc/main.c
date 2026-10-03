// modelc — офлайн-конвертер Wavefront OBJ в `.lo`. Он необязателен: OBJ в
// паке движок читает и сам и кладёт рядом готовый `.obj.lo`. Инструмент
// нужен там, где содержимое собирают заранее, например чтобы не возить
// исходники в сборку игры. Своего кода формата у него нет: разбор и запись
// лежат в общей `media_support`, которой пользуется движок.

#include "media/model.h"

#include "platform/system.h"

#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

typedef struct Message
{
    char text[256];
    uint32_t length;
} Message;

static void MessageReset(Message *message)
{
    message->length = 0u;
    message->text[0] = '\0';
}

static void MessageAppend(Message *message, const char *text)
{
    while (*text != '\0' && message->length + 1u < (uint32_t)sizeof(message->text))
    {
        message->text[message->length++] = *text++;
    }
    message->text[message->length] = '\0';
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
    message->text[message->length] = '\0';
}

static void Report(const char *text)
{
    Message message;
    MessageReset(&message);
    MessageAppend(&message, "modelc: ");
    MessageAppend(&message, text);
    MessageAppend(&message, "\n");
    PlatformWriteConsoleUtf8(message.text);
}

static void ReportUsage(void)
{
    PlatformWriteConsoleUtf8(
        "modelc converts a Wavefront OBJ model into the engine .lo format.\n"
        "\n"
        "  modelc [options] <input.obj> <output.lo>\n"
        "\n"
        "  --source-z-up  the OBJ already uses Z up (default: Y up, rotated to Z up)\n"
        "  --keep-v       keep the texture v coordinate (default: flipped to a top origin)\n"
        "  --help         show this text\n"
        "\n"
        "Polygons are split into triangles; faces without normals get smooth\n"
        "normals; usemtl names become material names, and groups or objects named\n"
        "COL_* or UCX_* become collision boxes instead of visible geometry.\n"
        "Put the result into models/<pack>.lop under the name the game asks for.\n");
}

static bool WideEquals(const wchar_t *left, const wchar_t *right)
{
    while (*left != L'\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

static int Convert(const wchar_t *inputPath, const wchar_t *outputPath, uint32_t importFlags)
{
    uint8_t *fileBytes = NULL;
    uint64_t fileSize = 0u;
    if (!PlatformReadEntireFile(inputPath, MODEL_MAX_FILE_BYTES, &fileBytes, &fileSize))
    {
        Report("the input file could not be read");
        return 1;
    }
    ModelInfo info;
    ModelStatus status = ModelInspect(fileBytes, (uint32_t)fileSize, &info);
    if (status == MODEL_OK && info.format != MODEL_FORMAT_OBJ)
        status = MODEL_NOT_RECOGNISED;
    if (status != MODEL_OK)
    {
        Report(ModelStatusText(status));
        PlatformFree(fileBytes);
        return 1;
    }

    ModelData model = {0};
    model.vertices = PlatformAllocate((size_t)info.vertexCapacity * sizeof(ModelVertex), false);
    model.indices = PlatformAllocate((size_t)info.indexCapacity * sizeof(uint32_t), false);
    model.parts = PlatformAllocate((size_t)info.partCapacity * sizeof(ModelPart), false);
    model.boxes = info.boxCapacity != 0u
                      ? PlatformAllocate((size_t)info.boxCapacity * sizeof(ModelBox), false)
                      : NULL;
    void *scratch = info.scratchBytes != 0u ? PlatformAllocate(info.scratchBytes, false) : NULL;
    uint8_t *encoded = NULL;
    int code = 1;
    if (model.vertices == NULL || model.indices == NULL || model.parts == NULL ||
        (info.boxCapacity != 0u && model.boxes == NULL) ||
        (info.scratchBytes != 0u && scratch == NULL))
    {
        Report("the model does not fit in memory");
        goto done;
    }
    const ModelImportOptions options = {importFlags};
    status = ModelDecode(fileBytes, (uint32_t)fileSize, &options, &info, &model, scratch,
                         info.scratchBytes);
    if (status != MODEL_OK)
    {
        Report(ModelStatusText(status));
        goto done;
    }
    // Отпечаток исходника остаётся нулевым: результат конвертера —
    // авторское содержимое, а не кэш, и движок его не пересобирает.
    uint32_t encodedBytes = 0u;
    status = ModelEncodedBytes(&model, &encodedBytes);
    if (status != MODEL_OK)
    {
        Report(ModelStatusText(status));
        goto done;
    }
    encoded = PlatformAllocate(encodedBytes, false);
    if (encoded == NULL || ModelEncode(&model, encoded, encodedBytes, NULL) != MODEL_OK)
    {
        Report("the encoded model does not fit in memory");
        goto done;
    }
    // Запись атомарна: прерванный конвертер не оставляет в паке полуфайл.
    if (!PlatformWriteFileAtomic(outputPath, encoded, encodedBytes))
    {
        Report("the output file could not be written");
        goto done;
    }
    Message message;
    MessageReset(&message);
    MessageAppend(&message, "modelc: ");
    MessageAppendNumber(&message, model.vertexCount);
    MessageAppend(&message, " vertices, ");
    MessageAppendNumber(&message, model.indexCount / 3u);
    MessageAppend(&message, " triangles, ");
    MessageAppendNumber(&message, model.partCount);
    MessageAppend(&message, " parts, ");
    MessageAppendNumber(&message, model.boxCount);
    MessageAppend(&message, " boxes -> ");
    MessageAppendNumber(&message, encodedBytes);
    MessageAppend(&message, " bytes\n");
    PlatformWriteConsoleUtf8(message.text);
    code = 0;

done:
    PlatformFree(encoded);
    PlatformFree(scratch);
    PlatformFree(model.boxes);
    PlatformFree(model.parts);
    PlatformFree(model.indices);
    PlatformFree(model.vertices);
    PlatformFree(fileBytes);
    return code;
}

static int Run(uint32_t argumentCount, const wchar_t *const *arguments)
{
    uint32_t importFlags = 0u;
    const wchar_t *inputPath = NULL;
    const wchar_t *outputPath = NULL;
    for (uint32_t index = 1; index < argumentCount; ++index)
    {
        const wchar_t *argument = arguments[index];
        if (WideEquals(argument, L"--help") || WideEquals(argument, L"-h"))
        {
            ReportUsage();
            return 0;
        }
        if (WideEquals(argument, L"--source-z-up"))
        {
            importFlags |= MODEL_IMPORT_SOURCE_Z_UP;
            continue;
        }
        if (WideEquals(argument, L"--keep-v"))
        {
            importFlags |= MODEL_IMPORT_KEEP_V;
            continue;
        }
        if (argument[0] == L'-' && argument[1] != L'\0')
        {
            Report("unknown option");
            return 2;
        }
        if (inputPath == NULL)
            inputPath = argument;
        else if (outputPath == NULL)
            outputPath = argument;
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
    return Convert(inputPath, outputPath, importFlags);
}

#if defined(_WIN32)

void ModelcEntryPoint(void)
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
    if (argc < 1)
        return 2;

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
        while (argv[index][length] != '\0')
            ++length;

        wchar_t *wide = PlatformAllocate(((size_t)length + 1u) * sizeof(wchar_t), true);
        if (wide == NULL || !PlatformUtf8ToWide(argv[index], length, wide, length + 1u, NULL))
        {
            PlatformFree(wide);
            converted = false;
            break;
        }
        arguments[index] = wide;
    }

    if (converted)
        code = Run((uint32_t)argc, arguments);
    else
        Report("an argument is not valid UTF-8");

    for (int index = 0; index < argc; ++index)
    {
        PlatformFree((void *)arguments[index]);
    }
    PlatformFree(arguments);
    return code;
}

#endif
