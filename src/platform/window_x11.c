#include "platform/window.h"

#include "platform/system.h"

#define Window X11Window
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#undef Window

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
    bool closeRequested;
    float wheelSteps;
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

void WindowSetFullscreen(Window *window, bool enabled)
{
    if (window != NULL)
        window->fullscreen = enabled;
}

bool WindowIsFullscreen(const Window *window)
{
    return window != NULL && window->fullscreen;
}

void WindowRequestClose(Window *window)
{
    if (window != NULL)
        window->closeRequested = true;
}
