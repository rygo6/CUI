# Platforms

A build has one window backend, set with `-DCUI_PLATFORM=` (`auto` is Win32 on Windows, Cocoa on macOS and X11 on Linux), and optionally one GPU renderer. `cui_app_backend` returns the backend's name.

| System | Window backend | Renderers | Window position |
|---|---|---|---|
| Windows | `win32` | OpenGL, software, Direct3D 12 (`CUI_D3D12`) | read and set |
| macOS | `cocoa` | OpenGL, software, Metal (`CUI_METAL`) | read and set |
| Linux, X11 | `x11` | OpenGL (GLX), software, Vulkan (`CUI_VULKAN`) | read and set |
| Linux, Wayland | `wayland` | OpenGL (EGL), software, Vulkan (`CUI_VULKAN`) | the compositor's |
| Any | `sdl2`, `glfw` | OpenGL, software | as the library gives it |

The GPU renderers and the Wayland window are built with CMake only, not in the single-file distribution. `CUI_RENDERER=gpu` picks the GPU renderer at run time; [Performance](performance.md#direct3d-12-vulkan-and-metal-against-opengl) compares them. Differences an app can meet:

- **One Linux window system per build.** An X11 build needs `libX11` and runs on a Wayland desktop through Xwayland; a Wayland build needs no X11 but does not run on a bare X server.
- **Wayland has no window positions.** `has_position` in a placement is false, `cui_app_set_position` does nothing, and the compositor places the window and restores a minimized one.
- **Display scale:** `CUI_SCALE` overrides it everywhere except Wayland, where the compositor decides.
- **Text input:** Win32, Cocoa and X11 have input methods; Wayland does not yet.

## Wayland windows

`-DCUI_PLATFORM=wayland` builds a native Wayland window instead of the X11 one. It needs a Wayland compositor at run time.

### Building

CMake needs `wayland-client`, `wayland-egl`, `wayland-cursor`, `xkbcommon`, `wayland-protocols` 1.31 or newer and `wayland-scanner`, stops with an error when one is missing or too old (Ubuntu 22.04 has `wayland-protocols` 1.25), and generates the protocol code at build time.

| System | Packages |
|---|---|
| Debian, Ubuntu | `libwayland-dev libxkbcommon-dev wayland-protocols` |
| Fedora | `wayland-devel libxkbcommon-devel wayland-protocols-devel` |
| Arch | `wayland libxkbcommon wayland-protocols` |

The backend is `src/platform/cui_app_wayland.c`. With `CUI_VULKAN` the build needs no X11 at all.

### What it does

- **Window:** an `xdg_toplevel`; `cui_app_desc.app_id` is its app ID.
- **Scale:** drawn at the display's own pixels. With `wp_fractional_scale_v1` and `wp_viewporter`, a scale of 1.5 gives a 1500 pixel picture for a 1000 point window; without them, the whole-number scale the outputs report.
- **Decorations:** the compositor's title bar where it offers `xdg-decoration` (KDE, Sway, other wlroots compositors). On GNOME the backend draws a plain bar on a subsurface (drag to move, double click to maximize, right click for the window menu, close, maximize and minimize buttons), and invisible border surfaces resize the window. It loads no toolkit and does not show the title.
- **Input:** pointer (motion, buttons, wheel and touchpad scrolling), keyboard through `xkbcommon` with the compositor's key repeat, cursors from the cursor theme (`XCURSOR_THEME`, `XCURSOR_SIZE`) at the display's scale, and clipboard text.
- **Renderers:** OpenGL through EGL (3.3 core, or ES 3.0 where there is no desktop OpenGL; loaded only when OpenGL is asked for), Vulkan through `VK_KHR_wayland_surface` (`cui_gpu_create` takes a `cui_x11_window` with `wl_surface` set and `display` the `wl_display`), and the software renderer through `wl_shm`, telling the compositor only the rectangle that changed.

### Pacing and idle

A frame is drawn when the compositor sends an event for the app or the app asked to be woken. Two things arrive on the same socket and must not start one:

- **The renderers' own events.** Mesa's Vulkan and EGL code put buffer releases and presentation feedback on the app's connection. The backend reads them, but only events its own handlers count end a wait; otherwise every frame shown would start the next, and an idle app would draw at the display's rate.
- **The frame callback.** OpenGL frames are paced by `wl_surface.frame`: the swap interval is 0, and the next swap waits for the last callback (at most a quarter of a second, since a hidden window gets none). Only that wait ends on the callback.

Vulkan frames are paced by the swapchain (FIFO). A FIFO presentation waits without end while the window is hidden, so each Vulkan frame also asks for a frame callback, and the next frame is drawn only once it came: a hidden window is not drawn, and is drawn again when it is seen.

### Not there yet

- Input methods (`text-input-v3`), compose sequences and dead keys.
- Dropped files.
- The window title in the built-in title bar, and hover feedback on its buttons.
- Window positions: Wayland lets a window neither read nor choose its place, and restoring a minimized window is the compositor's alone.
- Touch input and the primary selection (middle-click paste).
- A hidden app (`cui_app_desc.hidden`) that draws with Vulkan still shows its window when a screenshot is taken.
