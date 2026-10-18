// Aqua_Filament - Xlib window for Linux (works on Xorg and XWayland).
#if defined(__linux__) && !defined(__ANDROID__)

#include "window.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

#include <cstring>

namespace aquafl {
namespace {

const char* keyName(KeySym sym, char* buffer) {
    switch (sym) {
        case XK_space: return "space";
        case XK_Return: case XK_KP_Enter: return "enter";
        case XK_Escape: return "escape";
        case XK_Tab: return "tab";
        case XK_BackSpace: return "backspace";
        case XK_Delete: return "delete";
        case XK_Left: return "left";
        case XK_Right: return "right";
        case XK_Up: return "up";
        case XK_Down: return "down";
        case XK_Home: return "home";
        case XK_End: return "end";
        case XK_Page_Up: return "pageup";
        case XK_Page_Down: return "pagedown";
        case XK_Shift_L: case XK_Shift_R: return "shift";
        case XK_Control_L: case XK_Control_R: return "control";
        case XK_Alt_L: case XK_Alt_R: return "alt";
        default: break;
    }
    if (sym >= XK_F1 && sym <= XK_F12) {
        snprintf(buffer, 8, "f%d", int(sym - XK_F1 + 1));
        return buffer;
    }
    if (sym >= XK_a && sym <= XK_z) { buffer[0] = char('a' + (sym - XK_a)); buffer[1] = 0; return buffer; }
    if (sym >= XK_A && sym <= XK_Z) { buffer[0] = char('a' + (sym - XK_A)); buffer[1] = 0; return buffer; }
    if (sym >= XK_0 && sym <= XK_9) { buffer[0] = char('0' + (sym - XK_0)); buffer[1] = 0; return buffer; }
    if (sym >= XK_KP_0 && sym <= XK_KP_9) { buffer[0] = char('0' + (sym - XK_KP_0)); buffer[1] = 0; return buffer; }
    switch (sym) {
        case XK_minus: return "-";
        case XK_equal: return "=";
        case XK_comma: return ",";
        case XK_period: return ".";
        case XK_slash: return "/";
        case XK_bracketleft: return "[";
        case XK_bracketright: return "]";
        default: return nullptr;
    }
}

class X11Window final : public NativeWindow {
public:
    ~X11Window() override {
        if (mDisplay) {
            if (mWindow) XDestroyWindow(mDisplay, mWindow);
            XCloseDisplay(mDisplay);
        }
    }

    bool create(const std::string& title, int width, int height, bool resizable, bool visible,
            std::string& error) override {
        XInitThreads();
        mDisplay = XOpenDisplay(nullptr);
        if (!mDisplay) {
            error = "Cannot open X11 display (is DISPLAY set?) / 無法開啟 X11 顯示";
            return false;
        }
        int screen = DefaultScreen(mDisplay);
        XSetWindowAttributes attributes{};
        attributes.background_pixel = BlackPixel(mDisplay, screen);
        attributes.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask;
        mWindow = XCreateWindow(mDisplay, RootWindow(mDisplay, screen), 0, 0, (unsigned)width,
                (unsigned)height, 0, CopyFromParent, InputOutput, CopyFromParent,
                CWBackPixel | CWEventMask, &attributes);
        if (!mWindow) {
            error = "XCreateWindow failed / 無法建立視窗";
            return false;
        }
        mDelete = XInternAtom(mDisplay, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(mDisplay, mWindow, &mDelete, 1);
        if (!resizable) {
            XSizeHints* hints = XAllocSizeHints();
            hints->flags = PMinSize | PMaxSize;
            hints->min_width = hints->max_width = width;
            hints->min_height = hints->max_height = height;
            XSetWMNormalHints(mDisplay, mWindow, hints);
            XFree(hints);
        }
        int detectable = 0;
        XkbSetDetectableAutoRepeat(mDisplay, True, &detectable);
        setTitle(title);
        if (visible) {
            XMapWindow(mDisplay, mWindow);
            // Wait until the window is actually mapped so the swap chain sees a real drawable.
            for (XEvent e;;) {
                XNextEvent(mDisplay, &e);
                if (e.type == MapNotify) break;
            }
        }
        mWidth = width;
        mHeight = height;
        XFlush(mDisplay);
        return true;
    }

    void* nativeHandle() const override { return (void*)(uintptr_t)mWindow; }

    void pump(std::vector<Json>& events) override {
        while (XPending(mDisplay)) {
            XEvent e;
            XNextEvent(mDisplay, &e);
            switch (e.type) {
                case ClientMessage:
                    if ((Atom)e.xclient.data.l[0] == mDelete) {
                        mClose = true;
                        events.push_back({{"type", "close"}});
                    }
                    break;
                case ConfigureNotify:
                    if (e.xconfigure.width != mWidth || e.xconfigure.height != mHeight) {
                        mWidth = e.xconfigure.width;
                        mHeight = e.xconfigure.height;
                        events.push_back({{"type", "resize"}, {"width", mWidth}, {"height", mHeight}});
                    }
                    break;
                case KeyPress:
                case KeyRelease: {
                    char buffer[16] = {};
                    KeySym sym = XkbKeycodeToKeysym(mDisplay, (KeyCode)e.xkey.keycode, 0, 0);
                    const char* name = keyName(sym, buffer);
                    if (name) {
                        events.push_back({{"type", "key"}, {"key", name}, {"down", e.type == KeyPress}});
                    }
                    break;
                }
                case ButtonPress:
                case ButtonRelease: {
                    unsigned b = e.xbutton.button;
                    if (b == 4 || b == 5) {
                        if (e.type == ButtonPress) {
                            events.push_back({{"type", "mouse"}, {"action", "wheel"}, {"x", e.xbutton.x},
                                    {"y", e.xbutton.y}, {"delta", b == 4 ? 1.0 : -1.0}});
                        }
                        break;
                    }
                    int button = b == 1 ? 0 : b == 3 ? 1 : b == 2 ? 2 : -1;
                    if (button < 0) break;
                    events.push_back({{"type", "mouse"}, {"action", e.type == ButtonPress ? "down" : "up"},
                            {"button", button}, {"x", e.xbutton.x}, {"y", e.xbutton.y}});
                    break;
                }
                case MotionNotify:
                    events.push_back({{"type", "mouse"}, {"action", "move"}, {"x", e.xmotion.x},
                            {"y", e.xmotion.y}});
                    break;
                case FocusIn:
                case FocusOut:
                    events.push_back({{"type", "focus"}, {"focused", e.type == FocusIn}});
                    break;
                default:
                    break;
            }
        }
    }

    void size(int& width, int& height) const override {
        width = mWidth;
        height = mHeight;
    }

    void setTitle(const std::string& title) override {
        Atom utf8 = XInternAtom(mDisplay, "UTF8_STRING", False);
        Atom name = XInternAtom(mDisplay, "_NET_WM_NAME", False);
        XChangeProperty(mDisplay, mWindow, name, utf8, 8, PropModeReplace,
                (const unsigned char*)title.c_str(), (int)title.size());
        XStoreName(mDisplay, mWindow, title.c_str());
        XFlush(mDisplay);
    }

    bool closeRequested() const override { return mClose; }

private:
    Display* mDisplay = nullptr;
    Window mWindow = 0;
    Atom mDelete = 0;
    int mWidth = 0, mHeight = 0;
    bool mClose = false;
};

} // namespace

NativeWindow* createNativeWindow() { return new X11Window(); }

} // namespace aquafl

#endif
