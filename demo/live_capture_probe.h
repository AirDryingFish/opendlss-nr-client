#pragma once

#include "live_x11.h"
#include <X11/Xlib.h>
#include <chrono>
#include <memory>
#include <thread>

// Use our own two-color source for both capture paths. Closing this private
// display connection also destroys its temporary window and graphics context.
inline void probeLiveCapture(bool window) {
    std::unique_ptr<Display, decltype(&XCloseDisplay)> connection(XOpenDisplay(nullptr), XCloseDisplay);
    Display* display = connection.get();
    if (!display) throw std::runtime_error("Cannot open the X11 display. Check DISPLAY and XAUTHORITY.");
    const int screen = DefaultScreen(display);
    const auto* visual = DefaultVisual(display, screen);
    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    attributes.background_pixel = visual->red_mask;
    constexpr int side = 32;
    Window source = XCreateWindow(display, RootWindow(display, screen), 0, 0, side, side, 0, CopyFromParent,
                                  InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel, &attributes);
    XMapRaised(display, source);
    XSync(display, False);
    LiveX11 capture(window ? "" : "0,0,32,32", window ? std::to_string(source) : "");
    GC brush = XCreateGC(display, source, 0, nullptr);
    XSetForeground(display, brush, visual->green_mask);
    XFillRectangle(display, source, brush, side / 2, 0, side / 2, side);
    XFreeGC(display, brush);
    XSync(display, False);

    // A compositor can publish the window into the root image one refresh later.
    std::vector<uint8_t> rgba;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        if (capture.read(rgba) == LiveX11::ReadResult::Frame && rgba.size() == side * side * 4) {
            const auto* red = rgba.data() + 4 * (side * side / 2 + side / 4);
            const auto* green = red + 4 * side / 2;
            if (red[0] > 160 && red[1] < 80 && green[0] < 80 && green[1] > 160) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("X11 capture did not return the test window's two colors.");
}
