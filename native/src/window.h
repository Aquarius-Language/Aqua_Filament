// Aqua_Filament - minimal native window abstraction.
// The window is created and pumped exclusively on the library's render thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "third_party/json.hpp"

namespace aquafl {

using Json = nlohmann::json;

class NativeWindow {
public:
    virtual ~NativeWindow() = default;
    // Creates a visible (or hidden) top-level window. Returns false with `error` set on failure.
    virtual bool create(const std::string& title, int width, int height, bool resizable, bool visible,
            std::string& error) = 0;
    // Handle handed to Filament's Engine::createSwapChain (HWND on Windows, X11 Window id on Linux).
    virtual void* nativeHandle() const = 0;
    // Processes pending OS messages and appends portable events
    // ({"type":"key"|"mouse"|"resize"|"close"|"focus", ...}).
    virtual void pump(std::vector<Json>& events) = 0;
    // Current client-area size in physical pixels.
    virtual void size(int& width, int& height) const = 0;
    virtual void setTitle(const std::string& title) = 0;
    virtual bool closeRequested() const = 0;
};

NativeWindow* createNativeWindow();

} // namespace aquafl
