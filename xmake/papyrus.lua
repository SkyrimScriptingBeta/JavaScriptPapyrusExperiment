function compile_papyrus_scripts()
    before_build(function (target)
        print("Building Papyrus Scripts")
        local skyrim_with_ck = os.getenv("SKYRIM_WITH_CK")
        if not skyrim_with_ck then
            print("SKYRIM_WITH_CK environment variable not set")
            return
        end
        local ppj_path = path.join(target:scriptdir(), "papyrus.ppj")
        os.exec("pyro -i " .. ppj_path .. " --game-path \"" .. skyrim_with_ck .. "\"")
    end)
end
