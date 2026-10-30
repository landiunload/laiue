// Просмотрщик картинок на laiue: PNG, JPEG и GIF с анимацией, листание,
// масштаб колесом, клавишами и щипком, перетаскивание и поворот. Окно,
// ввод, графика и интерфейс — модули, которые программа грузит по имени,
// поэтому этот файл без изменений работает под Windows, Linux и Android.
//
//   laiue_viewer [каталог или файл] [--smoke]
//
// Клавиши: ←/→ — соседняя картинка, Home/End — первая и последняя,
// +/− и колесо — масштаб, 0 — вписать, 1 — один к одному, R — поворот,
// F — во весь экран, Esc — выход. На экране: касание у края листает,
// касание посередине прячет подпись, двойное касание переключает
// «вписать» и «один к одному», двумя пальцами картинку двигают и
// масштабируют.

#include "graphics/graphics_device_service.h"
#include "input/input_service.h"
#include "media/image.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "platform/window_service.h"
#include "ui/ui_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Сторона текстуры: всё крупнее уменьшается при загрузке. На телефоне
// предел ниже — там это ещё и экономия памяти.
#if defined(__ANDROID__)
#define MAX_SIDE 4096u
#else
#define MAX_SIDE 8192u
#endif
#define MAX_FILE_BYTES (UINT64_C(512) << 20)
#define MAX_FILES 65536u
#define DOUBLE_CLICK_SECONDS 0.35
#define CLICK_SLOP 12.0f
#define NAME_BYTES (LAIUE_PLATFORM_PATH_CAPACITY * 3u)
#define CAPTION_BYTES 768u

typedef struct Files
{
    char *text;        // имена в UTF-8 подряд, каждое с нулём в конце
    uint32_t *offsets; // начало каждого имени в text
    uint32_t count, capacity, textBytes, textCapacity;
} Files;

typedef struct Picture
{
    LaiueGraphicsHandle texture;
    uint32_t width, height;             // размер текстуры
    uint32_t sourceWidth, sourceHeight; // размер в файле
    uint8_t *frames;                    // кадры анимации; у неподвижной картинки NULL
    uint32_t frameCount, frame;
    double frameStart;
    uint16_t frameMilliseconds[IMAGE_MAX_FRAMES];
} Picture;

typedef struct Viewer
{
    LaiueModuleHost *host;
    const LaiueWindowServiceV1 *windows;
    uint32_t windowBytes;
    const LaiueInputServiceV1 *inputs;
    uint32_t inputBytes;
    const LaiueGraphicsDeviceServiceV2 *graphics;
    uint32_t graphicsBytes;
    const LaiueUiServiceV1 *ui;
    uint32_t uiBytes;
    Window *window;
    Input *input;
    LaiueGraphicsDeviceV2 *device;
    void *uiContext;
    LaiueGraphicsHandle vertices, indices, sampler;
    Picture picture;
    Files files;
    uint32_t current;
    wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    char caption[CAPTION_BYTES];
    // Вид: масштаб в пикселях экрана на пиксель картинки, точка повёрнутой
    // картинки в центре окна и поворот в четвертях оборота по часовой.
    float scale, centerX, centerY;
    uint32_t turns;
    bool fitted, shapeChanged, hideBar, pressed, smoke, failed;
    float pressX, pressY, travel, clickX, clickY;
    double lastTime, clickTime;
    int32_t width, height, atlasWidth, atlasHeight;
    uint32_t frames;
    LaiueUiQuadV1 quads[LAIUE_GRAPHICS_UI_MAX_QUADS];
} Viewer;

static bool Has(uint32_t actual, uint32_t declared, size_t offset, size_t bytes)
{
    return offset + bytes <= actual && offset + bytes <= declared;
}

static float Absolute(float value)
{
    return value < 0.0f ? -value : value;
}

static uint32_t Length(const char *text)
{
    uint32_t length = 0u;
    while (text[length] != '\0')
        ++length;
    return length;
}

static void Append(char *out, uint32_t capacity, const char *text)
{
    uint32_t length = Length(out);
    while (*text != '\0' && length + 1u < capacity)
        out[length++] = *text++;
    out[length] = '\0';
}

static void AppendNumber(char *out, uint32_t capacity, uint32_t value)
{
    char digits[11];
    uint32_t count = 0u;
    do
    {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    char text[12];
    for (uint32_t index = 0u; index < count; ++index)
        text[index] = digits[count - 1u - index];
    text[count] = '\0';
    Append(out, capacity, text);
}

static bool Equal(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

static char Lower(char character)
{
    return character >= 'A' && character <= 'Z' ? (char)(character - 'A' + 'a') : character;
}

static bool Digit(char character)
{
    return character >= '0' && character <= '9';
}

// Порядок, в котором файлы ждёт человек: «2» раньше «10», регистр
// латиницы не важен.
static int CompareNames(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (Digit(*a) && Digit(*b))
        {
            while (*a == '0')
                ++a;
            while (*b == '0')
                ++b;
            const char *numberA = a;
            const char *numberB = b;
            while (Digit(*a))
                ++a;
            while (Digit(*b))
                ++b;
            if (a - numberA != b - numberB)
                return a - numberA < b - numberB ? -1 : 1;
            for (; numberA != a; ++numberA, ++numberB)
                if (*numberA != *numberB)
                    return *numberA < *numberB ? -1 : 1;
            continue;
        }
        const unsigned char x = (unsigned char)Lower(*a++);
        const unsigned char y = (unsigned char)Lower(*b++);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return *a == *b ? 0 : *a == '\0' ? -1 : 1;
}

static void SiftDown(uint32_t *offsets, const char *text, uint32_t root, uint32_t count)
{
    for (;;)
    {
        uint32_t child = 2u * root + 1u;
        if (child >= count)
            return;
        if (child + 1u < count &&
            CompareNames(text + offsets[child], text + offsets[child + 1u]) < 0)
            ++child;
        if (CompareNames(text + offsets[root], text + offsets[child]) >= 0)
            return;
        const uint32_t swap = offsets[root];
        offsets[root] = offsets[child];
        offsets[child] = swap;
        root = child;
    }
}

static void SortNames(Files *files)
{
    for (uint32_t root = files->count / 2u; root-- > 0u;)
        SiftDown(files->offsets, files->text, root, files->count);
    for (uint32_t end = files->count; end-- > 1u;)
    {
        const uint32_t swap = files->offsets[0];
        files->offsets[0] = files->offsets[end];
        files->offsets[end] = swap;
        SiftDown(files->offsets, files->text, 0u, end);
    }
}

static bool IsPicture(const char *name)
{
    const char *dot = NULL;
    for (const char *at = name; *at != '\0'; ++at)
        if (*at == '.')
            dot = at;
    if (dot == NULL)
        return false;
    char extension[6] = {0};
    for (uint32_t index = 0u; index < 5u && dot[index + 1u] != '\0'; ++index)
        extension[index] = Lower(dot[index + 1u]);
    return Equal(extension, "png") || Equal(extension, "jpg") || Equal(extension, "jpeg") ||
           Equal(extension, "gif");
}

static bool AddName(Files *files, const char *name)
{
    const uint32_t bytes = Length(name) + 1u;
    if (files->count == MAX_FILES)
        return false;
    if (files->count == files->capacity)
    {
        const uint32_t capacity = files->capacity == 0u ? 64u : files->capacity * 2u;
        uint32_t *offsets = (uint32_t *)PlatformReallocate(
            files->offsets, (size_t)capacity * sizeof(*offsets), false);
        if (offsets == NULL)
            return false;
        files->offsets = offsets;
        files->capacity = capacity;
    }
    if (files->textBytes + bytes > files->textCapacity)
    {
        uint32_t capacity = files->textCapacity == 0u ? 4096u : files->textCapacity;
        while (files->textBytes + bytes > capacity)
            capacity *= 2u;
        char *text = (char *)PlatformReallocate(files->text, capacity, false);
        if (text == NULL)
            return false;
        files->text = text;
        files->textCapacity = capacity;
    }
    files->offsets[files->count++] = files->textBytes;
    memcpy(files->text + files->textBytes, name, bytes);
    files->textBytes += bytes;
    return true;
}

static void ListPictures(Viewer *viewer)
{
    PlatformDirectoryIterator iterator;
    if (!PlatformDirectoryOpen(&iterator, viewer->directory))
        return;
    static PlatformDirectoryEntry entry;
    static char name[NAME_BYTES];
    while (PlatformDirectoryNext(&iterator, &entry))
        if (!entry.isDirectory && PlatformWideToUtf8(entry.name, name, NAME_BYTES, NULL) &&
            IsPicture(name) && !AddName(&viewer->files, name))
            break;
    PlatformDirectoryClose(&iterator);
    SortNames(&viewer->files);
}

static const char *FileName(const Viewer *viewer, uint32_t index)
{
    return viewer->files.text + viewer->files.offsets[index];
}

static bool FilePath(const Viewer *viewer, uint32_t index, wchar_t *path)
{
    uint32_t length = 0u;
    while (viewer->directory[length] != L'\0')
        ++length;
    if (length + 2u >= LAIUE_PLATFORM_PATH_CAPACITY)
        return false;
    memcpy(path, viewer->directory, length * sizeof(wchar_t));
    path[length++] = L'/';
    const char *name = FileName(viewer, index);
    return PlatformUtf8ToWide(name, Length(name), path + length,
                              LAIUE_PLATFORM_PATH_CAPACITY - length, NULL);
}

// Стороны картинки на экране, с учётом поворота.
static void Turned(const Viewer *viewer, float *width, float *height)
{
    const bool sideways = (viewer->turns & 1u) != 0u;
    *width = (float)(sideways ? viewer->picture.sourceHeight : viewer->picture.sourceWidth);
    *height = (float)(sideways ? viewer->picture.sourceWidth : viewer->picture.sourceHeight);
}

static float ClampCenter(float center, float side, float half)
{
    if (side <= 2.0f * half)
        return side * 0.5f;
    return center < half ? half : center > side - half ? side - half : center;
}

// Картинка не уезжает с экрана: меньшая окна стоит по центру, большая
// закрывает его до края.
static void Clamp(Viewer *viewer)
{
    float width = 0.0f;
    float height = 0.0f;
    Turned(viewer, &width, &height);
    viewer->centerX =
        ClampCenter(viewer->centerX, width, (float)viewer->width * 0.5f / viewer->scale);
    viewer->centerY =
        ClampCenter(viewer->centerY, height, (float)viewer->height * 0.5f / viewer->scale);
}

// Вписывает картинку в окно, но не растягивает маленькую: мелкий рисунок
// честнее показать один к одному.
static void Fit(Viewer *viewer)
{
    float width = 0.0f;
    float height = 0.0f;
    Turned(viewer, &width, &height);
    viewer->scale = 1.0f;
    if (width > 0.0f && height > 0.0f && viewer->width > 0 && viewer->height > 0)
    {
        const float horizontal = (float)viewer->width / width;
        const float vertical = (float)viewer->height / height;
        const float fit = horizontal < vertical ? horizontal : vertical;
        viewer->scale = fit < 1.0f ? fit : 1.0f;
    }
    viewer->centerX = width * 0.5f;
    viewer->centerY = height * 0.5f;
    viewer->fitted = true;
}

// Меняет масштаб так, что точка картинки под (x, y) остаётся на месте.
static void Zoom(Viewer *viewer, float factor, float x, float y)
{
    float scale = viewer->scale * factor;
    scale = scale < 0.01f ? 0.01f : scale > 64.0f ? 64.0f : scale;
    const float offsetX = x - (float)viewer->width * 0.5f;
    const float offsetY = y - (float)viewer->height * 0.5f;
    viewer->centerX += offsetX / viewer->scale - offsetX / scale;
    viewer->centerY += offsetY / viewer->scale - offsetY / scale;
    viewer->scale = scale;
    viewer->fitted = false;
    Clamp(viewer);
}

static void Pan(Viewer *viewer, float x, float y)
{
    viewer->centerX -= x / viewer->scale;
    viewer->centerY -= y / viewer->scale;
    viewer->fitted = false;
    Clamp(viewer);
}

static void Turn(Viewer *viewer)
{
    float width = 0.0f;
    float height = 0.0f;
    Turned(viewer, &width, &height);
    const float x = viewer->centerX;
    viewer->centerX = height - viewer->centerY;
    viewer->centerY = x;
    viewer->turns = (viewer->turns + 1u) & 3u;
    viewer->shapeChanged = true;
    if (viewer->fitted)
        Fit(viewer);
    else
        Clamp(viewer);
}

static void ReleasePicture(Viewer *viewer)
{
    if (viewer->picture.texture != 0u && viewer->device != NULL)
        viewer->device->destroyHandle(viewer->device, viewer->picture.texture);
    PlatformFree(viewer->picture.frames);
    memset(&viewer->picture, 0, sizeof(viewer->picture));
}

// Текстура с mip-цепочкой: уменьшенная картинка без неё рябит. Уровни
// строятся по очереди в двух буферах: каждый следующий вчетверо меньше,
// и тот, из которого читают, никогда не совпадает с тем, куда пишут.
static bool Upload(Viewer *viewer, const uint8_t *pixels, uint32_t width, uint32_t height,
                   bool mipmaps)
{
    uint32_t levels = 1u;
    while (mipmaps && ((width | height) >> levels) != 0u)
        ++levels;
    const LaiueGraphicsTextureDescV1 description = {
        .structSize = sizeof(description),
        .format = LAIUE_GRAPHICS_FORMAT_RGBA8_SRGB,
        .extent = {width, height, 1u},
        .mipLevels = levels,
    };
    LaiueGraphicsHandle texture = 0u;
    if (!viewer->device->createTexture(viewer->device, &description, &texture))
        return false;
    const uint32_t width1 = width > 1u ? width / 2u : 1u;
    const uint32_t height1 = height > 1u ? height / 2u : 1u;
    const size_t bytes1 = (size_t)width1 * height1 * 4u;
    const size_t bytes2 =
        (size_t)(width1 > 1u ? width1 / 2u : 1u) * (height1 > 1u ? height1 / 2u : 1u) * 4u;
    uint8_t *chain = levels > 1u ? (uint8_t *)PlatformAllocate(bytes1 + bytes2, false) : NULL;
    bool uploaded = levels == 1u || chain != NULL;
    const uint8_t *level = pixels;
    uint32_t levelWidth = width;
    uint32_t levelHeight = height;
    for (uint32_t mip = 0u; uploaded && mip < levels; ++mip)
    {
        if (mip != 0u)
        {
            const uint32_t nextWidth = levelWidth > 1u ? levelWidth / 2u : 1u;
            const uint32_t nextHeight = levelHeight > 1u ? levelHeight / 2u : 1u;
            uint8_t *next = level == chain ? chain + bytes1 : chain;
            ImageResample(level, levelWidth, levelHeight, next, nextWidth, nextHeight);
            level = next;
            levelWidth = nextWidth;
            levelHeight = nextHeight;
        }
        const LaiueGraphicsTextureUploadV1 upload = {
            .structSize = sizeof(upload),
            .texture = texture,
            .data = level,
            .sizeBytes = (uint64_t)levelWidth * levelHeight * 4u,
            .mipLevel = mip,
        };
        uploaded = viewer->device->uploadTexture(viewer->device, &upload) != 0u;
    }
    PlatformFree(chain);
    if (!uploaded)
    {
        viewer->device->destroyHandle(viewer->device, texture);
        return false;
    }
    viewer->picture.texture = texture;
    viewer->picture.width = width;
    viewer->picture.height = height;
    return true;
}

static void SetCaption(Viewer *viewer, const char *text, const char *detail)
{
    viewer->caption[0] = '\0';
    Append(viewer->caption, sizeof(viewer->caption), text);
    Append(viewer->caption, sizeof(viewer->caption), detail);
}

// Читает картинку и отдаёт её видеокарте. Уменьшение до MAX_SIDE и
// mip-цепочка строятся здесь же; кадры GIF остаются в памяти для анимации.
static bool LoadPicture(Viewer *viewer, uint32_t index)
{
    ReleasePicture(viewer);
    viewer->current = index;
    viewer->turns = 0u;
    viewer->shapeChanged = true;
    static wchar_t path[LAIUE_PLATFORM_PATH_CAPACITY];
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    ImageStatus status = IMAGE_TRUNCATED;
    ImageInfo info = {0};
    uint8_t *pixels = NULL;
    if (FilePath(viewer, index, path) &&
        PlatformReadEntireFile(path, MAX_FILE_BYTES, &bytes, &size))
        status = ImageInspect(bytes, (uint32_t)size, &info);
    if (status == IMAGE_OK)
    {
        pixels = (uint8_t *)PlatformAllocate(info.pixelBytes, false);
        void *scratch = info.scratchBytes != 0u ? PlatformAllocate(info.scratchBytes, false) : NULL;
        status = pixels == NULL || (info.scratchBytes != 0u && scratch == NULL)
                     ? IMAGE_TOO_LARGE
                     : ImageDecode(bytes, (uint32_t)size, &info, pixels, info.pixelBytes, scratch,
                                   info.scratchBytes);
        PlatformFree(scratch);
    }
    PlatformFree(bytes);
    if (status != IMAGE_OK)
    {
        PlatformFree(pixels);
        SetCaption(viewer, FileName(viewer, index), " - ");
        Append(viewer->caption, sizeof(viewer->caption), ImageStatusText(status));
        return false;
    }

    uint32_t width = info.width;
    uint32_t height = info.height;
    const uint8_t *first = pixels;
    uint8_t *reduced = NULL;
    if (width > MAX_SIDE || height > MAX_SIDE)
    {
        const uint64_t longer = width > height ? width : height;
        width = (uint32_t)((uint64_t)width * MAX_SIDE / longer);
        height = (uint32_t)((uint64_t)height * MAX_SIDE / longer);
        width = width == 0u ? 1u : width;
        height = height == 0u ? 1u : height;
        reduced = (uint8_t *)PlatformAllocate((size_t)width * height * 4u, false);
        if (reduced != NULL)
            ImageResample(pixels, info.width, info.height, reduced, width, height);
        first = reduced;
    }
    // Анимация идёт кадрами из памяти: уменьшенный GIF показывает первый.
    const bool animated = info.frameCount > 1u && reduced == NULL;
    const bool uploaded = first != NULL && Upload(viewer, first, width, height, !animated);
    PlatformFree(reduced);
    if (uploaded && animated)
    {
        viewer->picture.frames = pixels;
        viewer->picture.frameCount = info.frameCount;
        viewer->picture.frameStart = PlatformMonotonicSeconds();
        memcpy(viewer->picture.frameMilliseconds, info.frameMilliseconds,
               sizeof(viewer->picture.frameMilliseconds));
    }
    else
    {
        PlatformFree(pixels);
    }
    if (!uploaded)
    {
        SetCaption(viewer, FileName(viewer, index), " - не хватает памяти");
        return false;
    }
    viewer->picture.sourceWidth = info.width;
    viewer->picture.sourceHeight = info.height;
    SetCaption(viewer, FileName(viewer, index), "");
    Fit(viewer);
    return true;
}

// Пустой каталог показывает узор: видно, что окно и графика живы, а
// подпись объясняет, куда положить картинки.
static bool ShowPattern(Viewer *viewer)
{
    enum
    {
        SIDE = 256
    };
    uint8_t *pixels = (uint8_t *)PlatformAllocate(SIDE * SIDE * 4u, false);
    if (pixels == NULL)
        return false;
    for (uint32_t y = 0u; y < SIDE; ++y)
        for (uint32_t x = 0u; x < SIDE; ++x)
        {
            uint8_t *pixel = pixels + 4u * (y * SIDE + x);
            pixel[0] = (uint8_t)x;
            pixel[1] = (uint8_t)y;
            pixel[2] = ((x / 32u) + (y / 32u)) % 2u == 0u ? 224u : 96u;
            pixel[3] = 255u;
        }
    ReleasePicture(viewer);
    const bool uploaded = Upload(viewer, pixels, SIDE, SIDE, true);
    PlatformFree(pixels);
    viewer->picture.sourceWidth = SIDE;
    viewer->picture.sourceHeight = SIDE;
    viewer->shapeChanged = true;
    static char directory[NAME_BYTES];
    if (!PlatformWideToUtf8(viewer->directory, directory, NAME_BYTES, NULL))
        directory[0] = '\0';
    SetCaption(viewer, "Нет картинок PNG, JPEG или GIF в ", directory);
    Fit(viewer);
    return uploaded;
}

static void Show(Viewer *viewer, uint32_t index)
{
    if (index < viewer->files.count && index != viewer->current)
        (void)LoadPicture(viewer, index);
}

// Кадры GIF сменяются по своим задержкам; кадр без задержки идёт, как в
// браузерах, через 100 мс.
static void Animate(Viewer *viewer, double now)
{
    Picture *picture = &viewer->picture;
    if (picture->frames == NULL)
        return;
    const uint32_t delay = picture->frameMilliseconds[picture->frame];
    if (now - picture->frameStart < (double)(delay < 20u ? 100u : delay) * 0.001)
        return;
    picture->frame = (picture->frame + 1u) % picture->frameCount;
    picture->frameStart = now;
    const size_t frameBytes = (size_t)picture->width * picture->height * 4u;
    const LaiueGraphicsTextureUploadV1 upload = {
        .structSize = sizeof(upload),
        .texture = picture->texture,
        .data = picture->frames + (size_t)picture->frame * frameBytes,
        .sizeBytes = frameBytes,
    };
    (void)viewer->device->uploadTexture(viewer->device, &upload);
}

static void HandleKeys(Viewer *viewer)
{
    const LaiueInputServiceV1 *keys = viewer->inputs;
    Input *input = viewer->input;
    const uint32_t last = viewer->files.count != 0u ? viewer->files.count - 1u : 0u;
    if (keys->consumeKeyPress(input, INPUT_KEY_ESCAPE))
        viewer->windows->requestClose(viewer->window);
    if (keys->consumeKeyPress(input, INPUT_KEY_RIGHT) ||
        keys->consumeKeyPress(input, INPUT_KEY_PAGE_DOWN) ||
        keys->consumeKeyPress(input, INPUT_KEY_SPACE))
        Show(viewer, viewer->current < last ? viewer->current + 1u : last);
    if (keys->consumeKeyPress(input, INPUT_KEY_LEFT) ||
        keys->consumeKeyPress(input, INPUT_KEY_PAGE_UP) ||
        keys->consumeKeyPress(input, INPUT_KEY_BACKSPACE))
        Show(viewer, viewer->current > 0u ? viewer->current - 1u : 0u);
    if (keys->consumeKeyPress(input, INPUT_KEY_HOME))
        Show(viewer, 0u);
    if (keys->consumeKeyPress(input, INPUT_KEY_END))
        Show(viewer, last);
    const float middleX = (float)viewer->width * 0.5f;
    const float middleY = (float)viewer->height * 0.5f;
    if (keys->consumeKeyPress(input, INPUT_KEY_EQUALS))
        Zoom(viewer, 1.25f, middleX, middleY);
    if (keys->consumeKeyPress(input, INPUT_KEY_MINUS))
        Zoom(viewer, 0.8f, middleX, middleY);
    if (keys->consumeKeyPress(input, INPUT_KEY_0))
        Fit(viewer);
    if (keys->consumeKeyPress(input, INPUT_KEY_1))
        Zoom(viewer, 1.0f / viewer->scale, middleX, middleY);
    if (keys->consumeKeyPress(input, INPUT_KEY_R))
        Turn(viewer);
    if (keys->consumeKeyPress(input, INPUT_KEY_F) || keys->consumeKeyPress(input, INPUT_KEY_F11))
        viewer->windows->setFullscreen(viewer->window,
                                       !viewer->windows->isFullscreen(viewer->window));
}

// Щелчок или касание без сдвига: двойное переключает масштаб, одиночное у
// края листает, посередине прячет подпись.
static void Click(Viewer *viewer, double now)
{
    const uint32_t last = viewer->files.count != 0u ? viewer->files.count - 1u : 0u;
    const float distance =
        Absolute(viewer->pressX - viewer->clickX) + Absolute(viewer->pressY - viewer->clickY);
    if (now - viewer->clickTime < DOUBLE_CLICK_SECONDS && distance < 4.0f * CLICK_SLOP)
    {
        if (viewer->fitted)
            Zoom(viewer, 1.0f / viewer->scale, viewer->pressX, viewer->pressY);
        else
            Fit(viewer);
        viewer->clickTime = 0.0;
        return;
    }
    viewer->clickTime = now;
    viewer->clickX = viewer->pressX;
    viewer->clickY = viewer->pressY;
    const float edge = (float)viewer->width * 0.2f;
    if (viewer->pressX < edge)
        Show(viewer, viewer->current > 0u ? viewer->current - 1u : 0u);
    else if (viewer->pressX > (float)viewer->width - edge)
        Show(viewer, viewer->current < last ? viewer->current + 1u : last);
    else
        viewer->hideBar = !viewer->hideBar;
}

static void HandlePointer(Viewer *viewer, double now)
{
    const LaiueInputServiceV1 *keys = viewer->inputs;
    Input *input = viewer->input;
    int32_t cursorX = 0;
    int32_t cursorY = 0;
    viewer->windows->getCursorClientPosition(viewer->window, &cursorX, &cursorY);
    // Каждый щелчок колеса — шаг в 1.25 раза; дробные шаги сенсорной
    // панели плавно делят его.
    float steps = viewer->windows->consumeMouseWheelSteps(viewer->window);
    float factor = 1.0f;
    for (; steps >= 1.0f; steps -= 1.0f)
        factor *= 1.25f;
    for (; steps <= -1.0f; steps += 1.0f)
        factor *= 0.8f;
    factor *= 1.0f + (steps > 0.0f ? 0.25f : 0.2f) * steps;
    if (factor != 1.0f)
        Zoom(viewer, factor, (float)cursorX, (float)cursorY);

    // Два пальца двигают и масштабируют, один тащит или касается.
    InputGesture gesture;
    if (Has(viewer->inputBytes, keys->structSize, offsetof(LaiueInputServiceV1, getGesture),
            sizeof(keys->getGesture)) &&
        keys->getGesture(input, &gesture))
    {
        Pan(viewer, gesture.panX, gesture.panY);
        Zoom(viewer, gesture.scale, gesture.centerX, gesture.centerY);
        viewer->pressed = false;
        return;
    }
    // Нажатие считается, только если оно в окне: щелчок по заголовку
    // двигает окно, а не листает. Нажатие и отпускание могут прийти за
    // один кадр — это тоже щелчок.
    const bool down = keys->isMouseButtonDown(input, INPUT_MOUSE_BUTTON_LEFT);
    if (!viewer->pressed && keys->wasMouseButtonPressed(input, INPUT_MOUSE_BUTTON_LEFT) &&
        cursorX >= 0 && cursorY >= 0 && cursorX < viewer->width && cursorY < viewer->height)
    {
        viewer->pressed = true;
        viewer->pressX = (float)cursorX;
        viewer->pressY = (float)cursorY;
        viewer->travel = 0.0f;
    }
    int32_t deltaX = 0;
    int32_t deltaY = 0;
    keys->getMouseDelta(input, &deltaX, &deltaY);
    if (viewer->pressed && down)
    {
        Pan(viewer, (float)deltaX, (float)deltaY);
        viewer->travel += Absolute((float)deltaX) + Absolute((float)deltaY);
    }
    if (viewer->pressed && !down)
    {
        viewer->pressed = false;
        if (viewer->travel <= CLICK_SLOP)
            Click(viewer, now);
    }
}

// Геометрия картинки — прямоугольник в её пикселях с углами текстуры,
// сдвинутыми на поворот. Она меняется только с картинкой или поворотом;
// масштаб и сдвиг каждый кадр задаёт сама отрисовка. Загрузка возможна
// только вне кадра.
static bool UpdateShape(Viewer *viewer)
{
    if (!viewer->shapeChanged || viewer->picture.texture == 0u)
        return true;
    float width = 0.0f;
    float height = 0.0f;
    Turned(viewer, &width, &height);
    const float corners[4][2] = {{0.0f, 0.0f}, {width, 0.0f}, {width, height}, {0.0f, height}};
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    LaiueGraphicsVertexV2 vertices[4];
    for (uint32_t corner = 0u; corner < 4u; ++corner)
    {
        const uint32_t source = (corner + 4u - viewer->turns) & 3u;
        vertices[corner] = (LaiueGraphicsVertexV2){
            .position = {corners[corner][0], corners[corner][1], 0.0f},
            .uv = {uv[source][0], uv[source][1]},
            .colorRGBA = 0xffffffffu,
        };
    }
    const LaiueGraphicsBufferUploadV1 upload = {.structSize = sizeof(upload),
                                                .buffer = viewer->vertices,
                                                .data = vertices,
                                                .sizeBytes = sizeof(vertices)};
    viewer->shapeChanged = viewer->device->uploadBuffer(viewer->device, &upload) == 0u;
    return !viewer->shapeChanged;
}

static bool DrawPicture(Viewer *viewer)
{
    if (viewer->picture.texture == 0u)
        return true;
    // Левый верхний угол — в целый пиксель: один к одному картинка резкая.
    const float left =
        (float)(int32_t)((float)viewer->width * 0.5f - viewer->centerX * viewer->scale + 0.5f);
    const float top =
        (float)(int32_t)((float)viewer->height * 0.5f - viewer->centerY * viewer->scale + 0.5f);
    const LaiueGraphicsDrawItemV2 draw = {.structSize = sizeof(draw),
                                          .vertexBuffer = viewer->vertices,
                                          .indexBuffer = viewer->indices,
                                          .indexCount = 6u,
                                          .originRelative = {left, top, 0.5f},
                                          .scale = viewer->scale,
                                          .texture = viewer->picture.texture,
                                          .sampler = viewer->sampler};
    return viewer->device->submit(viewer->device, &draw, 1u) != 0u;
}

// Подпись строится до кадра: шрифт перепекается под размер окна, и его
// атлас уходит в графику заново.
static bool PrepareBar(Viewer *viewer, float seconds)
{
    const LaiueUiServiceV1 *ui = viewer->ui;
    if (viewer->uiContext == NULL || viewer->hideBar)
        return true;
    if (!ui->begin(viewer->uiContext, viewer->width, viewer->height, 0.0f, 0.0f, 0u, 0u, 0.0f,
                   seconds))
        return false;
    if (viewer->atlasWidth != viewer->width || viewer->atlasHeight != viewer->height)
    {
        const uint8_t *atlas = NULL;
        uint32_t atlasWidth = 0u;
        uint32_t atlasHeight = 0u;
        if (!ui->getFontAtlas(viewer->uiContext, &atlas, &atlasWidth, &atlasHeight) ||
            !viewer->device->setUiFontAtlas(viewer->device, atlas, atlasWidth, atlasHeight))
            return false;
        viewer->atlasWidth = viewer->width;
        viewer->atlasHeight = viewer->height;
    }
    static char line[CAPTION_BYTES + 64u];
    line[0] = '\0';
    if (viewer->files.count != 0u)
    {
        AppendNumber(line, sizeof(line), viewer->current + 1u);
        Append(line, sizeof(line), "/");
        AppendNumber(line, sizeof(line), viewer->files.count);
        Append(line, sizeof(line), "   ");
    }
    Append(line, sizeof(line), viewer->caption);
    if (viewer->picture.texture != 0u)
    {
        Append(line, sizeof(line), "   ");
        AppendNumber(line, sizeof(line), viewer->picture.sourceWidth);
        Append(line, sizeof(line), " x ");
        AppendNumber(line, sizeof(line), viewer->picture.sourceHeight);
        Append(line, sizeof(line), "   ");
        AppendNumber(line, sizeof(line), (uint32_t)(viewer->scale * 100.0f + 0.5f));
        Append(line, sizeof(line), "%");
    }
    const float em = ui->textWidthUtf8(viewer->uiContext, "M");
    const float bar = em * 2.4f;
    const float top = (float)viewer->height - bar;
    ui->rect(viewer->uiContext, 0.0f, top, (float)viewer->width, bar, 0.0f, 0xb4141414u);
    ui->textUtf8(viewer->uiContext, em, top + em * 0.5f, 0xffe6e6e6u, line);
    return true;
}

static bool DrawBar(Viewer *viewer)
{
    if (viewer->uiContext == NULL || viewer->hideBar)
        return true;
    uint32_t count = 0u;
    (void)viewer->ui->copyDrawList(viewer->uiContext, viewer->quads, LAIUE_GRAPHICS_UI_MAX_QUADS,
                                   &count);
    return count == 0u || viewer->device->submitUi(viewer->device, viewer->quads, count) != 0u;
}

// Проверка для CI: в центре кадра картинка, а не цвет фона из угла.
static bool FrameShowsPicture(Viewer *viewer)
{
    if (viewer->device->structSize < LAIUE_GRAPHICS_DEVICE_V2_READBACK_SIZE ||
        viewer->device->readbackFrame == NULL)
        return true;
    const size_t bytes = (size_t)viewer->width * (size_t)viewer->height * 4u;
    uint8_t *pixels = (uint8_t *)PlatformAllocate(bytes, false);
    LaiueGraphicsFrameReadbackV2 readback = {
        .structSize = sizeof(readback), .pixels = pixels, .capacityBytes = bytes};
    bool shown = false;
    if (pixels != NULL && viewer->device->readbackFrame(viewer->device, &readback) &&
        readback.width != 0u && readback.height != 0u)
    {
        const uint32_t pitch =
            readback.rowPitchBytes != 0u ? readback.rowPitchBytes : readback.width * 4u;
        const uint8_t *center =
            pixels + (size_t)(readback.height / 2u) * pitch + (size_t)(readback.width / 2u) * 4u;
        shown = memcmp(pixels, center, 3u) != 0;
    }
    PlatformFree(pixels);
    return shown;
}

static void Frame(void *context)
{
    Viewer *viewer = (Viewer *)context;
    const double now = PlatformMonotonicSeconds();
    const float seconds = (float)(now - viewer->lastTime);
    viewer->lastTime = now;
    const LaiueWindowServiceV1 *windows = viewer->windows;
    if (windows->consumeFocusLoss(viewer->window))
        viewer->inputs->resetState(viewer->input);
    int32_t width = 0;
    int32_t height = 0;
    windows->getClientSize(viewer->window, &width, &height);
    if (windows->consumeResize(viewer->window) && width > 0 && height > 0)
        viewer->graphics->resize(viewer->device, width, height);
    if (width <= 0 || height <= 0)
        return;
    if (width != viewer->width || height != viewer->height)
    {
        viewer->width = width;
        viewer->height = height;
        if (viewer->fitted)
            Fit(viewer);
        else
            Clamp(viewer);
    }
    HandleKeys(viewer);
    HandlePointer(viewer, now);
    viewer->inputs->endFrame(viewer->input);
    Animate(viewer, now);

    LaiueGraphicsDeviceV2 *device = viewer->device;
    const bool check = viewer->smoke && viewer->frames == 2u;
    if (check && device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_READBACK_REQUEST_SIZE &&
        device->requestFrameReadback != NULL)
        (void)device->requestFrameReadback(device);
    // Камера переводит пиксели окна в координаты отсечения, y — вниз, как
    // на экране.
    const LaiueGraphicsCameraV2 camera = {
        .structSize = sizeof(camera),
        .viewProjection = {2.0f / (float)width, 0.0f, 0.0f, 0.0f, 0.0f, -2.0f / (float)height, 0.0f,
                           0.0f, 0.0f, 0.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f, 1.0f},
    };
    if (!UpdateShape(viewer) || !PrepareBar(viewer, seconds) ||
        !device->setCamera(device, &camera) ||
        !device->beginFrame(device, (uint32_t)width, (uint32_t)height))
        return;
    const bool drawn = DrawPicture(viewer) && DrawBar(viewer);
    if (!device->endFrame(device) || !drawn)
    {
        viewer->failed = true;
        windows->requestClose(viewer->window);
        return;
    }
    ++viewer->frames;
    if (check)
    {
        viewer->failed = !FrameShowsPicture(viewer);
        PlatformWriteConsoleUtf8(viewer->failed ? "viewer: the frame is empty\n"
                                                : "viewer: the picture is on screen\n");
        windows->requestClose(viewer->window);
    }
}

// Поверхность окна на Android исчезает в фоне и возвращается новой:
// графика отдаёт старую и строит цепочку кадров на новой.
static void SurfaceChanged(void *context, bool available)
{
    Viewer *viewer = (Viewer *)context;
    LaiueGraphicsDeviceV2 *device = viewer->device;
    if (device == NULL || device->structSize < LAIUE_GRAPHICS_DEVICE_V2_NATIVE_WINDOW_SIZE ||
        device->setNativeWindow == NULL)
        return;
    int32_t width = 0;
    int32_t height = 0;
    viewer->windows->getClientSize(viewer->window, &width, &height);
    void *native = available ? viewer->windows->getNativeHandle(viewer->window) : NULL;
    (void)device->setNativeWindow(device, native, width, height);
}

static bool CreateResources(Viewer *viewer)
{
    LaiueGraphicsDeviceV2 *device = viewer->device;
    if (device->structSize <
        offsetof(LaiueGraphicsDeviceV2, uploadTexture) + sizeof(device->uploadTexture))
        return false;
    const LaiueGraphicsBufferDescV1 vertices = {.structSize = sizeof(vertices),
                                                .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
                                                .sizeBytes = 4u * sizeof(LaiueGraphicsVertexV2)};
    const LaiueGraphicsBufferDescV1 indices = {.structSize = sizeof(indices),
                                               .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_INDEX,
                                               .sizeBytes = 6u * sizeof(uint32_t)};
    const LaiueGraphicsSamplerDescV1 sampler = {.structSize = sizeof(sampler),
                                                .minFilter = LAIUE_GRAPHICS_FILTER_LINEAR,
                                                .magFilter = LAIUE_GRAPHICS_FILTER_LINEAR,
                                                .addressModeU = LAIUE_GRAPHICS_ADDRESS_CLAMP,
                                                .addressModeV = LAIUE_GRAPHICS_ADDRESS_CLAMP,
                                                .addressModeW = LAIUE_GRAPHICS_ADDRESS_CLAMP};
    if (!device->createBuffer(device, &vertices, &viewer->vertices) ||
        !device->createBuffer(device, &indices, &viewer->indices) ||
        !device->createSampler(device, &sampler, &viewer->sampler))
        return false;
    static const uint32_t quad[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const LaiueGraphicsBufferUploadV1 upload = {.structSize = sizeof(upload),
                                                .buffer = viewer->indices,
                                                .data = quad,
                                                .sizeBytes = sizeof(quad)};
    if (!device->uploadBuffer(device, &upload))
        return false;
    if (device->structSize >= LAIUE_GRAPHICS_DEVICE_V2_CLEAR_COLOR_SIZE &&
        device->setClearColor != NULL)
    {
        // Цвет в линейном пространстве; на экране это #141417.
        const float background[3] = {0.0070f, 0.0070f, 0.0080f};
        (void)device->setClearColor(device, background);
    }
    return true;
}

// Каталог из аргумента, а без него — общий каталог файлов приложения на
// Android и текущий на остальных системах. Файл в аргументе открывается
// сам, а листаются соседи по его каталогу.
static bool ChooseDirectory(Viewer *viewer, const char *argument, char *selected)
{
    static char text[NAME_BYTES];
    selected[0] = '\0';
    text[0] = '\0';
    if (argument != NULL)
        Append(text, NAME_BYTES, argument);
    else if (PlatformGetEnvironmentUtf8("LAIUE_ANDROID_EXTERNAL_DATA", text, NAME_BYTES) == 0u)
        Append(text, NAME_BYTES, ".");
    if (!PlatformUtf8ToWide(text, Length(text), viewer->directory, LAIUE_PLATFORM_PATH_CAPACITY,
                            NULL))
        return false;
    PlatformPathInformation information;
    if (!PlatformGetPathInformation(viewer->directory, &information) || !information.exists ||
        information.isDirectory)
        return true;
    uint32_t cut = Length(text);
    while (cut > 0u && text[cut - 1u] != '/' && text[cut - 1u] != '\\')
        --cut;
    Append(selected, NAME_BYTES, text + cut);
    if (cut == 0u)
    {
        text[0] = '\0';
        Append(text, NAME_BYTES, ".");
    }
    else
    {
        // Корень «/» остаётся собой, у остальных отрезается разделитель.
        text[cut > 1u ? cut - 1u : cut] = '\0';
    }
    return PlatformUtf8ToWide(text, Length(text), viewer->directory, LAIUE_PLATFORM_PATH_CAPACITY,
                              NULL);
}

// Модули грузятся по имени, а модуль, которого нет рядом с программой,
// просто пропускается. Обязательны только окно, ввод и графика.
static bool LoadModules(Viewer *viewer, const char *provider, const char *providerId)
{
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic = {0};
    viewer->host = LaiueModuleHostCreate(&config, &diagnostic);
    if (viewer->host == NULL)
        return false;
    const char *names[] = {"window", "input", "ui", provider};
    const LaiueModuleProviderSelectionV1 selection = {.structSize = sizeof(selection),
                                                      .serviceName =
                                                          LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2,
                                                      .moduleId = providerId};
    const LaiueModuleNamedProfileV1 profile = {.structSize = sizeof(profile),
                                               .flags = LAIUE_MODULE_PROFILE_ALLOW_PARTIAL,
                                               .names = names,
                                               .nameCount = 4u,
                                               .providerSelections = &selection,
                                               .providerSelectionCount = 1u};
    const LaiueModuleStatus status =
        LaiueModuleHostLoadNamedProfileV1(viewer->host, &profile, NULL, &diagnostic);
    if (status != LAIUE_MODULE_OK && status != LAIUE_MODULE_PARTIAL)
        return false;
    viewer->windows = (const LaiueWindowServiceV1 *)LaiueModuleHostQueryService(
        viewer->host, LAIUE_WINDOW_SERVICE_NAME, 1u, LAIUE_WINDOW_SERVICE_V1_LEGACY_SIZE, NULL,
        &viewer->windowBytes);
    viewer->inputs = (const LaiueInputServiceV1 *)LaiueModuleHostQueryService(
        viewer->host, LAIUE_INPUT_SERVICE_NAME, 1u, LAIUE_INPUT_SERVICE_V1_LEGACY_SIZE, NULL,
        &viewer->inputBytes);
    viewer->graphics = (const LaiueGraphicsDeviceServiceV2 *)LaiueModuleHostQueryService(
        viewer->host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME_V2, 2u,
        LAIUE_GRAPHICS_DEVICE_SERVICE_V2_LEGACY_SIZE, NULL, &viewer->graphicsBytes);
    viewer->ui = (const LaiueUiServiceV1 *)LaiueModuleHostQueryService(
        viewer->host, LAIUE_UI_SERVICE_NAME, 1u, LAIUE_UI_SERVICE_V1_LEGACY_SIZE, NULL,
        &viewer->uiBytes);
    return viewer->windows != NULL && viewer->inputs != NULL && viewer->graphics != NULL;
}

static void Unload(Viewer *viewer)
{
    ReleasePicture(viewer);
    if (viewer->device != NULL)
    {
        viewer->device->destroyHandle(viewer->device, viewer->sampler);
        viewer->device->destroyHandle(viewer->device, viewer->indices);
        viewer->device->destroyHandle(viewer->device, viewer->vertices);
        viewer->graphics->destroyDevice(viewer->device);
    }
    if (viewer->uiContext != NULL)
        viewer->ui->contextDestroy(viewer->uiContext);
    if (viewer->input != NULL)
        viewer->inputs->destroy(viewer->input);
    if (viewer->window != NULL)
        viewer->windows->destroy(viewer->window);
    if (viewer->host != NULL)
        LaiueModuleHostDestroy(viewer->host);
    viewer->host = NULL;
    viewer->window = NULL;
    viewer->input = NULL;
    viewer->device = NULL;
    viewer->uiContext = NULL;
    viewer->vertices = 0u;
    viewer->indices = 0u;
    viewer->sampler = 0u;
    viewer->atlasWidth = 0;
    viewer->atlasHeight = 0;
}

static void InputFromWindow(void *context, void *raw)
{
    Viewer *viewer = (Viewer *)context;
    viewer->inputs->handleRawInput(viewer->input, raw);
}

// Окно, ввод, графика и интерфейс поверх загруженных модулей. Без модуля
// интерфейса картинка показывается без подписи.
static bool Open(Viewer *viewer)
{
    const WindowConfiguration configuration = {
        .title = L"laiue viewer", .width = 1280, .height = 800};
    viewer->window = viewer->windows->create(&configuration);
    if (viewer->window == NULL)
        return false;
    void *native = viewer->windows->getNativeHandle(viewer->window);
    viewer->input = viewer->inputs->create(native);
    if (viewer->input == NULL)
        return false;
    viewer->windows->setRawInputCallback(viewer->window, InputFromWindow, viewer);
    if (Has(viewer->windowBytes, viewer->windows->structSize,
            offsetof(LaiueWindowServiceV1, setSurfaceCallback),
            sizeof(viewer->windows->setSurfaceCallback)))
        viewer->windows->setSurfaceCallback(viewer->window, SurfaceChanged, viewer);
    viewer->windows->getClientSize(viewer->window, &viewer->width, &viewer->height);
    const LaiueGraphicsDeviceServiceV2 *graphics = viewer->graphics;
    const uint32_t created =
        Has(viewer->graphicsBytes, graphics->structSize,
            offsetof(LaiueGraphicsDeviceServiceV2, context), sizeof(graphics->context)) &&
                graphics->createDeviceWithContext != NULL
            ? graphics->createDeviceWithContext(graphics->context, native, viewer->width,
                                                viewer->height, LAIUE_GRAPHICS_BACKEND_AUTO,
                                                &viewer->device)
            : graphics->createDevice(native, viewer->width, viewer->height,
                                     LAIUE_GRAPHICS_BACKEND_AUTO, &viewer->device);
    if (!created || viewer->device == NULL || !CreateResources(viewer))
        return false;
    const LaiueUiServiceV1 *ui = viewer->ui;
    if (ui != NULL)
    {
        const uint32_t made = Has(viewer->uiBytes, ui->structSize,
                                  offsetof(LaiueUiServiceV1, context), sizeof(ui->context)) &&
                                      ui->contextCreateWithContext != NULL
                                  ? ui->contextCreateWithContext(ui->context, &viewer->uiContext)
                                  : ui->contextCreate(&viewer->uiContext);
        if (!made)
            viewer->uiContext = NULL;
    }
    return true;
}

int main(int argumentCount, char **arguments)
{
    static Viewer viewer;
    const char *path = NULL;
    for (int index = 1; index < argumentCount; ++index)
    {
        if (Equal(arguments[index], "--smoke"))
            viewer.smoke = true;
        else
            path = arguments[index];
    }
    static char selected[NAME_BYTES];
    if (!ChooseDirectory(&viewer, path, selected))
    {
        PlatformWriteConsoleUtf8("viewer: the path is not valid UTF-8\n");
        return 2;
    }
    ListPictures(&viewer);

    // Под Windows сначала D3D12, затем Vulkan; остальные системы — Vulkan.
    static const char *const providers[][2] = {
#if defined(_WIN32)
        {"graphics_d3d12", "laiue.graphics.d3d12"},
#endif
        {"graphics_vulkan", "laiue.graphics.vulkan"},
    };
    bool opened = false;
    for (size_t index = 0u; !opened && index < sizeof(providers) / sizeof(providers[0]); ++index)
    {
        opened = LoadModules(&viewer, providers[index][0], providers[index][1]) && Open(&viewer);
        if (!opened)
            Unload(&viewer);
    }
    bool failed = !opened;
    if (opened)
    {
        viewer.current = UINT32_MAX;
        uint32_t first = 0u;
        for (uint32_t index = 0u; index < viewer.files.count; ++index)
            if (Equal(FileName(&viewer, index), selected))
                first = index;
        bool shown = false;
        for (uint32_t index = first; !shown && index < viewer.files.count; ++index)
            shown = LoadPicture(&viewer, index);
        if (viewer.files.count == 0u)
            shown = ShowPattern(&viewer);
        viewer.lastTime = PlatformMonotonicSeconds();
        viewer.windows->runLoop(viewer.window, Frame, &viewer);
        failed = viewer.failed || (viewer.smoke && (!shown || viewer.frames < 3u));
    }
    else
    {
        PlatformWriteConsoleUtf8("viewer: no window and graphics modules could start\n");
    }
    Unload(&viewer);
    PlatformFree(viewer.files.offsets);
    PlatformFree(viewer.files.text);
    return failed ? 1 : 0;
}
