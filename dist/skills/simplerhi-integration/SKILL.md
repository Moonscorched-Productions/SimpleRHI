---
name: simplerhi-integration
description: Add the SimpleRHI single-header distribution to a C or C++ project and wire it into the build - CMake, xmake, plain compiler command lines, Emscripten - including building the srhic shader compiler, compiling .slang shaders at build time, platform link requirements and the ImGui backend. Use when setting up SimpleRHI in a new project, when a SimpleRHI build fails to compile or link, or when adding a platform (Windows, macOS, web) to an existing SimpleRHI project.
---

# Integrating SimpleRHI

The distribution is a folder that is copied into the project as is (for example `third_party/simplerhi/`):

| File | Purpose |
|---|---|
| `SimpleRHI.h` | The whole library: C API, runtime implementation, shader compiler implementation |
| `SimpleRHI.slang` | Shader include (`SRHI_BINDING`, `SRHI_UNIFORMS`) |
| `srhic.cpp` | Shader compiler command line tool, built from this one file |
| `imgui/` | Dear ImGui renderer backend (optional) |
| `example/` | Minimal SDL3 program with a working `CMakeLists.txt` and `xmake.lua` to copy from |
| `skills/` | Claude skills: copy the folders into the project's `.claude/skills/` |

`example/` is the reference integration: when in doubt, read its build files and mirror them.

## The four steps

1. **Add one implementation file.** The API is plain C and can be included anywhere; the implementation is C++17
   and must be compiled exactly once:

   ```cpp
   // srhi_impl.cpp   (on Apple platforms: compile as Objective-C++, or name it srhi_impl.mm)
   #define SRHI_IMPLEMENTATION
   #include "SimpleRHI.h"
   ```

   Never define `SRHI_IMPLEMENTATION` in a header or in more than one file. Do not define
   `SRHI_COMPILER_IMPLEMENTATION` in the game: it pulls in Slang. Only tools that compile shaders at runtime
   need it.

2. **Satisfy the platform** (table below).

3. **Build `srhic`** from `srhic.cpp` with the Slang SDK. It is a host tool: build it for the machine that
   compiles, also when cross-compiling.

4. **Compile each `.slang` file to a `.srhs` package** at build time and ship the packages with the
   executable (or embed them with `srhic --header`).

## Platform requirements

| Platform | Backend | Compile | Link |
|---|---|---|---|
| Windows | D3D11 | C++17 | `d3d11 dxgi d3dcompiler dxguid` (automatic with MSVC via `#pragma comment(lib)`; list them for MinGW / clang) |
| macOS, iOS | Metal 3 | Objective-C++ with ARC: `-x objective-c++ -fobjc-arc` (or a `.mm` file) | frameworks `Metal QuartzCore Foundation`, plus `AppKit` on macOS |
| Web (Emscripten) | WebGPU | `--use-port=emdawnwebgpu` | `--use-port=emdawnwebgpu -sASYNCIFY -sALLOW_MEMORY_GROWTH` |
| Desktop WebGPU (optional) | WebGPU via wgpu-native | `-DSRHI_ENABLE_WEBGPU`, wgpu-native headers (`webgpu/webgpu.h`, `webgpu/wgpu.h`) | `wgpu_native` |

Configuration macros, defined before the implementation include (or on the command line):
`SRHI_NO_D3D11`, `SRHI_NO_METAL` remove a default backend; `SRHI_ENABLE_WEBGPU` adds WebGPU on desktop.
Linux has no default backend: use `SRHI_ENABLE_WEBGPU`.

`srhic` needs the Slang SDK: `<slang>/include` on the include path, link `slang`, and the Slang shared library
(`slang.dll`, `libslang.so`, `libslang.dylib`) findable when it runs (on `PATH`, next to `srhic`, or via rpath).

## CMake

```cmake
set(SIMPLERHI_DIR "${CMAKE_SOURCE_DIR}/third_party/simplerhi")
set(SLANG_DIR "" CACHE PATH "Slang SDK root (contains include/ and lib/)")

# Shader compiler (host tool)
add_executable(srhic "${SIMPLERHI_DIR}/srhic.cpp")
target_compile_features(srhic PRIVATE cxx_std_17)
target_include_directories(srhic PRIVATE "${SLANG_DIR}/include")
target_link_directories(srhic PRIVATE "${SLANG_DIR}/lib")
target_link_libraries(srhic PRIVATE slang)
set_target_properties(srhic PROPERTIES BUILD_RPATH "${SLANG_DIR}/lib")

# One .srhs per .slang, rebuilt when the source changes, copied next to the executable
function(srhi_compile_shader target source)
    get_filename_component(name "${source}" NAME_WE)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.srhs")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND srhic "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${output}" -I "${SIMPLERHI_DIR}"
        DEPENDS srhic "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${SIMPLERHI_DIR}/SimpleRHI.slang"
        COMMENT "srhic ${source}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${output}" "$<TARGET_FILE_DIR:${target}>/${name}.srhs")
endfunction()

# The game
add_executable(game src/main.cpp src/srhi_impl.cpp)
target_compile_features(game PRIVATE cxx_std_17)
target_include_directories(game PRIVATE "${SIMPLERHI_DIR}")
srhi_compile_shader(game shaders/triangle.slang)

if(APPLE)
    set_source_files_properties(src/srhi_impl.cpp PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
    target_link_libraries(game PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit")
elseif(WIN32 AND NOT MSVC)
    target_link_libraries(game PRIVATE d3d11 dxgi d3dcompiler dxguid)
endif()
```

Shaders that `#include` shared headers: add those headers to `DEPENDS` and their folder with another `-I`.

## xmake

```lua
option("slang_dir")
    set_default("")
    set_showmenu(true)
option_end()

target("srhic")
    set_kind("binary")
    set_languages("cxx17")
    set_policy("build.fence", true)            -- it must be linked before targets that run it start building
    add_files("third_party/simplerhi/srhic.cpp")
    on_load(function (target)
        local dir = get_config("slang_dir")
        if dir and dir ~= "" then
            target:add("includedirs", path.join(dir, "include"))
            target:add("linkdirs", path.join(dir, "lib"))
            target:add("rpathdirs", path.join(dir, "lib"))
        end
    end)
    add_links("slang")

rule("srhi.shader")
    set_extensions(".slang")
    before_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        import("core.project.project")
        local srhic = project.target("srhic"):targetfile()
        local out = path.join(target:targetdir(), path.basename(sourcefile) .. ".srhs")
        batchcmds:show_progress(opt.progress, "${color.build.object}compiling.slang %s", sourcefile)
        batchcmds:mkdir(target:targetdir())
        batchcmds:vrunv(srhic, {sourcefile, "-o", out, "-I", path.join(os.projectdir(), "third_party/simplerhi")})
        batchcmds:add_depfiles(sourcefile, srhic)
        batchcmds:set_depmtime(os.mtime(out))
        batchcmds:set_depcache(target:dependfile(out))
    end)

target("game")
    set_kind("binary")
    set_languages("cxx17")
    add_rules("srhi.shader")
    add_deps("srhic", { inherit = false })
    add_includedirs("third_party/simplerhi")
    add_files("src/main.cpp", "shaders/*.slang")
    if is_plat("macosx", "iphoneos") then
        add_files("src/srhi_impl.cpp", { sourcekind = "mxx" })
        add_mxxflags("-fobjc-arc")
        add_frameworks("Metal", "QuartzCore", "Foundation", "AppKit")
    else
        add_files("src/srhi_impl.cpp")
    end
```

Configure with `xmake f --slang_dir=/path/to/slang`. On Windows run xmake from PowerShell or cmd.

## Plain command lines

```
# Windows, MSVC (Developer prompt)
cl /std:c++17 /EHsc srhic.cpp /I %SLANG%\include /link /LIBPATH:%SLANG%\lib slang.lib
srhic shaders\triangle.slang -o triangle.srhs -I third_party\simplerhi
cl /std:c++17 /EHsc main.cpp srhi_impl.cpp /I third_party\simplerhi

# macOS, clang
clang++ -std=c++17 srhic.cpp -I $SLANG/include -L $SLANG/lib -lslang -Wl,-rpath,$SLANG/lib -o srhic
./srhic shaders/triangle.slang -o triangle.srhs -I third_party/simplerhi
clang++ -std=c++17 main.cpp -x objective-c++ -fobjc-arc srhi_impl.cpp -I third_party/simplerhi \
    -framework Metal -framework QuartzCore -framework Foundation -framework AppKit -o game
```

## Web (Emscripten)

- Build `srhic` and the `.srhs` packages with a native build first: packages always contain WGSL.
- Compile and link everything with `--use-port=emdawnwebgpu`; link with `-sASYNCIFY` (device creation and
  `srhiDeviceWaitIdle` block by yielding to the browser), `-sALLOW_MEMORY_GROWTH`, and for larger apps
  `-sASYNCIFY_STACK_SIZE=65536 -sSTACK_SIZE=1MB`.
- Create the swapchain with `nativeWindow = NULL` and `canvasSelector = "#canvas"`.
- The frame loop must return to the browser once per frame: either `emscripten_set_main_loop`, or keep a plain
  loop and await `requestAnimationFrame` at the end of each iteration (possible because of Asyncify). Never
  spin or sleep on a thread while waiting for the GPU: call `emscripten_sleep` so callbacks can fire.
- Do not call a present function yourself; `srhiCommandBufferPresent` plus returning to the browser is enough.
- Ship packages and assets with `--preload-file`, or fetch them over HTTP.
- The page must be served over http(s) from a browser with WebGPU.

## Creating the swapchain from a window

`srhiSwapchainDesc::nativeWindow` is an `HWND` on Windows and a `CAMetalLayer*` on Apple platforms.

```c
// SDL3
#if defined(_WIN32)
desc.nativeWindow = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#elif defined(__APPLE__)   // create the window with SDL_WINDOW_METAL
desc.nativeWindow = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
#endif

// GLFW (create the window with GLFW_CLIENT_API = GLFW_NO_API)
desc.nativeWindow = glfwGetWin32Window(window);   // Windows; on macOS attach a CAMetalLayer to the NSView
```

Pass the size in pixels, not in points (`SDL_GetWindowSizeInPixels`, `glfwGetFramebufferSize`).

## ImGui backend

Add `imgui/imgui_impl_srhi.cpp` to the target that builds Dear ImGui (1.92 or newer, it uses the dynamic
texture API), with the SimpleRHI folder and ImGui on the include path. The shader is embedded in
`imgui_impl_srhi_shader.h`, so nothing has to be compiled or shipped. Usage is in the `simplerhi` skill's
recipes.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Unresolved `srhi*` symbols | No file defines `SRHI_IMPLEMENTATION`, or that file is not in the target |
| Duplicate `srhi*` symbols | `SRHI_IMPLEMENTATION` defined in more than one file |
| `#error ... C++17` | The implementation file is compiled as C; make it `.cpp` |
| `#error ... Objective-C++` | On Apple the implementation file needs `-x objective-c++ -fobjc-arc` or a `.mm` extension |
| ARC errors (`release` is unavailable / bridged cast) | `-fobjc-arc` is missing on the implementation file |
| Unresolved `D3D11CreateDevice`, `CreateDXGIFactory`, `D3DCompile`, `IID_*` | Non-MSVC Windows toolchain: link `d3d11 dxgi d3dcompiler dxguid` |
| `slang.h` not found, unresolved `slang_*` | Only `srhic` (or code defining `SRHI_COMPILER_IMPLEMENTATION`) needs Slang: point it at the SDK |
| `srhic` fails to start (missing DLL / dylib) | The Slang shared library is not on `PATH` / rpath |
| "not a valid shader package" at runtime | Packages built by an older `srhic`: rebuild shaders |
| `srhiDeviceCreate` returns NULL | The requested backend is not compiled in for this platform, or there is no suitable GPU |

After integrating, build and run `example/` once on the target machine: if it shows a rotating triangle, the
toolchain, Slang and the GPU backend all work.
