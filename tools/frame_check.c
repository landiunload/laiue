/* Readback smoke utility: the engine's image decoder is the only PNG parser. */
#include "media/image.h"
#include "platform/system.h"
#include "walk_scenario.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#define FRAME_CHECK_MAX_FILE_BYTES (8u * 1024u * 1024u)
#define FRAME_CHECK_MAX_HEAP_BYTES (128u * 1024u * 1024u)

typedef struct FrameMessage
{
    char text[768];
    uint32_t length;
} FrameMessage;

static void Append(FrameMessage *message, const char *text)
{
    while (*text != '\0' && message->length + 1u < sizeof(message->text))
        message->text[message->length++] = *text++;
    message->text[message->length] = '\0';
}

static void Number(FrameMessage *message, uint64_t value)
{
    char digits[20];
    uint32_t length = 0u;
    do
    {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (length != 0u)
    {
        const char digit[] = {digits[--length], '\0'};
        Append(message, digit);
    }
}

static int Failure(const char *reason)
{
    FrameMessage message = {0};
    Append(&message, "{\"status\":\"FAIL\",\"error\":\"");
    Append(&message, reason);
    Append(&message, "\"}\n");
    PlatformWriteConsoleUtf8(message.text);
    return 1;
}

static int CheckFrame(const char *path)
{
    PlatformReadFile file = {0};
    uint64_t fileSize = 0u;
    if (!PlatformFileOpenRead(path, &file, &fileSize))
        return Failure("cannot_open_regular_file");
    if (fileSize == 0u || fileSize > FRAME_CHECK_MAX_FILE_BYTES)
    {
        PlatformFileClose(&file);
        return Failure("file_size_limit");
    }
    uint8_t *bytes = PlatformAllocate((size_t)fileSize, false);
    uint32_t read = 0u;
    const bool loaded = bytes != NULL &&
                        PlatformFileReadAt(&file, 0u, bytes, (uint32_t)fileSize, &read) &&
                        read == (uint32_t)fileSize;
    PlatformFileClose(&file);
    if (!loaded)
    {
        PlatformFree(bytes);
        return Failure("cannot_read_complete_file");
    }

    ImageInfo info = {0};
    ImageStatus status = ImageInspect(bytes, (uint32_t)fileSize, &info);
    if (status != IMAGE_OK || ImageProbe(bytes, (uint32_t)fileSize) != IMAGE_FORMAT_PNG)
    {
        PlatformFree(bytes);
        return Failure("invalid_or_unsupported_png");
    }
    /* The shared parser additionally applies IMAGE_MAX_DIMENSION (4096).
     * Reject before allocating even if that parser's limits grow later. */
    if (info.width < 3u || info.height < 4u || info.width > 8192u || info.height > 8192u ||
        info.frameCount != 1u)
    {
        PlatformFree(bytes);
        return Failure("decoded_image_size_limit");
    }
    const uint64_t expectedPixels = (uint64_t)info.width * info.height * 4u;
    const uint64_t decodeBytes = (uint64_t)info.pixelBytes + info.scratchBytes;
    if (expectedPixels != info.pixelBytes || expectedPixels != info.frameBytes ||
        fileSize + decodeBytes > FRAME_CHECK_MAX_HEAP_BYTES)
    {
        PlatformFree(bytes);
        return Failure("decoded_image_size_limit");
    }
    uint8_t *decoded = PlatformAllocate((size_t)decodeBytes, false);
    if (decoded == NULL)
    {
        PlatformFree(decoded);
        PlatformFree(bytes);
        return Failure("allocation_failed");
    }
    status =
        ImageDecode(bytes, (uint32_t)fileSize, &info, decoded, info.pixelBytes,
                    info.scratchBytes != 0u ? decoded + info.pixelBytes : NULL, info.scratchBytes);
    PlatformFree(bytes);
    if (status != IMAGE_OK)
    {
        PlatformFree(decoded);
        return Failure("png_decode_failed");
    }

    /* Exclude upper 40%, lower 25%, and the side touch controls. */
    const uint32_t left = info.width / 3u;
    const uint32_t right = info.width * 2u / 3u;
    const uint32_t top = info.height * 2u / 5u;
    const uint32_t bottom = info.height * 3u / 4u;
    const size_t pitch = (size_t)info.width * 4u;
    const size_t offset = (size_t)top * pitch + (size_t)left * 4u;
    WalkScenarioImage image;
    const bool inspected =
        WalkScenarioAnalyzeRGBA8(decoded + offset, (size_t)info.pixelBytes - offset, right - left,
                                 bottom - top, pitch, &image) != 0u;
    PlatformFree(decoded);
    if (!inspected)
        return Failure("invalid_viewport");
    const bool useful = image.useful != 0u;
    FrameMessage message = {0};
    Append(&message, useful ? "{\"status\":\"PASS\",\"useful\":true"
                            : "{\"status\":\"FAIL\",\"useful\":false");
    Append(&message, ",\"width\":");
    Number(&message, info.width);
    Append(&message, ",\"height\":");
    Number(&message, info.height);
    Append(&message, ",\"colors\":");
    Number(&message, image.quantizedColors);
    Append(&message, ",\"minimumLuminance\":");
    Number(&message, image.minimumLuminance);
    Append(&message, ",\"maximumLuminance\":");
    Number(&message, image.maximumLuminance);
    Append(&message, ",\"viewportPixelCount\":");
    Number(&message, image.pixelCount);
    Append(&message, ",\"majorityPixelCount\":");
    Number(&message, image.majorityPixelCount);
    Append(&message, ",\"viewportLeft\":");
    Number(&message, left);
    Append(&message, ",\"viewportTop\":");
    Number(&message, top);
    Append(&message, ",\"viewportWidth\":");
    Number(&message, right - left);
    Append(&message, ",\"viewportHeight\":");
    Number(&message, bottom - top);
    Append(&message, "}\n");
    PlatformWriteConsoleUtf8(message.text);
    return useful ? 0 : 1;
}

#if defined(_WIN32)
void FrameCheckEntryPoint(void)
{
    int argumentCount = 0;
    LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    int code = 1;
    char *path = NULL;
    if (arguments == NULL || argumentCount != 2)
        code = Failure("usage_laiue_frame_check_path_png");
    else
    {
        path = PlatformAllocate(LAIUE_PLATFORM_PATH_CAPACITY * 4u, false);
        if (path == NULL ||
            !PlatformWideToUtf8(arguments[1], path, LAIUE_PLATFORM_PATH_CAPACITY * 4u, NULL))
            code = Failure("invalid_or_too_long_unicode_path");
        else
            code = CheckFrame(path);
    }
    PlatformFree(path);
    if (arguments != NULL)
        LocalFree(arguments);
    ExitProcess((UINT)code);
}
#else
int main(int argc, char **argv)
{
    return argc == 2 ? CheckFrame(argv[1]) : Failure("usage_laiue_frame_check_path_png");
}
#endif
