// Окно Android поверх NativeActivity и native_app_glue из NDK.
//
// android_main принадлежит движку: он перенаправляет stdout и stderr в
// logcat, дожидается первой поверхности, делает рабочим каталогом
// внутренний каталог приложения и вызывает обычный main программы с
// аргументами из extra «args» запустившего intent. Поэтому одна и та же
// программа на laiue собирается для настольных систем и для Android без
// собственного Android-кода. У процесса одна активность, а значит, и одно
// окно: второе WindowCreate возвращает NULL.

#include "platform/window.h"

#include "platform/system.h"

#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WINDOW_TEXT_CAPACITY 256u
#define ANDROID_ARGUMENT_CAPACITY 64u
#define ANDROID_ARGUMENT_BYTES 4096u
#define ANDROID_LOG_TAG_CAPACITY 64u

struct Window
{
    struct android_app *app;
    int32_t clientWidth;
    int32_t clientHeight;
    int32_t cursorX;
    int32_t cursorY;
    bool resizePending;
    bool focusLossPending;
    bool focused;
    bool resumed;
    // Поверхность есть сейчас; была отдана, и о возвращении нужно сообщить.
    bool surfaceReady;
    bool surfaceLost;
    bool mouseLookEnabled;
    bool fullscreen;
    bool closeRequested;
    float wheelSteps;
    uint32_t text[WINDOW_TEXT_CAPACITY];
    uint32_t textHead;
    uint32_t textCount;
    RawInputCallback rawInputCallback;
    void *rawInputUserData;
    SurfaceCallback surfaceCallback;
    void *surfaceUserData;
};

// Активность процесса. android_main задаёт её до вызова main и обнуляет
// после: система может запустить android_main в том же процессе снова.
static struct android_app *androidApplication;
static Window *androidWindow;
static char androidLogTag[ANDROID_LOG_TAG_CAPACITY] = "laiue";

/* ---- stdout и stderr в logcat ------------------------------------------------------- */

typedef struct LogRedirect
{
    int pipeRead;
    int savedOutput;
    int savedError;
    pthread_t thread;
    bool running;
} LogRedirect;

static void *LogThread(void *context)
{
    const LogRedirect *redirect = (const LogRedirect *)context;
    char line[1024];
    size_t length = 0u;
    char buffer[512];
    for (;;)
    {
        const ssize_t count = read(redirect->pipeRead, buffer, sizeof(buffer));
        if (count <= 0)
            break;
        for (ssize_t index = 0; index < count; ++index)
        {
            const char character = buffer[index];
            if (character == '\n' || length + 1u == sizeof(line))
            {
                line[length] = '\0';
                __android_log_write(ANDROID_LOG_INFO, androidLogTag, line);
                length = 0u;
                if (character == '\n')
                    continue;
            }
            line[length++] = character;
        }
    }
    if (length != 0u)
    {
        line[length] = '\0';
        __android_log_write(ANDROID_LOG_INFO, androidLogTag, line);
    }
    return NULL;
}

static void LogRedirectStart(LogRedirect *redirect)
{
    redirect->running = false;
    redirect->pipeRead = -1;
    redirect->savedOutput = dup(STDOUT_FILENO);
    redirect->savedError = dup(STDERR_FILENO);
    int ends[2];
    if (redirect->savedOutput < 0 || redirect->savedError < 0 || pipe(ends) != 0)
        goto failed;
    // Строки уходят в logcat целиком, как только напечатаны.
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (dup2(ends[1], STDOUT_FILENO) < 0 || dup2(ends[1], STDERR_FILENO) < 0)
    {
        close(ends[0]);
        close(ends[1]);
        goto failed;
    }
    // Писать в канал остаются только дескрипторы 1 и 2: когда они
    // восстановлены, читатель получает конец файла.
    close(ends[1]);
    redirect->pipeRead = ends[0];
    if (pthread_create(&redirect->thread, NULL, LogThread, redirect) != 0)
    {
        dup2(redirect->savedOutput, STDOUT_FILENO);
        dup2(redirect->savedError, STDERR_FILENO);
        close(redirect->pipeRead);
        redirect->pipeRead = -1;
        goto failed;
    }
    redirect->running = true;
    return;
failed:
    if (redirect->savedOutput >= 0)
        close(redirect->savedOutput);
    if (redirect->savedError >= 0)
        close(redirect->savedError);
    redirect->savedOutput = -1;
    redirect->savedError = -1;
}

static void LogRedirectStop(LogRedirect *redirect)
{
    if (!redirect->running)
        return;
    fflush(stdout);
    fflush(stderr);
    dup2(redirect->savedOutput, STDOUT_FILENO);
    dup2(redirect->savedError, STDERR_FILENO);
    close(redirect->savedOutput);
    close(redirect->savedError);
    pthread_join(redirect->thread, NULL);
    close(redirect->pipeRead);
    redirect->running = false;
}

/* ---- События активности ------------------------------------------------------------- */

static void UpdateSize(Window *window)
{
    ANativeWindow *surface = window->app->window;
    if (surface == NULL)
        return;
    const int32_t width = ANativeWindow_getWidth(surface);
    const int32_t height = ANativeWindow_getHeight(surface);
    if (width > 0 && height > 0 && (width != window->clientWidth || height != window->clientHeight))
    {
        window->clientWidth = width;
        window->clientHeight = height;
        window->resizePending = true;
    }
}

static void HandleCommand(struct android_app *app, int32_t command)
{
    Window *window = (Window *)app->userData;
    if (window == NULL)
        return;
    switch (command)
    {
    case APP_CMD_INIT_WINDOW:
        window->surfaceReady = true;
        UpdateSize(window);
        if (window->surfaceLost)
        {
            window->surfaceLost = false;
            if (window->surfaceCallback != NULL)
                window->surfaceCallback(window->surfaceUserData, true);
        }
        break;
    case APP_CMD_TERM_WINDOW:
        // Поверхность освобождается системой сразу после возврата отсюда,
        // поэтому всё, что создано из неё, отдаётся прямо сейчас.
        if (window->surfaceReady && window->surfaceCallback != NULL)
            window->surfaceCallback(window->surfaceUserData, false);
        window->surfaceReady = false;
        window->surfaceLost = true;
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
    case APP_CMD_CONTENT_RECT_CHANGED:
        UpdateSize(window);
        break;
    case APP_CMD_GAINED_FOCUS:
        window->focused = true;
        break;
    case APP_CMD_LOST_FOCUS:
        if (window->focused)
            window->focusLossPending = true;
        window->focused = false;
        break;
    case APP_CMD_RESUME:
        window->resumed = true;
        break;
    case APP_CMD_PAUSE:
        window->resumed = false;
        break;
    case APP_CMD_DESTROY:
        window->closeRequested = true;
        break;
    default:
        break;
    }
}

// Печатные символы аппаратной клавиатуры. Ввод через метод ввода
// NativeActivity не доставляет.
static uint32_t KeyCharacter(int32_t code, int32_t meta)
{
    const bool shift = (meta & AMETA_SHIFT_ON) != 0;
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z)
        return (uint32_t)((shift != ((meta & AMETA_CAPS_LOCK_ON) != 0) ? 'A' : 'a') +
                          (code - AKEYCODE_A));
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9 && !shift)
        return (uint32_t)('0' + (code - AKEYCODE_0));
    switch (code)
    {
    case AKEYCODE_SPACE:
        return ' ';
    case AKEYCODE_COMMA:
        return shift ? '<' : ',';
    case AKEYCODE_PERIOD:
        return shift ? '>' : '.';
    case AKEYCODE_MINUS:
        return shift ? '_' : '-';
    case AKEYCODE_EQUALS:
        return shift ? '+' : '=';
    case AKEYCODE_SLASH:
        return shift ? '?' : '/';
    case AKEYCODE_SEMICOLON:
        return shift ? ':' : ';';
    case AKEYCODE_APOSTROPHE:
        return shift ? '"' : '\'';
    default:
        return 0u;
    }
}

static int32_t HandleInput(struct android_app *app, AInputEvent *event)
{
    Window *window = (Window *)app->userData;
    if (window == NULL || event == NULL)
        return 0;
    int32_t handled = 0;
    const int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_MOTION && AMotionEvent_getPointerCount(event) != 0u)
    {
        window->cursorX = (int32_t)(AMotionEvent_getX(event, 0u) + 0.5f);
        window->cursorY = (int32_t)(AMotionEvent_getY(event, 0u) + 0.5f);
        if ((AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) ==
            AMOTION_EVENT_ACTION_SCROLL)
            window->wheelSteps += AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0u);
        handled = 1;
    }
    else if (type == AINPUT_EVENT_TYPE_KEY)
    {
        const int32_t code = AKeyEvent_getKeyCode(event);
        // Громкость, питание и прочие системные клавиши остаются системе.
        if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN ||
            code == AKEYCODE_VOLUME_MUTE || code == AKEYCODE_POWER || code == AKEYCODE_HOME ||
            code == AKEYCODE_APP_SWITCH)
            return 0;
        if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN)
        {
            const uint32_t character = KeyCharacter(code, AKeyEvent_getMetaState(event));
            if (character != 0u && window->textCount < WINDOW_TEXT_CAPACITY)
            {
                window->text[(window->textHead + window->textCount) % WINDOW_TEXT_CAPACITY] =
                    character;
                window->textCount++;
            }
        }
        handled = 1;
    }
    if (window->rawInputCallback != NULL)
        window->rawInputCallback(window->rawInputUserData, event);
    return handled;
}

// Обрабатывает накопившиеся события; timeout -1 ждёт первого.
static void PumpEvents(struct android_app *app, int timeout)
{
    int events = 0;
    struct android_poll_source *source = NULL;
    while (ALooper_pollOnce(timeout, NULL, &events, (void **)&source) >= 0)
    {
        if (source != NULL)
            source->process(app, source);
        if (app->destroyRequested != 0)
            return;
        timeout = 0;
    }
}

/* ---- Window ------------------------------------------------------------------------- */

Window *WindowCreate(const WindowConfiguration *configuration)
{
    struct android_app *app = androidApplication;
    if (configuration == NULL || app == NULL || androidWindow != NULL)
        return NULL;
    Window *window = (Window *)PlatformAllocate(sizeof(*window), true);
    if (window == NULL)
        return NULL;
    window->app = app;
    window->focused = true;
    window->resumed = true;
    window->cursorX = -1;
    window->cursorY = -1;
    app->userData = window;
    androidWindow = window;
    // android_main уже дождался поверхности; окно, созданное позже, берёт
    // её как есть, а если её отняли — ждёт возвращения.
    while (app->window == NULL && app->destroyRequested == 0)
        PumpEvents(app, -1);
    if (app->window == NULL)
    {
        WindowDestroy(window);
        return NULL;
    }
    window->surfaceReady = true;
    UpdateSize(window);
    window->resizePending = false;
    return window;
}

void WindowDestroy(Window *window)
{
    if (window == NULL)
        return;
    if (window->app != NULL && window->app->userData == window)
        window->app->userData = NULL;
    if (androidWindow == window)
        androidWindow = NULL;
    PlatformFree(window);
}

void *WindowGetNativeHandle(const Window *window)
{
    return window == NULL ? NULL : (void *)window->app->window;
}

void WindowSetRawInputCallback(Window *window, RawInputCallback callback, void *userData)
{
    if (window == NULL)
        return;
    window->rawInputCallback = callback;
    window->rawInputUserData = callback != NULL ? userData : NULL;
}

void WindowSetSurfaceCallback(Window *window, SurfaceCallback callback, void *userData)
{
    if (window == NULL)
        return;
    window->surfaceCallback = callback;
    window->surfaceUserData = callback != NULL ? userData : NULL;
}

void WindowGetClientSize(const Window *window, int32_t *width, int32_t *height)
{
    if (width != NULL)
        *width = window == NULL ? 0 : window->clientWidth;
    if (height != NULL)
        *height = window == NULL ? 0 : window->clientHeight;
}

bool WindowConsumeResize(Window *window)
{
    if (window == NULL || !window->resizePending)
        return false;
    window->resizePending = false;
    return true;
}

bool WindowConsumeFocusLoss(Window *window)
{
    if (window == NULL || !window->focusLossPending)
        return false;
    window->focusLossPending = false;
    return true;
}

void WindowRunLoop(Window *window, FrameCallback onFrame, void *userData)
{
    if (window == NULL)
        return;
    struct android_app *app = window->app;
    while (!window->closeRequested && app->destroyRequested == 0)
    {
        // Кадры рисуются, пока активность на экране и у неё есть
        // поверхность; иначе поток спит до следующего события.
        const bool drawing = window->resumed && window->surfaceReady;
        PumpEvents(app, drawing ? 0 : -1);
        if (window->closeRequested || app->destroyRequested != 0)
            break;
        if (window->resumed && window->surfaceReady && onFrame != NULL)
            onFrame(userData);
    }
}

void WindowSetMouseLook(Window *window, bool enabled)
{
    if (window != NULL)
        window->mouseLookEnabled = enabled;
}

bool WindowIsMouseLookEnabled(const Window *window)
{
    return window != NULL && window->mouseLookEnabled;
}

void WindowGetCursorClientPosition(const Window *window, int32_t *x, int32_t *y)
{
    if (x != NULL)
        *x = window == NULL ? -1 : window->cursorX;
    if (y != NULL)
        *y = window == NULL ? -1 : window->cursorY;
}

void WindowRequestClose(Window *window)
{
    if (window != NULL)
        window->closeRequested = true;
}

float WindowConsumeMouseWheelSteps(Window *window)
{
    if (window == NULL)
        return 0.0f;
    const float steps = window->wheelSteps;
    window->wheelSteps = 0.0f;
    return steps;
}

uint32_t WindowConsumeText(Window *window, uint32_t *codepoints, uint32_t capacity)
{
    if (window == NULL || codepoints == NULL)
        return 0u;
    uint32_t written = 0u;
    while (written < capacity && window->textCount != 0u)
    {
        codepoints[written++] = window->text[window->textHead];
        window->textHead = (window->textHead + 1u) % WINDOW_TEXT_CAPACITY;
        window->textCount--;
    }
    return written;
}

void WindowSetFullscreen(Window *window, bool enabled)
{
    if (window == NULL)
        return;
    window->fullscreen = enabled;
    ANativeActivity_setWindowFlags(window->app->activity, enabled ? AWINDOW_FLAG_FULLSCREEN : 0u,
                                   enabled ? 0u : AWINDOW_FLAG_FULLSCREEN);
}

bool WindowIsFullscreen(const Window *window)
{
    return window != NULL && window->fullscreen;
}

/* ---- Точка входа -------------------------------------------------------------------- */

// Пакет из внутреннего каталога «/data/user/0/<пакет>/files»: им
// подписываются строки logcat.
static void SelectLogTag(const ANativeActivity *activity)
{
    const char *path = activity->internalDataPath;
    if (path == NULL)
        return;
    const char *end = strrchr(path, '/');
    if (end == NULL || end == path)
        return;
    const char *start = end - 1;
    while (start > path && *start != '/')
        --start;
    if (*start == '/')
        ++start;
    const size_t length = (size_t)(end - start);
    if (length == 0u || length >= sizeof(androidLogTag))
        return;
    memcpy(androidLogTag, start, length);
    androidLogTag[length] = '\0';
}

// Значение extra «args» запустившего intent: adb shell am start ... --es args "--smoke".
static size_t IntentArguments(ANativeActivity *activity, char *output, size_t capacity)
{
    output[0] = '\0';
    JavaVM *vm = activity->vm;
    JNIEnv *env = NULL;
    if (vm == NULL || activity->clazz == NULL ||
        (*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || env == NULL)
        return 0u;
    size_t length = 0u;
    if ((*env)->PushLocalFrame(env, 8) == 0)
    {
        jclass activityClass = (*env)->GetObjectClass(env, activity->clazz);
        jmethodID getIntent = activityClass == NULL
                                  ? NULL
                                  : (*env)->GetMethodID(env, activityClass, "getIntent",
                                                        "()Landroid/content/Intent;");
        jobject intent =
            getIntent == NULL ? NULL : (*env)->CallObjectMethod(env, activity->clazz, getIntent);
        jclass intentClass = intent == NULL ? NULL : (*env)->GetObjectClass(env, intent);
        jmethodID getExtra = intentClass == NULL
                                 ? NULL
                                 : (*env)->GetMethodID(env, intentClass, "getStringExtra",
                                                       "(Ljava/lang/String;)Ljava/lang/String;");
        jstring name = getExtra == NULL ? NULL : (*env)->NewStringUTF(env, "args");
        jstring value =
            name == NULL ? NULL : (jstring)(*env)->CallObjectMethod(env, intent, getExtra, name);
        if (value != NULL && !(*env)->ExceptionCheck(env))
        {
            const char *characters = (*env)->GetStringUTFChars(env, value, NULL);
            if (characters != NULL)
            {
                length = strlen(characters);
                if (length >= capacity)
                    length = capacity - 1u;
                memcpy(output, characters, length);
                output[length] = '\0';
                (*env)->ReleaseStringUTFChars(env, value, characters);
            }
        }
        if ((*env)->ExceptionCheck(env))
        {
            (*env)->ExceptionClear(env);
            length = 0u;
            output[0] = '\0';
        }
        (*env)->PopLocalFrame(env, NULL);
    }
    (*vm)->DetachCurrentThread(vm);
    return length;
}

// Делит строку пробелами на аргументы; кавычек нет.
static int SplitArguments(char *text, char **arguments, int capacity)
{
    int count = 0;
    char *cursor = text;
    while (*cursor != '\0' && count < capacity)
    {
        while (*cursor == ' ' || *cursor == '\t')
            *cursor++ = '\0';
        if (*cursor == '\0')
            break;
        arguments[count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t')
            ++cursor;
    }
    return count;
}

typedef int (*ProgramMain)(int argumentCount, char **arguments);

#if defined(LAIUE_STATIC)

// Окно собрано в ту же библиотеку, что и программа.
extern int main(int argumentCount, char **arguments);

static int RunProgram(ANativeActivity *activity, int count, char **arguments)
{
    (void)activity;
    return main(count, arguments);
}

#else

#include <android/asset_manager.h>
#include <dlfcn.h>

#define ANDROID_MAIN_LIBRARY_ASSET "laiue/main_library"
#define ANDROID_MAIN_LIBRARY_CAPACITY 96u

// Модуль окна — библиотека активности, а программа — отдельная
// библиотека приложения, имя которой упаковщик кладёт в ресурс
// laiue/main_library. Её main ищется так же, как точка входа модуля.
static int RunProgram(ANativeActivity *activity, int count, char **arguments)
{
    char name[ANDROID_MAIN_LIBRARY_CAPACITY];
    AAsset *asset = activity->assetManager == NULL
                        ? NULL
                        : AAssetManager_open(activity->assetManager, ANDROID_MAIN_LIBRARY_ASSET,
                                             AASSET_MODE_BUFFER);
    int length = asset == NULL ? -1 : AAsset_read(asset, name, sizeof(name) - 1u);
    if (asset != NULL)
        AAsset_close(asset);
    while (length > 0 &&
           (name[length - 1] == '\n' || name[length - 1] == '\r' || name[length - 1] == ' '))
        --length;
    if (length <= 0)
    {
        __android_log_write(ANDROID_LOG_ERROR, androidLogTag,
                            "the APK names no main library in " ANDROID_MAIN_LIBRARY_ASSET);
        return 1;
    }
    name[length] = '\0';
    char soname[ANDROID_MAIN_LIBRARY_CAPACITY + 8u];
    if (snprintf(soname, sizeof(soname), "lib%s.so", name) >= (int)sizeof(soname))
        return 1;
    void *library = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL)
    {
        __android_log_print(ANDROID_LOG_ERROR, androidLogTag, "%s: %s", soname, dlerror());
        return 1;
    }
    void *symbol = dlsym(library, "main");
    if (symbol == NULL)
    {
        __android_log_print(ANDROID_LOG_ERROR, androidLogTag, "%s exports no main", soname);
        dlclose(library);
        return 1;
    }
    ProgramMain program = NULL;
    memcpy((void *)&program, (const void *)&symbol, sizeof(program));
    const int code = program(count, arguments);
    dlclose(library);
    return code;
}

#endif

void android_main(struct android_app *app)
{
    androidApplication = app;
    androidWindow = NULL;
    app->userData = NULL;
    app->onAppCmd = HandleCommand;
    app->onInputEvent = HandleInput;
    SelectLogTag(app->activity);

    LogRedirect redirect;
    LogRedirectStart(&redirect);

    // Файлы, записанные по относительному пути, ложатся в каталог
    // приложения, а не в корень, куда писать нельзя.
    if (app->activity->internalDataPath != NULL && chdir(app->activity->internalDataPath) != 0)
        __android_log_write(ANDROID_LOG_WARN, androidLogTag,
                            "the application directory is not reachable");
    // Оба каталога приложения программа находит в окружении: закрытый —
    // он же текущий — и общий, куда файлы кладут снаружи (adb push).
    if (app->activity->internalDataPath != NULL)
        (void)setenv("LAIUE_ANDROID_DATA", app->activity->internalDataPath, 1);
    if (app->activity->externalDataPath != NULL)
        (void)setenv("LAIUE_ANDROID_EXTERNAL_DATA", app->activity->externalDataPath, 1);

    static char argumentText[ANDROID_ARGUMENT_BYTES];
    static char programName[ANDROID_LOG_TAG_CAPACITY];
    char *arguments[ANDROID_ARGUMENT_CAPACITY + 1u];
    memcpy(programName, androidLogTag, sizeof(programName));
    arguments[0] = programName;
    (void)IntentArguments(app->activity, argumentText, sizeof(argumentText));
    const int count =
        1 + SplitArguments(argumentText, arguments + 1, (int)ANDROID_ARGUMENT_CAPACITY - 1);
    arguments[count] = NULL;

    // Программа начинается с готовой поверхностью: долгая подготовка до
    // WindowCreate не держит поток интерфейса Android в ожидании.
    while (app->window == NULL && app->destroyRequested == 0)
        PumpEvents(app, -1);
    if (app->destroyRequested == 0)
    {
        const int code = RunProgram(app->activity, count, arguments);
        if (code != 0)
            __android_log_print(ANDROID_LOG_WARN, androidLogTag, "main returned %d", code);
    }

    // Активность закрывается вместе с программой, и glue требует
    // дождаться её разрушения.
    if (app->destroyRequested == 0)
        ANativeActivity_finish(app->activity);
    while (app->destroyRequested == 0)
        PumpEvents(app, -1);
    app->userData = NULL;
    androidWindow = NULL;
    androidApplication = NULL;
    LogRedirectStop(&redirect);
}
