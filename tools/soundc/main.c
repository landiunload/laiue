// soundc — офлайн-конвертер WAV в `.la`. Он необязателен: WAV в паке
// движок читает и сам, а `.la` лишь избавляет загрузку от разбора и
// вчетверо сокращает размер при ADPCM. Инструмент собирается тем же
// профилем, что и движок: без CRT на Windows и поверх платформенного
// слоя везде.

#include "media/la_encode.h"
#include "media/sound.h"

#include "platform/system.h"

#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

// Предел входного файла совпадает с пределом контейнера: больше всё
// равно не поместится в `.la`.
#define SOUNDC_MAX_INPUT_BYTES 0x20000000u

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
    MessageAppend(&message, "soundc: ");
    MessageAppend(&message, text);
    MessageAppend(&message, "\n");
    PlatformWriteConsoleUtf8(message.text);
}

static void ReportUsage(void)
{
    PlatformWriteConsoleUtf8(
        "soundc converts WAV into the .la sound container.\n"
        "\n"
        "  soundc [options] <input.wav> <output.la>\n"
        "\n"
        "  --pcm16   store samples losslessly (default)\n"
        "  --adpcm   store IMA ADPCM: four times smaller, slightly lossy\n"
        "  --help    show this text\n"
        "\n"
        "WAV input may be PCM 8, 16, 24 or 32 bit or IEEE float 32 or 64 bit,\n"
        "mono or stereo, including WAVE_FORMAT_EXTENSIBLE. MP3 input is\n"
        "MPEG-1 Layer III at 32000, 44100 or 48000 Hz; the encoder delay that\n"
        "LAME records is removed, so the sound starts where it should.\n"
        "Put the result into sounds/<pack>.lap under the name the game asks for.\n");
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

static int Convert(const wchar_t *inputPath, const wchar_t *outputPath, SoundEncoding encoding)
{
    uint8_t *fileBytes = NULL;
    uint64_t fileSize = 0u;
    if (!PlatformReadEntireFile(inputPath, SOUNDC_MAX_INPUT_BYTES, &fileBytes, &fileSize))
    {
        Report("the input file could not be read");
        return 1;
    }

    SoundInfo info;
    SoundStatus soundStatus = SoundInspect(fileBytes, (uint32_t)fileSize, &info);
    if (soundStatus != SOUND_OK)
    {
        Report(SoundStatusText(soundStatus));
        PlatformFree(fileBytes);
        return 1;
    }

    uint32_t sampleCount = info.sampleCount;
    int16_t *samples = PlatformAllocate((size_t)sampleCount * sizeof(int16_t), false);
    void *scratch = info.scratchBytes != 0u ? PlatformAllocate(info.scratchBytes, false) : NULL;
    if (samples == NULL || (info.scratchBytes != 0u && scratch == NULL))
    {
        Report("the decoded samples do not fit in memory");
        PlatformFree(samples);
        PlatformFree(scratch);
        PlatformFree(fileBytes);
        return 1;
    }

    soundStatus = SoundDecodeSamples(fileBytes, (uint32_t)fileSize, &info, samples, sampleCount,
                                     scratch, info.scratchBytes);
    PlatformFree(scratch);
    PlatformFree(fileBytes);
    if (soundStatus != SOUND_OK)
    {
        Report(SoundStatusText(soundStatus));
        PlatformFree(samples);
        return 1;
    }

    uint32_t encodedBytes = 0u;
    SoundStatus laStatus = SoundEncodedBytes(encoding, info.frameCount, info.channelCount, &encodedBytes);
    if (laStatus != SOUND_OK)
    {
        Report(SoundStatusText(laStatus));
        PlatformFree(samples);
        return 1;
    }

    uint8_t *encoded = PlatformAllocate(encodedBytes, false);
    if (encoded == NULL)
    {
        Report("the encoded sound does not fit in memory");
        PlatformFree(samples);
        return 1;
    }

    SoundClip clip = {
        .samples = samples,
        .frameCount = info.frameCount,
        .channelCount = info.channelCount,
        .sampleRate = info.sampleRate,
        .encoding = encoding,
    };
    // Отпечаток исходника остаётся нулевым: результат конвертера —
    // авторское содержимое, а не кэш, и движок его не пересобирает.
    laStatus = SoundEncode(&clip, encoded, encodedBytes, NULL);
    PlatformFree(samples);
    if (laStatus != SOUND_OK)
    {
        Report(SoundStatusText(laStatus));
        PlatformFree(encoded);
        return 1;
    }

    // Запись атомарна: прерванный конвертер не оставляет в паке
    // полуфайл, который движок примет за повреждённый звук.
    bool written = PlatformWriteFileAtomic(outputPath, encoded, encodedBytes);
    PlatformFree(encoded);
    if (!written)
    {
        Report("the output file could not be written");
        return 1;
    }

    uint64_t milliseconds = (uint64_t)info.frameCount * 1000u / info.sampleRate;
    Message message;
    MessageReset(&message);
    MessageAppend(&message, "soundc: ");
    MessageAppendNumber(&message, info.sampleRate);
    MessageAppend(&message, " Hz, ");
    MessageAppendNumber(&message, info.channelCount);
    MessageAppend(&message, " ch, ");
    MessageAppendNumber(&message, info.frameCount);
    MessageAppend(&message, " frames, ");
    MessageAppendNumber(&message, milliseconds / 1000u);
    MessageAppend(&message, ".");
    if (milliseconds % 1000u < 100u) MessageAppend(&message, "0");
    if (milliseconds % 1000u < 10u) MessageAppend(&message, "0");
    MessageAppendNumber(&message, milliseconds % 1000u);
    MessageAppend(&message, " s -> ");
    MessageAppend(&message, encoding == SOUND_ENCODING_ADPCM ? "ADPCM " : "PCM16 ");
    MessageAppendNumber(&message, encodedBytes);
    MessageAppend(&message, " bytes\n");
    PlatformWriteConsoleUtf8(message.text);
    return 0;
}

static int Run(uint32_t argumentCount, const wchar_t *const *arguments)
{
    SoundEncoding encoding = SOUND_ENCODING_PCM16;
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
        if (WideEquals(argument, L"--adpcm"))
        {
            encoding = SOUND_ENCODING_ADPCM;
            continue;
        }
        if (WideEquals(argument, L"--pcm16"))
        {
            encoding = SOUND_ENCODING_PCM16;
            continue;
        }
        if (argument[0] == L'-' && argument[1] != L'\0')
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
    return Convert(inputPath, outputPath, encoding);
}

#if defined(_WIN32)

void SoundcEntryPoint(void)
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
    const wchar_t **arguments =
        PlatformAllocate((size_t)argc * sizeof(const wchar_t *), true);
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
        while (argv[index][length] != '\0') ++length;

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
