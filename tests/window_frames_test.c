// Кадры по требованию на настоящем окне. Потерянное пробуждение здесь значит
// зависание: его ловит тайм-аут теста.

#include "mod/module_host.h"
#include "platform/system.h"
#include "platform/window_service.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#define WINDOW_MODULE_NAME L"laiue_window.dll"
#elif defined(__APPLE__)
#define WINDOW_MODULE_NAME L"liblaiue_window.dylib"
#else
#define WINDOW_MODULE_NAME L"liblaiue_window.so"
#endif

// CTest считает этот код пропуском: на машине без экрана окна не будет.
#define SKIP_EXIT_CODE 125
#define DEADLINE_SECONDS 0.02
#define DEADLINE_STAGE_SECONDS 0.2
// Без ожидания пустые кадры идут сотнями тысяч в секунду, а события окна
// при запуске добавляют лишь десятки.
#define MOST_DEFERRED_FRAMES 500u
#define WAKE_DELAY_MILLISECONDS 30u

typedef enum Stage
{
    STAGE_DEADLINE,
    STAGE_WAKE_FROM_THREAD,
    STAGE_WAKE_DURING_FRAME,
    STAGE_CLOSE_WHILE_DEFERRED,
    STAGE_DONE,
} Stage;

typedef struct Run
{
    const LaiueWindowServiceV1 *windows;
    Window *window;
    Stage stage;
    uint32_t frames;
    double stageStart;
    volatile uint32_t wakeSent;
    PlatformThread waker;
} Run;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Window frames check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Join(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
                 const wchar_t *name)
{
    uint32_t index = 0u;
    while (root[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
    {
        output[index] = root[index];
        ++index;
    }
    if (root[index] != L'\0')
        return false;
    if (index != 0u && output[index - 1u] != L'/' && output[index - 1u] != L'\\')
        output[index++] = L'/';
    uint32_t nameIndex = 0u;
    while (name[nameIndex] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index++] = name[nameIndex++];
    if (name[nameIndex] != L'\0')
        return false;
    output[index] = L'\0';
    return true;
}

static uint32_t WakeAfterDelay(void *context)
{
    Run *run = (Run *)context;
    PlatformSleepMilliseconds(WAKE_DELAY_MILLISECONDS);
    PlatformAtomicStoreU32Release(&run->wakeSent, 1u);
    run->windows->wakeFrame(run->window);
    return 0u;
}

static void Frame(void *context)
{
    Run *run = (Run *)context;
    const LaiueWindowServiceV1 *windows = run->windows;
    const double now = PlatformMonotonicSeconds();
    ++run->frames;
    switch (run->stage)
    {
    case STAGE_DEADLINE:
        // События окна будят раньше срока, но кадры всё равно редки.
        if (now - run->stageStart < DEADLINE_STAGE_SECONDS)
        {
            windows->deferNextFrame(run->window, DEADLINE_SECONDS);
            return;
        }
        Expect(run->frames >= 3u, "the deadline starts the next frame");
        Expect(run->frames <= MOST_DEFERRED_FRAMES, "deferred frames are rare");
        Expect(PlatformThreadStart(&run->waker, WakeAfterDelay, run), "the waking thread starts");
        run->stage = STAGE_WAKE_FROM_THREAD;
        windows->deferNextFrame(run->window, -1.0);
        return;
    case STAGE_WAKE_FROM_THREAD:
        // Событие окна тоже приводит сюда: ждём, пока поток действительно
        // разбудит цикл.
        if (PlatformAtomicLoadU32Acquire(&run->wakeSent) == 0u)
        {
            windows->deferNextFrame(run->window, -1.0);
            return;
        }
        PlatformThreadJoin(&run->waker);
        // Пробуждение из кадра не должно потеряться.
        run->stage = STAGE_WAKE_DURING_FRAME;
        windows->wakeFrame(run->window);
        windows->deferNextFrame(run->window, -1.0);
        return;
    case STAGE_WAKE_DURING_FRAME:
        run->stage = STAGE_CLOSE_WHILE_DEFERRED;
        // Без единого события этот кадр идёт только по сроку.
        windows->deferNextFrame(run->window, 0.0);
        return;
    case STAGE_CLOSE_WHILE_DEFERRED:
        // Отложенный кадр не задерживает закрытие.
        run->stage = STAGE_DONE;
        windows->requestClose(run->window);
        windows->deferNextFrame(run->window, -1.0);
        return;
    case STAGE_DONE:
        return;
    }
}

LAIUE_TEST_ENTRY(WindowFramesTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t modulePath[LAIUE_PLATFORM_PATH_CAPACITY];
    static LaiueModuleHostConfigV1 config;
    static LaiueModuleDiagnostic diagnostic;
    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "the executable directory is known");
    Expect(Join(modulePath, directory, WINDOW_MODULE_NAME), "the module path fits");
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "the module host creates");
    LaiueModuleBinaryV1 binary = {modulePath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueWindowServiceV1 *windows = (const LaiueWindowServiceV1 *)LaiueModuleHostQueryService(
        host, LAIUE_WINDOW_SERVICE_NAME, LAIUE_WINDOW_SERVICE_ABI_VERSION_1,
        LAIUE_WINDOW_SERVICE_V1_FRAMES_SIZE, &version, &size);
    Expect(windows != NULL && size >= LAIUE_WINDOW_SERVICE_V1_FRAMES_SIZE &&
               windows->deferNextFrame != NULL && windows->wakeFrame != NULL &&
               windows->consumeRedraw != NULL,
           "the window service has frames on demand");

    const WindowConfiguration configuration = {
        .title = L"laiue window frames test", .width = 160, .height = 120};
    static Run run;
    run.windows = windows;
    run.window = windows->create(&configuration);
    if (run.window == NULL)
    {
        // Там, где окно обязано быть, его отсутствие — ошибка, а не пропуск.
        char required[2];
        Expect(PlatformGetEnvironmentUtf8("LAIUE_WINDOW_TEST_REQUIRED", required,
                                          sizeof(required)) == 0u,
               "a window creates");
        LaiueTestRuntimeWrite("Skipped: this machine has no display\n");
        LaiueModuleHostDestroy(host);
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    run.stageStart = PlatformMonotonicSeconds();
    windows->runLoop(run.window, Frame, &run);
    Expect(run.stage == STAGE_DONE, "the loop ends after the close request");
    (void)windows->consumeRedraw(run.window);
    Expect(!windows->consumeRedraw(run.window), "a redraw request is reported once");
    windows->destroy(run.window);
    LaiueModuleHostDestroy(host);
    LaiueTestRuntimeWrite("Window frames passed\n");
    LAIUE_TEST_SUCCESS();
}
