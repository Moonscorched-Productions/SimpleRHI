--
-- @ Author: Amélie Heinrich
-- @ Create Time: 2026-11-13 00:00:00
-- @ Copyright: Copyright (c) 2026 Amélie Heinrich. All rights reserved.
--

-- Compiles .slang files with srhic into Content/Cache/Shaders/<name>.srhs at build time.
-- Targets using it must add_deps("srhic").
rule("srhi.shader")
    set_extensions(".slang")
    before_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        import("core.project.project")
        local srhic = project.target("srhic"):targetfile()
        local root = os.projectdir()
        local outdir = path.join(root, "Content", "Cache", "Shaders")
        local out = path.join(outdir, path.basename(sourcefile) .. ".srhs")
        local includes = {
            path.join(root, "Source", "SimpleRHI", "Shaders"),
            path.join(root, "Source", "Examples", "Common"),
        }
        batchcmds:show_progress(opt.progress, "${color.build.object}compiling.slang %s", sourcefile)
        batchcmds:mkdir(outdir)
        batchcmds:vrunv(srhic, {sourcefile, "-o", out, "-I", includes[1], "-I", includes[2]})
        batchcmds:add_depfiles(sourcefile, path.join(includes[1], "SimpleRHI.slang"), path.join(includes[2], "Common.slang"), srhic)
        batchcmds:set_depmtime(os.mtime(out))
        batchcmds:set_depcache(target:dependfile(out))
    end)
rule_end()
