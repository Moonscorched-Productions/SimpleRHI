-- SimpleRHI example with xmake.
--   xmake f --slang_dir=/path/to/slang-sdk     (the folder that contains include/ and lib/)
--   xmake && xmake run example
-- The Slang shared library (slang.dll / libslang.so / libslang.dylib) must be findable when srhic runs:
-- put it on PATH (Windows) or rely on the rpath set below (Linux, macOS).

set_project("srhi_example")
add_rules("mode.debug", "mode.release")
set_languages("cxx17")
add_requires("libsdl3")

option("slang_dir")
    set_default("")
    set_showmenu(true)
    set_description("Slang SDK root (contains include/ and lib/)")
option_end()

-- The shader compiler, built from one file.
target("srhic")
    set_kind("binary")
    set_policy("build.fence", true) -- other targets run it while they build
    add_files("../srhic.cpp")
    on_load(function (target)
        local dir = get_config("slang_dir")
        if dir and dir ~= "" then
            target:add("includedirs", path.join(dir, "include"))
            target:add("linkdirs", path.join(dir, "lib"))
            target:add("rpathdirs", path.join(dir, "lib"))
        end
    end)
    add_links("slang")
target_end()

target("example")
    set_kind("binary")
    add_deps("srhic", { inherit = false })
    add_includedirs("..")
    add_packages("libsdl3")
    add_files("main.cpp")
    if is_plat("macosx", "iphoneos") then
        -- The Metal backend is Objective-C++ with ARC.
        add_files("srhi_impl.cpp", { sourcekind = "mxx" })
        add_mxxflags("-fobjc-arc")
        add_frameworks("Metal", "QuartzCore", "Foundation", "AppKit")
    else
        add_files("srhi_impl.cpp")
    end
    if is_plat("mingw") then
        add_syslinks("d3d11", "dxgi", "d3dcompiler", "dxguid") -- MSVC links these automatically
    end
    -- Compile the shader next to the executable.
    before_build(function (target)
        import("core.project.project")
        local srhic = project.target("srhic"):targetfile()
        os.mkdir(target:targetdir())
        os.vrunv(srhic, { path.join(os.scriptdir(), "triangle.slang"), "-o", path.join(target:targetdir(), "triangle.srhs"),
                          "-I", path.join(os.scriptdir(), "..") })
    end)
target_end()
