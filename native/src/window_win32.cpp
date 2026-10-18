// Aqua_Filament - Win32 window. Created and pumped on the library render thread.
#if defined(_WIN32)

#include "window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

namespace aquafl {
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), count);
    return out;
}

const char* keyName(WPARAM vk, char* buffer) {
    switch (vk) {
        case VK_SPACE: return "space";
        case VK_RETURN: return "enter";
        case VK_ESCAPE: return "escape";
        case VK_TAB: return "tab";
        case VK_BACK: return "backspace";
        case VK_DELETE: return "delete";
        case VK_LEFT: return "left";
        case VK_RIGHT: return "right";
        case VK_UP: return "up";
        case VK_DOWN: return "down";
        case VK_HOME: return "home";
        case VK_END: return "end";
        case VK_PRIOR: return "pageup";
        case VK_NEXT: return "pagedown";
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "shift";
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "control";
        case VK_MENU: case VK_LMENU: case VK_RMENU: return "alt";
        case VK_OEM_MINUS: return "-";
        case VK_OEM_PLUS: return "=";
        case VK_OEM_COMMA: return ",";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_2: return "/";
        case VK_OEM_4: return "[";
        case VK_OEM_6: return "]";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(buffer, 8, "f%d", int(vk - VK_F1 + 1)); return buffer; }
    if (vk >= 'A' && vk <= 'Z') { buffer[0] = char('a' + (vk - 'A')); buffer[1] = 0; return buffer; }
    if (vk >= '0' && vk <= '9') { buffer[0] = char(vk); buffer[1] = 0; return buffer; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { buffer[0] = char('0' + (vk - VK_NUMPAD0)); buffer[1] = 0; return buffer; }
    return nullptr;
}

const wchar_t* kClassName = L"AquaFilamentWindow";

class Win32Window final : public NativeWindow {
public:
    ~Win32Window() override {
        if (mHwnd) DestroyWindow(mHwnd);
    }

    bool create(const std::string& title, int width, int height, bool resizable, bool visible,
            std::string& error) override {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Win32Window::proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc); // Re-registration after a reload fails harmlessly.

        // Per-monitor DPI awareness for this (render) thread only: the swap chain then matches the physical
        // pixels instead of being bitmap-stretched. Requested sizes are logical (96 DPI) units.
        using SetDpiContext = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        using GetDpi = UINT(WINAPI*)();
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (auto setContext = reinterpret_cast<SetDpiContext>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"))) {
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            if (auto systemDpi = reinterpret_cast<GetDpi>(GetProcAddress(user32, "GetDpiForSystem"))) {
                UINT dpi = systemDpi();
                if (dpi > 96) { width = MulDiv(width, (int)dpi, 96); height = MulDiv(height, (int)dpi, 96); }
            }
        }
        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        RECT rect{0, 0, width, height};
        AdjustWindowRect(&rect, style, FALSE);
        mHwnd = CreateWindowExW(0, kClassName, widen(title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
        if (!mHwnd) {
            error = "CreateWindowEx failed / 無法建立視窗 (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        SetWindowLongPtrW(mHwnd, GWLP_USERDATA, (LONG_PTR)this);
        if (visible) {
            ShowWindow(mHwnd, SW_SHOW);
            UpdateWindow(mHwnd);
            SetForegroundWindow(mHwnd);
        }
        RECT client{};
        GetClientRect(mHwnd, &client);
        mWidth = client.right - client.left;
        mHeight = client.bottom - client.top;
        return true;
    }

    void* nativeHandle() const override { return (void*)mHwnd; }

    void pump(std::vector<Json>& events) override {
        mEvents = &events;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        mEvents = nullptr;
        // Resize events that arrived while no pump was active are reported here.
        if (mResized) {
            mResized = false;
            events.push_back({{"type", "resize"}, {"width", mWidth}, {"height", mHeight}});
        }
    }

    void size(int& width, int& height) const override {
        width = mWidth;
        height = mHeight;
    }

    void setTitle(const std::string& title) override { SetWindowTextW(mHwnd, widen(title).c_str()); }

    bool closeRequested() const override { return mClose; }

private:
    void push(Json&& event) {
        if (mEvents) mEvents->push_back(std::move(event));
    }

    LRESULT handle(UINT message, WPARAM w, LPARAM l) {
        switch (message) {
            case WM_CLOSE:
                mClose = true;
                push({{"type", "close"}});
                return 0; // The application decides when to close.
            case WM_SIZE: {
                int width = LOWORD(l), height = HIWORD(l);
                if (width > 0 && height > 0 && (width != mWidth || height != mHeight)) {
                    mWidth = width;
                    mHeight = height;
                    mResized = true;
                }
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case 0x02E0: { // WM_DPICHANGED: adopt the size Windows suggests for the new monitor
                auto* rect = reinterpret_cast<RECT*>(l);
                SetWindowPos(mHwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_SETFOCUS: push({{"type", "focus"}, {"focused", true}}); break;
            case WM_KILLFOCUS: push({{"type", "focus"}, {"focused", false}}); break;
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
            case WM_KEYUP: case WM_SYSKEYUP: {
                bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                if (down && (l & (1 << 30))) return 0; // ignore auto-repeat
                char buffer[16] = {};
                if (const char* name = keyName(w, buffer)) push({{"type", "key"}, {"key", name}, {"down", down}});
                if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) break; // keep Alt+F4
                return 0;
            }
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
                bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
                int button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) ? 0
                        : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) ? 1 : 2;
                if (down) SetCapture(mHwnd); else ReleaseCapture();
                push({{"type", "mouse"}, {"action", down ? "down" : "up"}, {"button", button},
                        {"x", GET_X_LPARAM(l)}, {"y", GET_Y_LPARAM(l)}});
                return 0;
            }
            case WM_MOUSEMOVE:
                push({{"type", "mouse"}, {"action", "move"}, {"x", GET_X_LPARAM(l)}, {"y", GET_Y_LPARAM(l)}});
                return 0;
            case WM_MOUSEWHEEL: {
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(mHwnd, &p);
                push({{"type", "mouse"}, {"action", "wheel"}, {"x", p.x}, {"y", p.y},
                        {"delta", GET_WHEEL_DELTA_WPARAM(w) / (double)WHEEL_DELTA}});
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW(mHwnd, message, w, l);
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
            auto* self = static_cast<Win32Window*>(create->lpCreateParams);
            self->mHwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) return self->handle(message, w, l);
        return DefWindowProcW(hwnd, message, w, l);
    }

    HWND mHwnd = nullptr;
    std::vector<Json>* mEvents = nullptr;
    int mWidth = 0, mHeight = 0;
    bool mClose = false, mResized = false;
};

} // namespace

NativeWindow* createNativeWindow() { return new Win32Window(); }

} // namespace aquafl

#endif
