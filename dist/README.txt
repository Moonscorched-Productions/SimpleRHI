SimpleRHI - drop-in distribution
================================

SimpleRHI is a small rendering hardware interface for games: D3D11-class features, a Metal-shaped C API, and
three backends (D3D11 on Windows, Metal 3 on Apple platforms, WebGPU in the browser or through wgpu-native).
Shaders are written once in Slang and compiled offline into .srhs packages.

Copy this whole folder into your project (for example third_party/simplerhi/).


WHAT IS HERE
------------

  SimpleRHI.h        The whole library in one header: C API + runtime + shader compiler.
  SimpleRHI.slang    Include for your shaders (SRHI_BINDING, SRHI_UNIFORMS).
  srhic.cpp          The shader compiler command line tool. One file, needs the Slang SDK.
  imgui/             Dear ImGui renderer backend (optional, needs ImGui 1.92+).
  example/           A rotating triangle with SDL3, with a CMakeLists.txt and an xmake.lua.
  skills/            Claude skills. Copy the three folders into <your project>/.claude/skills/
                       simplerhi              using the API
                       simplerhi-shaders      writing and compiling shaders
                       simplerhi-integration  build system setup and troubleshooting

SimpleRHI.h, SimpleRHI.slang, srhic.cpp and imgui/ are generated from the SimpleRHI repository by
Tools/MakeDist/make_dist.py. Do not edit them here.


QUICK START
-----------

1. Add one implementation file to your project (C++17):

       // srhi_impl.cpp
       #define SRHI_IMPLEMENTATION
       #include "SimpleRHI.h"

   Everywhere else just #include "SimpleRHI.h". The API is plain C.

2. Platform requirements for that file:

   Windows      Nothing with MSVC (d3d11, dxgi, d3dcompiler and dxguid are linked automatically).
                MinGW / clang: link d3d11 dxgi d3dcompiler dxguid.
   macOS, iOS   Compile it as Objective-C++ with ARC: name it srhi_impl.mm or pass
                "-x objective-c++ -fobjc-arc". Link the Metal, QuartzCore, Foundation and AppKit frameworks.
   Web          Emscripten: compile and link with --use-port=emdawnwebgpu, link with -sASYNCIFY
                -sALLOW_MEMORY_GROWTH.
   Linux        No native backend. Define SRHI_ENABLE_WEBGPU and link wgpu-native.

   Options (define before the include or on the command line):
       SRHI_NO_D3D11, SRHI_NO_METAL    leave a default backend out
       SRHI_ENABLE_WEBGPU              add the WebGPU backend on desktop (needs wgpu-native's
                                       webgpu/webgpu.h and webgpu/wgpu.h, link wgpu_native)

3. Build the shader compiler. Get the Slang SDK from https://github.com/shader-slang/slang/releases
   (SLANG below is the folder that contains include/ and lib/):

       Windows:  cl /std:c++17 /EHsc srhic.cpp /I %SLANG%\include /link /LIBPATH:%SLANG%\lib slang.lib
       Others:   c++ -std=c++17 srhic.cpp -I $SLANG/include -L $SLANG/lib -lslang -Wl,-rpath,$SLANG/lib -o srhic

   The Slang shared library (slang.dll / libslang.so / libslang.dylib) must be on PATH or next to srhic.

4. Compile your shaders. One package per .slang file, for every backend at once:

       srhic shaders/triangle.slang -o triangle.srhs -I third_party/simplerhi

   Load it with srhiShaderModuleCreate(device, bytes, size, "triangle").
   "srhic --header name" writes a C array instead, to embed the shader in the executable.
   "srhic --dump file.srhs" prints what a package contains.


BUILDING THE EXAMPLE
--------------------

  CMake:   cd example
           cmake -B build -DSLANG_DIR=/path/to/slang [-DCMAKE_PREFIX_PATH=/path/to/SDL3]
           cmake --build build

  xmake:   cd example
           xmake f --slang_dir=/path/to/slang      (SDL3 is downloaded by xmake)
           xmake
           xmake run example

Both build srhic, compile triangle.slang and put triangle.srhs next to the executable. Their build files are
meant to be copied: they contain a reusable "compile a shader" function / rule.


USING THE SHADER COMPILER AS A LIBRARY
--------------------------------------

Tools and editors that compile shaders at runtime can use srhiCompileShader() directly. In one .cpp of that
tool, with the Slang SDK on the include path and slang linked:

       #define SRHI_COMPILER_IMPLEMENTATION
       #include "SimpleRHI.h"

The runtime (SRHI_IMPLEMENTATION) never needs Slang, so games do not ship it.


STATUS
------

Tested from this folder on Windows 11 (MSVC, Slang 2026.22): the full SimpleRHI test suite built against the
single header passes on D3D11 and on WebGPU (wgpu-native), and the example builds and runs with both CMake and
xmake. The header also compiles with Emscripten. The Metal section is generated from the same sources that
pass the test suite on macOS, but the single-header form has not been compiled on a Mac yet.
