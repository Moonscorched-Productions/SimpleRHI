--
-- @ Author: Amélie Heinrich
-- @ Create Time: 2026-11-13 00:00:00
-- @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
--

set_project("SimpleRHI")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
add_rules("plugin.compile_commands.autoupdate", { outputdir = "." })
set_languages("cxx17", "c11")
set_warnings("all")

-- Slang install (headers + libslang). Default: /usr/local on macOS, override with --slang_dir=...
option("slang_dir")
    set_default(is_host("macosx") and "/usr/local" or "")
    set_showmenu(true)
    set_description("Slang SDK root (contains include/slang and lib)")
option_end()

option("webgpu")
    set_default(true)
    set_showmenu(true)
    set_description("Build the WebGPU backend (wgpu-native on desktop, emdawnwebgpu on wasm)")
option_end()

add_requires("stb", "glm")
if not is_plat("wasm") then
    add_requires("libsdl3", "cgltf", "nlohmann_json")
    add_requires("imgui v1.92.7-docking", { configs = { sdl3 = true } })
    if has_config("webgpu") then
        add_requires("wgpu-native")
    end
end

-- Browser build (xmake f -p wasm): WebGPU through emdawnwebgpu. Asyncify lets the blocking parts of the
-- API (device creation, srhiDeviceWaitIdle) and the examples' main loop yield to the browser.
--
-- SDL3 comes from the Emscripten port, and ImGui is compiled from source against it (the xmake packages
-- would build their own SDL3).
if is_plat("wasm") then
    package("imgui-source")
        set_kind("library", { headeronly = true })
        add_urls("https://github.com/ocornut/imgui.git")
        add_versions("v1.92.7-docking", "v1.92.7-docking")
        on_install(function (package)
            os.cp("*.h", package:installdir("include"))
            os.cp("*.cpp", package:installdir("src"))
            os.cp("backends/imgui_impl_sdl3.h", package:installdir("include"))
            os.cp("backends/imgui_impl_sdl3.cpp", package:installdir("src"))
        end)
    package_end()
    add_requires("imgui-source v1.92.7-docking")

    add_cxflags("--use-port=emdawnwebgpu", "--use-port=sdl3")
    add_ldflags("--use-port=sdl3")
    add_ldflags("--use-port=emdawnwebgpu", "-sASYNCIFY", "-sASYNCIFY_STACK_SIZE=65536", "-sALLOW_MEMORY_GROWTH",
                "-sMAXIMUM_MEMORY=4GB", "-sSTACK_SIZE=1MB")
end

set_rundir("$(projectdir)")
includes("xmake/srhi.lua")

-- =====================================================================================================
-- Runtime
-- =====================================================================================================

target("SimpleRHI")
    set_kind("static")
    add_includedirs("Include", { public = true })
    add_headerfiles("Include/SimpleRHI.h")
    add_files("Source/SimpleRHI/srhi_common.cpp")
    if is_plat("macosx", "iphoneos") then
        add_files("Source/SimpleRHI/srhi_metal.mm")
        add_mxflags("-fobjc-arc")
        add_frameworks("Metal", "QuartzCore", "Foundation", "AppKit", { public = true })
        add_defines("SRHI_HAS_METAL")
    end
    if is_plat("windows") then
        add_files("Source/SimpleRHI/srhi_d3d11.cpp")
        add_syslinks("d3d11", "dxgi", "d3dcompiler", "dxguid", { public = true })
        add_defines("SRHI_HAS_D3D11")
    end
    if has_config("webgpu") or is_plat("wasm") then
        add_files("Source/SimpleRHI/srhi_webgpu.cpp")
        add_defines("SRHI_HAS_WEBGPU")
        if not is_plat("wasm") then
            add_packages("wgpu-native", { public = true })
        end
    end
target_end()

-- =====================================================================================================
-- Shader compiler
-- =====================================================================================================

if not is_plat("wasm") then
    target("SimpleRHICompiler")
        set_kind("static")
        add_includedirs("Include", { public = true })
        add_files("Source/SimpleRHI/srhi_compiler.cpp")
        on_load(function (target)
            local dir = get_config("slang_dir")
            if dir and dir ~= "" then
                target:add("includedirs", path.join(dir, "include", "slang"), path.join(dir, "include"), { public = true })
                target:add("linkdirs", path.join(dir, "lib"), { public = true })
                target:add("rpathdirs", path.join(dir, "lib"), { public = true })
            end
        end)
        add_links("slang", { public = true })
    target_end()

    target("srhi_bake")
        set_kind("binary")
        add_includedirs("Include")
        add_packages("cgltf", "stb")
        add_files("Source/Tools/AssetBaker/main.cpp")
        if is_plat("macosx") then
            add_files("Source/Tools/AssetBaker/anbc_impl.mm")
            add_mxflags("-fobjc-arc")
            add_frameworks("Metal", "Foundation")
        else
            add_files("Source/Tools/AssetBaker/anbc_impl.cpp")
            set_languages("cxx20") -- anbc.h uses designated initializers
        end
    target_end()

    target("srhic")
        set_kind("binary")
        set_policy("build.fence", true) -- shader rules run it, so it must be linked before dependents compile
        add_deps("SimpleRHICompiler")
        add_files("Source/Tools/ShaderCompiler/main.cpp")
    target_end()
end


-- =====================================================================================================
-- Tests
-- =====================================================================================================

if not is_plat("wasm") then
    target("srhi_tests")
        set_kind("binary")
        add_deps("SimpleRHI", "SimpleRHICompiler")
        add_packages("stb", "nlohmann_json")
        add_files("Tests/Framework/*.cpp", "Tests/Cases/*.cpp")
    target_end()
end

-- =====================================================================================================
-- ImGui backend and examples
-- =====================================================================================================

do
    target("SimpleRHIImGui")
        set_kind("static")
        add_deps("SimpleRHI")
        add_includedirs("Source/ImGui", { public = true })
        add_files("Source/ImGui/imgui_impl_srhi.cpp")
        if is_plat("wasm") then
            add_packages("imgui-source", { public = true })
            on_load(function (target)
                local pkg = target:pkg("imgui-source")
                if pkg then
                    target:add("files", path.join(pkg:installdir(), "src", "*.cpp"))
                end
            end)
        else
            add_packages("imgui", { public = true })
        end
    target_end()

    target("ExamplesCommon")
        set_kind("static")
        add_deps("SimpleRHI", "SimpleRHIImGui")
        add_packages("glm", "stb", { public = true })
        if not is_plat("wasm") then
            add_packages("libsdl3", "imgui", { public = true })
        end
        add_files("Source/Examples/Common/*.cpp")
        if is_plat("wasm") then
            after_build(function ()
                os.cp(path.join(os.projectdir(), "Source/Examples/Common/Index.html"), path.join(os.projectdir(), "build/web/index.html"))
            end)
        end
    target_end()

    for _, dir in ipairs(os.dirs(path.join(os.scriptdir(), "Source/Examples/*"))) do
        local name = path.filename(dir)
        if name ~= "Common" then
            target(name)
                set_kind("binary")
                set_group("Examples")
                add_deps("ExamplesCommon")
                add_files(path.join(dir, "*.cpp"))
                if is_plat("wasm") then
                    -- Shader packages and baked assets come from a native build (Content/Cache) and are
                    -- fetched over HTTP: serve the repository root and open /build/web/.
                    set_targetdir("$(projectdir)/build/web")
                    set_extension(".html")
                    add_ldflags("--shell-file", path.join(os.scriptdir(), "Source/Examples/Common/Shell.html"), { force = true })
                else
                    add_rules("srhi.shader")
                    add_deps("srhic")
                    add_files(path.join(dir, "*.slang"))
                end
            target_end()
        end
    end
end
