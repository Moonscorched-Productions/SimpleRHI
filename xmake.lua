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

if not is_plat("wasm") then
    add_requires("libsdl3", "cgltf", "stb", "glm", "nlohmann_json")
    add_requires("imgui v1.92.7-docking", { configs = { sdl3 = true } })
    if has_config("webgpu") then
        add_requires("wgpu-native")
    end
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
        end
    target_end()

    target("srhic")
        set_kind("binary")
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

if not is_plat("wasm") then
    target("SimpleRHIImGui")
        set_kind("static")
        add_deps("SimpleRHI")
        add_packages("imgui", { public = true })
        add_includedirs("Source/ImGui", { public = true })
        add_files("Source/ImGui/imgui_impl_srhi.cpp")
    target_end()

    target("ExamplesCommon")
        set_kind("static")
        add_deps("SimpleRHI", "SimpleRHIImGui")
        add_packages("libsdl3", "imgui", "glm", "stb", "cgltf", { public = true })
        add_files("Source/Examples/Common/*.cpp")
    target_end()

    for _, dir in ipairs(os.dirs(path.join(os.scriptdir(), "Source/Examples/*"))) do
        local name = path.filename(dir)
        if name ~= "Common" then
            target(name)
                set_kind("binary")
                set_group("Examples")
                add_rules("srhi.shader")
                add_deps("ExamplesCommon", "srhic")
                add_files(path.join(dir, "*.cpp"), path.join(dir, "*.slang"))
            target_end()
        end
    end
end
