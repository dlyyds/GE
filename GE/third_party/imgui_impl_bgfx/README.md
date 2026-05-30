# imgui_impl_bgfx

An [ImGui](https://github.com/ocornut/imgui) adapter for the [bgfx](https://github.com/bkaradzic/bgfx) renderer backend.

## Feature / Characteristics

1. Header-only.
2. **Only in C++**. It cannot compile in C.
3. Supports multiple viewport.
4. Supports dynamic textures. In fact, it doesn't even implement the old way. Come on, they've been using dynamic textures for nearly a year now. Are we just don't care to update?
5. Provides a larger set of configs for better control.
6. Probably the newest among the ones on GitHub and will follow the latest bgfx / ImGui versions.

## Before Using

### Bring Your Own Render Pipeline

Bringing your own shader and texture sampler to the backend is the expected default. But if you don't want to be bothered, initialize the backend with `.autoShaderSampler = true`. We will use the shader in the `shader` directory and create a sampler.

### Platform Bridge

**This is only for multiple viewport support**. If you don't need it, initialize the backend with `.enableMultiViewport = false`, and skip this section.

A set of callback functions that we specifically ask you to provide that cross the bridge from bgfx to the platform (backend).

The backend would probably don't work or crash on creating a new viewport if you don't provide them.

Currently there are two of them:

1. `void* getNativeWindowHandle(const ImGuiViewport& viewport)`: This callback must return the **OS native window handle** that bgfx needs for rendering. Refer to your platform backend's documentation to see how they put the information needed to `ImGuiViewport` so you can implement this function correctly.

   ImGui commonly exposes backend window information through `viewport.PlatformHandle` and `viewport.PlatformHandleRaw`. ImGui documents `PlatformHandle` as a **higher-level platform window handle** and `PlatformHandleRaw` as a **lower-level platform-native window handle**; on current ImGui, `PlatformHandleRaw` is explicitly described as expected to be an `HWND` on Win32 and **unused for other platforms**.

   The commonly followed contract is in two fields: `PlatformHandle` and `PlatformHandleRaw`. The former is a "higher level handle" that often cannot be used directly with bgfx but can be used to get the native window handle with some platform/library-specific API calls; the latter is a "lower level handle" that often is the native window handle or can be easily converted to the native window handle with some more API calls.

   Because of that, the default implementation that simply returns `viewport.PlatformHandleRaw` is **only a placeholder, not a solution**. It is **expected** that you provide your own implementation for abstraction layers such as SDL/GLFW/custom engines.

   For example, on SDL3, they store `SDL_GetWindowID(window)` in `viewport.PlatformHandle` and `viewport.PlatformHandleRaw` is not consistent, so you call 

2. `ImVec2 getFrameBufferScale(const ImGuiViewport& viewport)`: Return the number of **framebuffer pixels per ImGui logical unit** for this viewport. This is used to convert logical ImGui coordinates and sizes into actual framebuffer pixel sizes for rendering and clipping.

   This is different from content/DPI scaling: framebuffer scale answers “how many framebuffer pixels do I render for one ImGui unit?”, while DPI/content scale answers “how large should UI content appear to the user on this display?”

   The default one simply returns `viewport.FramebufferScale` and defaults to `{1, 1}` if not presented. This is usually sufficient **if the platform backend keeps `viewport.FramebufferScale` updated correctly**, which can be assumed if you're using a competent one.

## Usage

### Start Using

Include the header:

```cpp
#include "imgui_impl_bgfx.hpp"
```

And you are good to go. You shouldn't include other headers.

Check out the file itself for available functions. TL;DR: We mostly follow the conventional renderer backend contract, except you don't need to call `ImGui_Implbgfx_NewFrame` on new frames.

### Custom Header Paths

This backend uses [CherryGrove](https://github.com/cherryridge/CherryGrove)'s inclusion paths for bgfx and ImGui headers by default. If you have different paths in your project, check out `inclusions.hpp` and define the macros.

### Extra Functions

The backend exposes some internal data structures and functionalities for developers that might need more power.

Use with caution. Modifying anything here is probably dangerous.

Include this header for access to them:

```cpp
#include "imgui_impl_bgfx_extras.hpp"
```

### Documentation

Check out the comments and definitions in the files for documentation.

## License

Licensed under the MIT License.