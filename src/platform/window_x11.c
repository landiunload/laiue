#include "platform/window.h"

#include "platform/system.h"

#define Window X11Window
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#undef Window

#include <string.h>

#define WINDOW_TEXT_CAPACITY 256u

struct Window
{
    Display *display;
    X11Window handle;
    Atom closeAtom;
    LaiueWindowNativeHandleV1 nativeHandle;
    int32_t clientWidth;
    int32_t clientHeight;
    bool resizePending;
    bool focusLossPending;
    bool mouseLookEnabled;
    bool focused;
    bool fullscreen;
    /* Enter of Alt+Enter is down: the toggle waits for its release. */
    bool fullscreenKeyHeld;
    bool closeRequested;
    float wheelSteps;
    /* Typed text until consumed: a ring of code points. */
    uint32_t text[WINDOW_TEXT_CAPACITY];
    uint32_t textHead;
    uint32_t textCount;
    RawInputCallback rawInputCallback;
    void *rawInputUserData;
};

static void ProcessEvent(Window *window, XEvent *event)
{
    if (window == NULL || event == NULL)
        return;
    switch (event->type)
    {
        case ConfigureNotify:
            window->clientWidth = event->xconfigure.width;
            window->clientHeight = event->xconfigure.height;
            window->resizePending = true;
            break;
        case FocusIn:
            window->focused = true;
            break;
        case FocusOut:
            if (window->focused)
                window->focusLossPending = true;
            window->focused = false;
            break;
        case ClientMessage:
            if ((Atom)event->xclient.data.l[0] == window->closeAtom)
                window->closeRequested = true;
            break;
        case KeyPress:
        {
            /* Latin-1 from the key; other scripts need an input method. */
            char bytes[8];
            KeySym symbol = 0;
            const int count =
                XLookupString(&event->xkey, bytes, (int)sizeof(bytes), &symbol, NULL);
            for (int index = 0; index < count; ++index)
            {
                const uint32_t codepoint = (uint32_t)(unsigned char)bytes[index];
                if (codepoint >= 0x20u && codepoint != 0x7Fu &&
                    window->textCount < WINDOW_TEXT_CAPACITY)
                {
                    window->text[(window->textHead + window->textCount) % WINDOW_TEXT_CAPACITY] =
                        codepoint;
                    window->textCount++;
                }
            }
            /* Alt+Enter switches full screen, as on Windows. */
            if (symbol == XK_Return && (event->xkey.state & Mod1Mask) != 0 &&
                !window->fullscreenKeyHeld)
            {
                window->fullscreenKeyHeld = true;
                WindowSetFullscreen(window, !window->fullscreen);
            }
            break;
        }
        case KeyRelease:
            if (XLookupKeysym(&event->xkey, 0) == XK_Return)
                window->fullscreenKeyHeld = false;
            break;
        case ButtonPress:
            if (event->xbutton.button == Button4)
                window->wheelSteps += 1.0f;
            else if (event->xbutton.button == Button5)
                window->wheelSteps -= 1.0f;
            break;
        default:
            break;
    }
    if (window->rawInputCallback != NULL)
        window->rawInputCallback(window->rawInputUserData, event);
}

Window *WindowCreate(const WindowConfiguration *configuration)
{
    if (configuration == NULL || configuration->width <= 0 || configuration->height <= 0)
        return NULL;
    Display *display = XOpenDisplay(NULL);
    if (display == NULL)
        return NULL;
    const int screen = DefaultScreen(display);
    Window *window = (Window *)PlatformAllocate(sizeof(*window), true);
    if (window == NULL)
    {
        XCloseDisplay(display);
        return NULL;
    }
    window->display = display;
    window->clientWidth = configuration->width;
    window->clientHeight = configuration->height;
    window->handle = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                                         (unsigned int)configuration->width,
                                         (unsigned int)configuration->height, 0,
                                         BlackPixel(display, screen),
                                         BlackPixel(display, screen));
    if (window->handle == 0)
    {
        PlatformFree(window);
        XCloseDisplay(display);
        return NULL;
    }
    XSelectInput(display, window->handle,
                 StructureNotifyMask | FocusChangeMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    /* A held key repeats KeyPress alone. By default every repeat also sends
     * a KeyRelease, and input would count each one as a new press. */
    (void)XkbSetDetectableAutoRepeat(display, True, NULL);
    window->closeAtom = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window->handle, &window->closeAtom, 1);
    char title[LAIUE_PLATFORM_PATH_CAPACITY];
    if (configuration->title == NULL ||
        !PlatformWideToUtf8(configuration->title, title, sizeof(title), NULL))
    {
        title[0] = 'L';
        title[1] = 'A';
        title[2] = 'I';
        title[3] = 'U';
        title[4] = 'E';
        title[5] = '\0';
    }
    XStoreName(display, window->handle, title);
    XMapWindow(display, window->handle);
    XFlush(display);
    window->nativeHandle.display = display;
    window->nativeHandle.window = (uintptr_t)window->handle;
    window->focused = true;
    return window;
}

void WindowDestroy(Window *window)
{
    if (window == NULL)
        return;
    if (window->display != NULL)
    {
        if (window->handle != 0)
            XDestroyWindow(window->display, window->handle);
        XCloseDisplay(window->display);
    }
    PlatformFree(window);
}

void *WindowGetNativeHandle(const Window *window)
{
    return window == NULL ? NULL : (void *)&window->nativeHandle;
}

void WindowSetRawInputCallback(Window *window, RawInputCallback callback, void *userData)
{
    if (window == NULL)
        return;
    window->rawInputCallback = callback;
    window->rawInputUserData = userData;
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
    while (!window->closeRequested && window->handle != 0)
    {
        while (XPending(window->display) != 0)
        {
            XEvent event;
            XNextEvent(window->display, &event);
            ProcessEvent(window, &event);
        }
        if (onFrame != NULL)
            onFrame(userData);
        if (!window->focused)
            PlatformSleepMilliseconds(50u);
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
        *x = -1;
    if (y != NULL)
        *y = -1;
    if (window == NULL || window->display == NULL || window->handle == 0)
        return;
    X11Window root;
    X11Window child;
    int rootX = 0;
    int rootY = 0;
    int localX = 0;
    int localY = 0;
    unsigned int mask = 0u;
    if (XQueryPointer(window->display, window->handle, &root, &child, &rootX, &rootY,
                      &localX, &localY, &mask) != False)
    {
        if (x != NULL)
            *x = localX;
        if (y != NULL)
            *y = localY;
    }
}

float WindowConsumeMouseWheelSteps(Window *window)
{
    if (window == NULL)
        return 0.0f;
    const float result = window->wheelSteps;
    window->wheelSteps = 0.0f;
    return result;
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
    if (window == NULL || window->display == NULL || window->fullscreen == enabled)
        return;
    /* The window manager stretches the window over its monitor without a
     * frame and gives the old size back (EWMH _NET_WM_STATE). */
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = window->handle;
    event.xclient.message_type = XInternAtom(window->display, "_NET_WM_STATE", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = enabled ? 1 : 0; /* _NET_WM_STATE_ADD or _REMOVE */
    event.xclient.data.l[1] =
        (long)XInternAtom(window->display, "_NET_WM_STATE_FULLSCREEN", False);
    event.xclient.data.l[3] = 1; /* asked by an application */
    XSendEvent(window->display, DefaultRootWindow(window->display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(window->display);
    window->fullscreen = enabled;
}

bool WindowIsFullscreen(const Window *window)
{
    return window != NULL && window->fullscreen;
}

void WindowSetSurfaceCallback(Window *window, SurfaceCallback callback, void *userData)
{
    // Поверхность окна X11 живёт вместе с окном и не меняется.
    (void)window;
    (void)callback;
    (void)userData;
}

float WindowGetContentScale(const Window *window)
{
    // Xft.dpi из ресурсов сервера — по нему масштабируют интерфейс GTK и Qt.
    // Без него точка равна пикселю.
    const char *resources =
        window != NULL && window->display != NULL ? XResourceManagerString(window->display) : NULL;
    static const char key[] = "Xft.dpi:";
    for (const char *line = resources; line != NULL && *line != '\0';)
    {
        if (strncmp(line, key, sizeof(key) - 1u) == 0)
        {
            const char *cursor = line + sizeof(key) - 1u;
            while (*cursor == ' ' || *cursor == '\t')
                ++cursor;
            // Целая часть и дробная: strtod зависит от локали процесса.
            float dpi = 0.0f;
            while (*cursor >= '0' && *cursor <= '9')
                dpi = dpi * 10.0f + (float)(*cursor++ - '0');
            if (*cursor == '.')
            {
                ++cursor;
                float unit = 0.1f;
                while (*cursor >= '0' && *cursor <= '9')
                {
                    dpi += unit * (float)(*cursor++ - '0');
                    unit *= 0.1f;
                }
            }
            return dpi >= 48.0f && dpi <= 960.0f ? dpi / 96.0f : 1.0f;
        }
        line = strchr(line, '\n');
        if (line != NULL)
            ++line;
    }
    return 1.0f;
}

// Политика энергопотребления процесса, WINDOW_POWER_*. У Linux нет режима
// эффективности, который процесс включал бы себе сам: политика только
// хранится.
static volatile uint32_t powerPolicy = WINDOW_POWER_AUTO;

bool WindowSetPowerPolicy(uint32_t policy)
{
    if (policy <= WINDOW_POWER_PERFORMANCE)
        PlatformAtomicStoreU32Release(&powerPolicy, policy);
    return false;
}

uint32_t WindowGetPowerPolicy(void)
{
    return PlatformAtomicLoadU32Acquire(&powerPolicy);
}

void WindowRequestClose(Window *window)
{
    if (window != NULL)
        window->closeRequested = true;
}
