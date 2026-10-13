task("hello")
    set_menu {
        usage = "xmake hello [options]",
        description = "执行一个自定义脚本",
        options = {
            {'n', "name", "kv", "World", "指定问候的对象"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local name = option.get("name")
        print("Hello, %s!", name)
        -- 跨平台写法：根据系统选择 cmd /c 或 sh -c
        if os.host() == "windows" then
            os.execv("cmd", {"/c", "echo This is a shell command."})
        else
            os.execv("sh", {"-c", "echo 'This is a shell command.'"})
        end

        os.execv("python", {"-c", "print('Hello from Python!')"})
    end)


-- 文档：正文在 doc/ 下按小节拆分，**根目录 readme.md 是生成产物**（见 doc/README.md）。
-- 所以改文档要改 doc/ 下的小文件，然后跑 `xmake doc` 合并回 readme.md。
-- 原来的 -o/--output 去掉了：readme.md 的位置是固定的（必须在仓库根，GitHub 才认），
-- 指定输出目录没有意义；换成了更实用的 --check（校验是否忘了合并）。
task("doc")
    set_menu {
        usage = "xmake doc [options]",
        description = "把 doc/ 下的分节文档合并成仓库根目录的 readme.md",
        options = {
            {'c', "check", "k", nil, "只校验 readme.md 与 doc/ 是否同步，不同步则报错退出"},
            {'l', "list",  "k", nil, "只列出参与合并的文件与行数"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("lib.detect.find_program")

        -- 脚本相对本 xmake.lua 所在目录（仓库根）定位，不依赖执行时的 cwd
        local script = path.join(os.scriptdir(), "doc", "build_readme.py")
        if not os.isfile(script) then
            raise("找不到文档合并脚本：%s", script)
        end

        -- Windows 上优先 python / py：`python3` 可能命中 Microsoft Store 的
        -- 「应用执行别名」存根（WindowsApps\python3.exe），那个存根不会报错，
        -- 而是挂起或弹出商店页。其余平台按惯例先 python3。
        local names = is_host("windows") and {"python", "py", "python3"} or {"python3", "python"}
        local python
        for _, n in ipairs(names) do
            local p = find_program(n)
            if p then
                if type(p) == "table" then p = p.program end
                python = p
                break
            end
        end
        if not python then
            raise("找不到 python，请先安装并确保它在 PATH 中")
        end

        local argv = {script}
        if option.get("check") then
            table.insert(argv, "--check")
        elseif option.get("list") then
            table.insert(argv, "--list")
        end

        -- os.execv 在子进程返回非 0 时是**直接抛错**（不是返回退出码），并且它自己
        -- 那句 "execv(...) failed(N), unknown reason" 对使用者没有指导意义，所以用
        -- try/catch 兜住，换成能照着做的提示。
        try {
            function ()
                os.execv(python, argv)
            end,
            catch {
                function (errors)
                    if option.get("check") then
                        raise("readme.md 与 doc/ 不同步，请执行 `xmake doc` 重新生成")
                    end
                    raise("文档生成失败：%s", errors)
                end
            }
        }
    end)