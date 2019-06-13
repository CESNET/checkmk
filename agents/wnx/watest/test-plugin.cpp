// test-plugin.cpp

//
#include "pch.h"

#include <time.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <string_view>

#include "cfg.h"
#include "cfg_details.h"
#include "cma_core.h"
#include "common/cfg_info.h"
#include "providers/plugins.h"
#include "read_file.h"

namespace cma {  // to become friendly for wtools classes

constexpr const char* SecondLine = "0, 1, 2, 3, 4, 5, 6, 7, 8";

static void CreatePluginInTemp(const std::filesystem::path& Path, int Timeout,
                               std::string Name) {
    std::ofstream ofs(Path.u8string());

    if (!ofs) {
        XLOG::l("Can't open file {} error {}", Path.u8string(), GetLastError());
        return;
    }

    ofs << "@echo off\n"
        //<< "timeout /T " << Timeout << " /NOBREAK > nul\n"
        << "powershell Start-Sleep " << Timeout << " \n"
        << "@echo ^<^<^<" << Name << "^>^>^>\n"
        << "@echo " << SecondLine << "\n";
}

static void RemoveFolder(const std::filesystem::path& Path) {
    namespace fs = std::filesystem;
    fs::path top = Path;
    fs::path dir_path;

    cma::PathVector directories;
    std::error_code ec;
    for (auto& p : fs::recursive_directory_iterator(top, ec)) {
        dir_path = p.path();
        if (fs::is_directory(dir_path)) {
            directories.push_back(fs::canonical(dir_path));
        }
    }

    for (std::vector<fs::path>::reverse_iterator rit = directories.rbegin();
         rit != directories.rend(); ++rit) {
        if (fs::is_empty(*rit)) {
            fs::remove(*rit);
        }
    }

    fs::remove_all(Path);
}

// because PluginMap is relative complicated(PluginEntry is not trivial)
// we will use special method to insert artificial data in map
static void InsertEntry(PluginMap& pm, const std::string& name, int timeout,
                        bool async, int cache_age) {
    namespace fs = std::filesystem;
    fs::path p = name;
    pm.emplace(std::make_pair(name, p));
    auto it = pm.find(name);
    cma::cfg::PluginInfo e = {async, timeout, cache_age, 1};
    it->second.applyConfigUnit(e, false);
}

TEST(PluginTest, TimeoutCalc) {
    using namespace cma::provider;
    {
        PluginMap pm;

        EXPECT_EQ(0, FindMaxTimeout(pm, provider::PluginType::all))
            << "empty should has 0 timeout";
    }

    // test async
    {
        PluginMap pm;
        InsertEntry(pm, "a1", 5, true, 0);
        EXPECT_EQ(5, FindMaxTimeout(pm, provider::PluginType::all));
        EXPECT_EQ(5, FindMaxTimeout(pm, provider::PluginType::async));
        EXPECT_EQ(0, FindMaxTimeout(pm, provider::PluginType::sync));
        InsertEntry(pm, "a2", 15, true, 0);
        EXPECT_EQ(15, FindMaxTimeout(pm, provider::PluginType::all));
        EXPECT_EQ(15, FindMaxTimeout(pm, provider::PluginType::async));
        EXPECT_EQ(0, FindMaxTimeout(pm, provider::PluginType::sync));
        InsertEntry(pm, "a3", 25, false, 100);
        EXPECT_EQ(25, FindMaxTimeout(pm, provider::PluginType::all));
        EXPECT_EQ(25, FindMaxTimeout(pm, provider::PluginType::async));
        EXPECT_EQ(0, FindMaxTimeout(pm, provider::PluginType::sync));

        InsertEntry(pm, "a4", 7, true, 100);
        EXPECT_EQ(25, FindMaxTimeout(pm, provider::PluginType::all));
        EXPECT_EQ(25, FindMaxTimeout(pm, provider::PluginType::async));
        EXPECT_EQ(0, FindMaxTimeout(pm, provider::PluginType::sync));

        InsertEntry(pm, "a4", 100, false, 0);  // sync
        EXPECT_EQ(100, FindMaxTimeout(pm, provider::PluginType::all));
        EXPECT_EQ(25, FindMaxTimeout(pm, provider::PluginType::async));
        EXPECT_EQ(100, FindMaxTimeout(pm, provider::PluginType::sync));
    }

    // test sync
    {
        PluginMap pm;
        InsertEntry(pm, "a1", 5, false, 0);
        EXPECT_EQ(5, FindMaxTimeout(pm, provider::PluginType::all));
        EXPECT_EQ(0, FindMaxTimeout(pm, provider::PluginType::async));
        EXPECT_EQ(5, FindMaxTimeout(pm, provider::PluginType::sync));
        InsertEntry(pm, "a2", 15, false, 0);
        EXPECT_EQ(15, FindMaxTimeout(pm, provider::PluginType::all));
        EXPECT_EQ(0, FindMaxTimeout(pm, provider::PluginType::async));
        EXPECT_EQ(15, FindMaxTimeout(pm, provider::PluginType::sync));

        InsertEntry(pm, "a3", 25, false, 100);
        EXPECT_EQ(25, FindMaxTimeout(pm, provider::PluginType::all));
        EXPECT_EQ(25, FindMaxTimeout(pm, provider::PluginType::async));
        EXPECT_EQ(15, FindMaxTimeout(pm, provider::PluginType::sync));
    }
}

TEST(PluginTest, JobStartSTop) {
    namespace fs = std::filesystem;

    fs::path temp_folder = cma::cfg::GetTempDir();

    CreatePluginInTemp(temp_folder / "a.cmd", 120, "a");

    auto [pid, job, process] =
        cma::tools::RunStdCommandAsJob((temp_folder / "a.cmd").wstring());
    ::Sleep(1000);
    TerminateJobObject(job, 21);
    if (job) CloseHandle(job);
    if (process) CloseHandle(process);
    /*

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32 process;
        ZeroMemory(&process, sizeof(process));
        process.dwSize = sizeof(process);
        Process32First(snapshot, &process);
        do {
            // process.th32ProcessId is the PID.
            if (process.th32ParentProcessID == pid) {
                cma::tools::win::KillProcess(process.th32ProcessID);
            }

        } while (Process32Next(snapshot, &process));

        CloseHandle(job);
    cma::tools::win::KillProcess(pid);
    */
}

TEST(PluginTest, Extensions) {
    using namespace std;

    auto pshell = MakePowershellWrapper();
    EXPECT_TRUE(pshell.find(L"powershell.exe") != std::wstring::npos);

    auto p = ConstructCommandToExec(L"a.exe");
    auto p_expected = L"\"a.exe\"";
    EXPECT_EQ(p, p_expected);

    p = ConstructCommandToExec(L"a.cmd");
    p_expected = L"\"a.cmd\"";
    EXPECT_EQ(p, p_expected);

    p = ConstructCommandToExec(L"a.bat");
    p_expected = L"\"a.bat\"";
    EXPECT_EQ(p, p_expected);

    p = ConstructCommandToExec(L"a.e");
    EXPECT_EQ(p.empty(), true);
    p = ConstructCommandToExec(L"xxxxxxxxx");
    EXPECT_EQ(p.empty(), true);

    p = ConstructCommandToExec(L"a.pl");
    p_expected = L"perl.exe \"a.pl\"";
    EXPECT_EQ(p, p_expected);

    p = ConstructCommandToExec(L"a.py");
    p_expected = L"python.exe \"a.py\"";
    EXPECT_EQ(p, p_expected);

    p = ConstructCommandToExec(L"a.vbs");
    p_expected = L"cscript.exe //Nologo \"a.vbs\"";
    EXPECT_EQ(p, p_expected);

    p = ConstructCommandToExec(L"a.ps1");
    p_expected =
        L"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File \"a.ps1\"";
    EXPECT_EQ(p, p_expected);
}

TEST(PluginTest, ConfigFolders) {
    using namespace cma::cfg;
    using namespace wtools;
    cma::OnStart(cma::AppType::test);
    {
        std::string s = "@core\\";
        auto result = cma::cfg::ReplacePredefinedMarkers(s);
        EXPECT_EQ(result, ConvertToUTF8(GetSystemPluginsDir()) + "\\");
    }

    {
        std::string s = "@user\\";
        auto result = cma::cfg::ReplacePredefinedMarkers(s);
        EXPECT_EQ(result, ConvertToUTF8(GetUserPluginsDir()) + "\\");
    }

    {
        std::string s = "@data\\";
        auto result = cma::cfg::ReplacePredefinedMarkers(s);
        EXPECT_EQ(result, ConvertToUTF8(GetUserDir()) + "\\");
    }

    {
        std::string s = "user\\";
        auto result = cma::cfg::ReplacePredefinedMarkers(s);
        EXPECT_EQ(result, s);
    }
}

TEST(PluginTest, ApplyConfig) {
    cma::PluginEntry pe("c:\\a\\x.cmd");
    EXPECT_EQ(pe.failures(), 0);
    pe.failures_ = 2;
    EXPECT_EQ(pe.failures(), 2);
    pe.retry_ = 0;
    EXPECT_EQ(pe.failed(), false);
    pe.retry_ = 1;
    EXPECT_EQ(pe.failed(), true);

    {
        cma::cfg::PluginInfo e = {true, 10, 1, 1};
        pe.applyConfigUnit(e, false);
        EXPECT_EQ(pe.failures(), 0);
        EXPECT_EQ(pe.async(), true);
        EXPECT_EQ(pe.local(), false);
        EXPECT_EQ(pe.retry(), 1);
        EXPECT_EQ(pe.timeout(), 10);
        EXPECT_EQ(pe.cacheAge(), cma::cfg::kMinimumCacheAge);

        pe.failures_ = 2;
        EXPECT_EQ(pe.failures(), 2);
        EXPECT_EQ(pe.failed(), true);
    }

    // heck that async configured entry reset to sync with data drop
    {
        pe.data_.resize(10);
        pe.failures_ = 5;
        EXPECT_EQ(pe.data().size(), 10);
        cma::cfg::PluginInfo e = {false, 10, 0, 11};
        pe.applyConfigUnit(e, true);
        EXPECT_EQ(pe.failures(), 0);
        EXPECT_EQ(pe.async(), false);
        EXPECT_EQ(pe.local(), true);
        EXPECT_EQ(pe.cacheAge(), 0);
        EXPECT_EQ(pe.retry(), 11);
        EXPECT_EQ(pe.failures(), 0);
        EXPECT_TRUE(pe.data().empty());
    }
}

static void CreateFileInTemp(const std::filesystem::path& Path) {
    std::ofstream ofs(Path.u8string());

    if (!ofs) {
        XLOG::l("Can't open file {} error {}", Path.u8string(), GetLastError());
        return;
    }

    ofs << Path.u8string() << std::endl;
}

// returns folder where
static cma::PathVector GetFolderStructure() {
    using namespace cma::cfg;
    namespace fs = std::filesystem;
    fs::path tmp = cma::cfg::GetTempDir();
    if (!fs::exists(tmp) || !fs::is_directory(tmp) ||
        tmp.u8string().find("\\temp") == 0 ||
        tmp.u8string().find("\\temp") == std::string::npos) {
        XLOG::l(XLOG::kStdio)("Cant create folder structure {} {} {}",
                              fs::exists(tmp), fs::is_directory(tmp),
                              tmp.u8string().find("\\temp"));
        return {};
    }
    PathVector pv;
    for (auto& folder : {"a", "b", "c"}) {
        auto dir = tmp / folder;
        pv.emplace_back(dir);
    }
    return pv;
}

static void MakeFolderStructure(cma::PathVector Paths) {
    using namespace cma::cfg;
    namespace fs = std::filesystem;
    for (auto& dir : Paths) {
        std::error_code ec;
        fs::create_directory(dir, ec);
        if (ec.value() != 0) {
            XLOG::l(XLOG::kStdio)("Can't create a folder {}", dir.u8string());
            continue;
        }

        CreateFileInTemp(dir / "x1.txt");
        CreateFileInTemp(dir / "x2.ps1");
        CreateFileInTemp(dir / "x3.ps2");
        CreateFileInTemp(dir / "y4.bat");
        CreateFileInTemp(dir / "z5.cmd");
        CreateFileInTemp(dir / "z6.exe");
        CreateFileInTemp(dir / "z7.vbs");
    }
}

static void RemoveFolderStructure(cma::PathVector Pv) {
    using namespace cma::cfg;
    for (auto& folder : Pv) {
        RemoveFolder(folder);
    }
}

TEST(PluginTest, ExeUnitCtor) {
    using namespace cma::cfg;

    // valid
    {
        bool as = false;
        int tout = 1;
        int age = 0;
        int retr = 2;
        Plugins::ExeUnit e("Plugin", as, tout, age, retr, true);
        EXPECT_EQ(e.async(), as);
        EXPECT_EQ(e.retry(), retr);
        EXPECT_EQ(e.timeout(), tout);
        EXPECT_EQ(e.cacheAge(), age);
        EXPECT_EQ(e.run(), true);
    }

    // valid async
    {
        bool as = true;
        int tout = 1;
        int age = 120;
        int retr = 2;
        Plugins::ExeUnit e("Plugin", as, tout, age, retr, true);
        EXPECT_EQ(e.async(), as);
        EXPECT_EQ(e.cacheAge(), age);
    }

    // valid async but low cache
    {
        bool as = true;
        int tout = 1;
        int age = cma::cfg::kMinimumCacheAge - 1;
        int retr = 2;
        Plugins::ExeUnit e("Plugin", as, tout, age, retr, true);
        EXPECT_EQ(e.async(), as);
        EXPECT_EQ(e.cacheAge(), cma::cfg::kMinimumCacheAge);
    }

    // sync but with cache age
    {
        bool as = false;
        int tout = 1;
        int age = cma::cfg::kMinimumCacheAge - 1;
        int retr = 2;
        Plugins::ExeUnit e("Plugin", as, tout, age, retr, true);
        EXPECT_EQ(e.async(), true);
        EXPECT_EQ(e.cacheAge(), cma::cfg::kMinimumCacheAge);
    }
}

TEST(PluginTest, HackPlugin) {
    std::vector<char> in;
    cma::tools::AddVector(in, std::string("<<<a>>>\r\n***\n<<<b>>>"));

    {
        std::vector<char> out;
        auto ret = cma::HackPluginDataWithCacheInfo(out, in, 123, 456);
        ASSERT_TRUE(ret);
        std::string str(out.data(), out.size());
        EXPECT_EQ(str, "<<<a:cached(123,456)>>>\n***\n<<<b:cached(123,456)>>>");
    }

    {
        std::vector<char> out;
        in.clear();
        cma::tools::AddVector(in, std::string("<<<a\r\n***"));
        auto ret = cma::HackPluginDataWithCacheInfo(out, in, 123, 456);
        ASSERT_TRUE(ret);
        std::string str(out.data(), out.size());
        EXPECT_EQ(str, "<<<a\n***");
    }

    {
        std::vector<char> out;
        in.clear();
        auto ret = cma::HackPluginDataWithCacheInfo(out, in, 123, 456);
        EXPECT_FALSE(ret);
    }

    {
        std::vector<char> out;
        in.clear();
        cma::tools::AddVector(in, std::string(" <<<a>>>\n***\n"));
        auto ret = cma::HackPluginDataWithCacheInfo(out, in, 123, 456);
        ASSERT_TRUE(ret);
        std::string str(out.data(), out.size());
        EXPECT_EQ(str, " <<<a>>>\n***\n");
    }
}

TEST(PluginTest, FilesAndFolders) {
    using namespace cma::cfg;
    using namespace wtools;
    namespace fs = std::filesystem;
    cma::OnStart(cma::AppType::test);
    {
        EXPECT_EQ(groups::localGroup.foldersCount(), 1);
        EXPECT_EQ(groups::plugins.foldersCount(), 2);
        PathVector pv;
        for (auto& folder : groups::plugins.folders()) {
            pv.emplace_back(folder);
        }
        auto files = cma::GatherAllFiles(pv);
        if (files.size() < 10) {
            auto f = groups::plugins.folders();
            XLOG::l(XLOG::kStdio | XLOG::kInfo)(
                "\n\nTEST IS SKIPPED> YOU HAVE NO PLUGINS {} {} {} {}\n\n\n ",
                wtools::ConvertToUTF8(f[0]), wtools::ConvertToUTF8(f[1]),
                wtools::ConvertToUTF8(f[2]), wtools::ConvertToUTF8(f[3]));
            return;
        }

        EXPECT_TRUE(files.size() > 20);

        auto execute = GetInternalArray(groups::kGlobal, vars::kExecute);

        cma::FilterPathByExtension(files, execute);
        EXPECT_TRUE(files.size() >= 6);
        cma::RemoveDuplicatedNames(files);

        auto yaml_units = GetArray<YAML::Node>(
            cma::cfg::groups::kPlugins, cma::cfg::vars::kPluginsExecution);
        std::vector<Plugins::ExeUnit> exe_units;
        cma::cfg::LoadExeUnitsFromYaml(exe_units, yaml_units);
        ASSERT_EQ(exe_units.size(), 3);

        EXPECT_EQ(exe_units[2].async(), false);
        EXPECT_EQ(exe_units[2].cacheAge(), 0);

        EXPECT_EQ(exe_units[0].timeout(), 30);
        EXPECT_EQ(exe_units[0].cacheAge(), 0);
        EXPECT_EQ(exe_units[0].async(), false);
        EXPECT_EQ(exe_units[0].retry(), 0);
    }

    {
        EXPECT_EQ(groups::localGroup.foldersCount(), 1);
        PathVector pv;
        for (auto& folder : groups::localGroup.folders()) {
            pv.emplace_back(folder);
        }
        auto files = cma::GatherAllFiles(pv);
        auto yaml_units = GetArray<YAML::Node>(
            cma::cfg::groups::kLocal, cma::cfg::vars::kPluginsExecution);
        std::vector<Plugins::ExeUnit> exe_units;
        cma::cfg::LoadExeUnitsFromYaml(exe_units, yaml_units);
        // no local files
        PluginMap pm;
        UpdatePluginMap(pm, true, files, exe_units, true);
        EXPECT_TRUE(pm.size() == 0);
        // for (auto& entry : pm) EXPECT_TRUE(entry.second.local());
    }

    {
        auto pv = GetFolderStructure();
        ASSERT_TRUE(0 != pv.size());
        RemoveFolderStructure(pv);
        MakeFolderStructure(pv);
        ON_OUT_OF_SCOPE(RemoveFolderStructure(pv));
        auto files = cma::GatherAllFiles(pv);
        ASSERT_EQ(files.size(), 21);

        const auto files_base = files;

        cma::FilterPathByExtension(files, {"exe"});
        EXPECT_EQ(files.size(), 3);

        files = files_base;
        cma::FilterPathByExtension(files, {"cmd"});
        EXPECT_EQ(files.size(), 3);

        files = files_base;
        cma::FilterPathByExtension(files, {"bad"});
        EXPECT_EQ(files.size(), 0);

        files = files_base;
        cma::FilterPathByExtension(files, {"exe", "cmd", "ps1"});
        EXPECT_EQ(files.size(), 9);

        files = files_base;
        cma::RemoveDuplicatedNames(files);
        EXPECT_EQ(files.size(), 7);
    }
}

static const std::vector<cma::cfg::Plugins::ExeUnit> exe_units_base = {
    //
    {"*.ps1", true, 10, 0, 5, true},     //
    {"*.cmd", false, 12, 500, 3, true},  //
    {"*", true, 10, 0, 3, false},        //
};

static const std::vector<cma::cfg::Plugins::ExeUnit> x2 = {
    //
    {"*.ps1", false, 13, 0, 9, true},  //
    {"*", true, 10, 0, 3, false},      //
};

static const std::vector<cma::cfg::Plugins::ExeUnit> x3_cmd = {
    //
    {"???-?.cmd", true, 10, 0, 5, true},  //
    {"*", true, 10, 0, 3, false},         //
};

static const std::vector<cma::cfg::Plugins::ExeUnit> x4_all = {
    //
    {"*.cmd", true, 10, 0, 5, false},  // disable all cmd
    {"*", true, 10, 0, 3, true},       // enable all other
};

static const cma::PathVector pv_main = {
    "c:\\z\\x\\asd.d.ps1",  // 0
    "c:\\z\\x\\1.ps2",      // 1
    "c:\\z\\x\\asd.d.exe",  // 2
    "c:\\z\\x\\asd.d.cmd",  // 3
    "c:\\z\\x\\asd.d.bat",  // 4
    "c:\\z\\x\\asd-d.cmd"   // 5
};

static const cma::PathVector pv_short = {
    "c:\\z\\x\\asd.d.cmd",  //
    "c:\\z\\x\\asd.d.bat",  //
    "c:\\z\\x\\asd-d.cmd"   //
};

TEST(PluginTest, GeneratePluginEntry) {
    using namespace cma::cfg;
    using namespace wtools;
    namespace fs = std::filesystem;
    cma::OnStart(cma::AppType::test);
    {
        {
            auto pv = FilterPathVector(pv_main, exe_units_base, false);
            EXPECT_EQ(pv.size(), 3);
            EXPECT_EQ(pv[0], pv_main[0]);
            EXPECT_EQ(pv[1], pv_main[3]);
            EXPECT_EQ(pv[2], pv_main[5]);
        }

        {
            auto pv = FilterPathVector(pv_main, x2, false);
            EXPECT_EQ(pv.size(), 1);
            EXPECT_EQ(pv[0], pv_main[0]);
        }

        {
            auto pv = FilterPathVector(pv_main, x4_all, false);
            EXPECT_EQ(pv.size(),
                      pv_main.size() - 2);  // two coms are excluded
        }

        {
            auto pv = FilterPathVector(pv_main, x4_all, true);
            EXPECT_EQ(pv.size(), 0);  // nothing
        }

        // Filter and Insert
        {
            PluginMap pm;
            InsertInPluginMap(pm, {});
            EXPECT_EQ(pm.size(), 0);

            auto pv = FilterPathVector(pv_main, exe_units_base, false);
            InsertInPluginMap(pm, pv);
            EXPECT_EQ(pm.size(), pv.size());
            for (auto& f : pv) {
                EXPECT_FALSE(nullptr == GetEntrySafe(pm, f.u8string()));
            }

            InsertInPluginMap(pm, pv);  // no changes(the same)
            EXPECT_EQ(pm.size(), pv.size());

            pv.pop_back();
            FilterPluginMap(pm, pv);
            EXPECT_EQ(pm.size(), pv.size());

            FilterPluginMap(pm, {});
            EXPECT_EQ(pm.size(), 0);

            InsertInPluginMap(pm, pv_main);
            EXPECT_EQ(pm.size(), pv_main.size());
            ApplyExeUnitToPluginMap(pm, exe_units_base, true);
            auto e_0 = GetEntrySafe(pm, pv_main[0].u8string());
            ASSERT_NE(nullptr, e_0);
            EXPECT_EQ(e_0->local(), true);

            auto e_3 = GetEntrySafe(pm, pv_main[3].u8string());
            ASSERT_NE(nullptr, e_3);
            EXPECT_EQ(e_3->local(), true);

            auto e_5 = GetEntrySafe(pm, pv_main[4].u8string());
            ASSERT_NE(nullptr, e_5);
            EXPECT_EQ(e_5->local(), true);
        }

        PluginMap pm;
        UpdatePluginMap(pm, false, pv_main, exe_units_base);
        EXPECT_EQ(pm.size(), 0);

        UpdatePluginMap(pm, false, pv_main, exe_units_base, true);
        EXPECT_EQ(pm.size(), 0);
        UpdatePluginMap(pm, false, pv_main, exe_units_base, false);
        EXPECT_EQ(pm.size(), 3);  // 1 ps1 and 2 cmd

        auto e = GetEntrySafe(pm, "c:\\z\\x\\asd.d.ps1");
        ASSERT_NE(nullptr, e);
        EXPECT_EQ(e->async(), true);
        EXPECT_EQ(e->path(), "c:\\z\\x\\asd.d.ps1");
        EXPECT_EQ(e->timeout(), 10);
        EXPECT_EQ(e->cacheAge(), cma::cfg::kMinimumCacheAge);
        EXPECT_EQ(e->retry(), 5);

        e = GetEntrySafe(pm, "c:\\z\\x\\asd.d.cmd");
        ASSERT_NE(nullptr, e);
        EXPECT_EQ(e->async(), true);
        EXPECT_EQ(e->path(), "c:\\z\\x\\asd.d.cmd");
        EXPECT_EQ(e->timeout(), 12);
        EXPECT_EQ(e->cacheAge(), 500);
        EXPECT_EQ(e->retry(), 3);

        e = GetEntrySafe(pm, "c:\\z\\x\\asd-d.cmd");
        ASSERT_NE(nullptr, e);
        EXPECT_EQ(e->async(), true);
        EXPECT_EQ(e->path(), "c:\\z\\x\\asd-d.cmd");
        EXPECT_EQ(e->timeout(), 12);
        EXPECT_EQ(e->cacheAge(), 500);
        EXPECT_EQ(e->retry(), 3);

        // Update
        UpdatePluginMap(pm, false, pv_main, x2, false);
        EXPECT_EQ(pm.size(), 1);
        e = GetEntrySafe(pm, "c:\\z\\x\\asd.d.ps1");
        ASSERT_NE(nullptr, e);
        EXPECT_EQ(e->async(), false);
        EXPECT_EQ(e->path(), "c:\\z\\x\\asd.d.ps1");
        EXPECT_EQ(e->timeout(), x2[0].timeout());
        EXPECT_EQ(e->cacheAge(), x2[0].cacheAge());
        EXPECT_EQ(e->retry(), x2[0].retry());

        // Update
        UpdatePluginMap(pm, false, pv_short, x3_cmd, false);
        EXPECT_EQ(pm.size(), 1);
        e = GetEntrySafe(pm, "c:\\z\\x\\asd-d.cmd");
        ASSERT_NE(nullptr, e);
        EXPECT_EQ(e->async(), true);
        EXPECT_EQ(e->path(), "c:\\z\\x\\asd-d.cmd");
        EXPECT_EQ(e->timeout(), x3_cmd[0].timeout());
        EXPECT_EQ(e->cacheAge(), x3_cmd[0].cacheAge());
        EXPECT_EQ(e->retry(), x3_cmd[0].retry());

        UpdatePluginMap(pm, false, pv_main, x4_all, false);
        EXPECT_EQ(pm.size(), 4);

        // two files are dropped
        e = GetEntrySafe(pm, pv_main[3].u8string());
        ASSERT_EQ(nullptr, e);
        e = GetEntrySafe(pm, pv_main[5].u8string());
        ASSERT_EQ(nullptr, e);

        // four files a left
        ASSERT_NE(nullptr, GetEntrySafe(pm, pv_main[0].u8string()));
        ASSERT_NE(nullptr, GetEntrySafe(pm, pv_main[1].u8string()));
        ASSERT_NE(nullptr, GetEntrySafe(pm, pv_main[2].u8string()));
        ASSERT_NE(nullptr, GetEntrySafe(pm, pv_main[4].u8string()));
    }
}

TEST(PluginTest, SyncStartSimulationFuture_Long) {
    using namespace cma::cfg;
    using namespace wtools;
    namespace fs = std::filesystem;
    using namespace std::chrono;
    cma::OnStart(cma::AppType::test);
    std::vector<Plugins::ExeUnit> exe_units = {
        //
        {"*.cmd", false, 10, 0, 3, true},  //
        {"*", true, 10, 0, 3, false},      //
    };

    fs::path temp_folder = cma::cfg::GetTempDir();

    CreatePluginInTemp(temp_folder / "a.cmd", 5, "a");
    CreatePluginInTemp(temp_folder / "b.cmd", 0, "b");
    CreatePluginInTemp(temp_folder / "c.cmd", 3, "c");
    CreatePluginInTemp(temp_folder / "d.cmd", 120, "d");

    PathVector vp = {
        (temp_folder / "a.cmd").u8string(),  //
        (temp_folder / "b.cmd").u8string(),  //
        (temp_folder / "c.cmd").u8string(),  //
        (temp_folder / "d.cmd").u8string(),  //
    };

    std::vector<std::string> strings = {
        "<<<a>>>",  //
        "<<<b>>>",  //
        "<<<c>>>",  //
        "<<<d>>>",  // not delivered!
    };

    ON_OUT_OF_SCOPE(for (auto& f : vp) fs::remove(f););

    PluginMap pm;  // load from the groups::plugin
    UpdatePluginMap(pm, false, vp, exe_units, false);

    using namespace std;
    using DataBlock = vector<char>;

    vector<future<DataBlock>> results;
    int requested_count = 0;

    // sync part
    for (auto& entry_pair : pm) {
        auto& entry_name = entry_pair.first;
        auto& entry = entry_pair.second;

        // C++ async black magic
        results.emplace_back(std::async(
            std::launch::async,  // first param

            [](cma::PluginEntry* Entry) -> DataBlock {  // lambda
                if (!Entry) return {};
                return Entry->getResultsSync(Entry->path().wstring());
            },  // lambda end

            &entry  // lambda parameter
            ));
        requested_count++;
    }
    EXPECT_TRUE(requested_count == 4);

    DataBlock out;
    int delivered_count = 0;
    for (auto& r : results) {
        auto result = r.get();
        if (result.size()) {
            ++delivered_count;
            cma::tools::AddVector(out, result);
        }
    }
    EXPECT_TRUE(delivered_count == 3);

    int found_headers = 0;
    std::string_view str(out.data(), out.size());
    for (int i = 0; i < 3; ++i) {
        if (str.find(strings[i]) != std::string_view::npos) ++found_headers;
    }
    EXPECT_EQ(found_headers, 3);
}

static auto GenerateCachedHeader(const std::string& UsualHeader,
                                 const cma::PluginEntry* Ready) {
    std::vector<char> out;
    std::vector<char> in;
    cma::tools::AddVector(in, UsualHeader);
    auto ret = cma::HackPluginDataWithCacheInfo(out, in, Ready->legacyTime(),
                                                Ready->cacheAge());
    if (ret) {
        std::string str(out.data(), out.size());
        return str;
    }

    return std::string();
}

static auto ParsePluginOut(const std::vector<char>& Data) {
    std::string out(Data.begin(), Data.end());
    auto table = cma::tools::SplitString(out, "\n");
    auto sz = table.size();
    auto first_line = sz > 0 ? table[0] : "";
    auto second_line = sz > 1 ? table[1] : "";

    return std::make_tuple(sz, first_line, second_line);
}

const std::vector<std::string> strings = {
    "<<<async2>>>",  //
    "<<<async30>>>"  //
};

std::vector<cma::cfg::Plugins::ExeUnit> exe_units_invalid = {
    //        Async  Timeout CacheAge  Retry  Run
    // clang-format off
    {"*.cmd", true,  10,     0,        3,     true},
    {"*",     true,  10,     0,        3,     false},
    // clang-format off

};


std::vector<std::filesystem::path> as_files;

PathVector as_vp;

std::vector<cma::cfg::Plugins::ExeUnit> exe_units_valid = {
    //       Async  Timeout CacheAge              Retry  Run
    // clang-format off
    {"*.cmd", true, 10,     cma::cfg::kMinimumCacheAge + 1, 3,     true},
    {"*",     true, 10,     cma::cfg::kMinimumCacheAge + 1, 3,     false},
    // clang-format on
};

std::vector<cma::cfg::Plugins::ExeUnit> exe_units_valid_SYNC = {
    //       Async  Timeout CacheAge              Retry  Run
    // clang-format off
    {"*.cmd", false, 10,     0, 3,     true},
    {"*",     false, 10,     0, 3,     false},
    // clang-format on
};

void PrepareStructures() {
    std::filesystem::path temp_folder = cma::cfg::GetTempDir();
    as_vp.clear();
    struct PluginDesc {
        int timeout_;
        const char* file_name_;
        const char* section_name;
    } plugin_desc_arr[2] = {
        {2, "async2.cmd", "async2"},
        {30, "async30.cmd", "async30"},
    };

    for (auto& pd : plugin_desc_arr) {
        as_files.push_back(temp_folder / pd.file_name_);
        CreatePluginInTemp(as_files.back(), pd.timeout_, pd.section_name);
        as_vp.push_back(as_files.back());
    }
}

TEST(PluginTest, RemoveDuplicatedPlugins) {
    PluginMap x;
    RemoveDuplicatedPlugins(x, false);
    EXPECT_TRUE(x.size() == 0);

    x.emplace(std::make_pair("c:\\123\\a.bb", "c:\\123\\a.bb"));
    EXPECT_TRUE(x.size() == 1);
    RemoveDuplicatedPlugins(x, false);
    EXPECT_TRUE(x.size() == 1);
    x.emplace(std::make_pair("c:\\123\\aa.bb", "c:\\123\\aa.bb"));
    EXPECT_TRUE(x.size() == 2);
    RemoveDuplicatedPlugins(x, false);
    EXPECT_TRUE(x.size() == 2);

    x.emplace(std::make_pair("c:\\123\\another\\a.bb", "c:\\123\a.bb"));
    x.emplace(std::make_pair("c:\\123\\another\\aa.bb", "c:\\123\\aa.bb"));
    x.emplace(std::make_pair("c:\\123\\aa.bb", "c:\\123\\aa.bb"));
    x.emplace(std::make_pair("c:\\123\\yy.bb", "c:\\123\\aa.bb"));
    EXPECT_TRUE(x.size() == 5);
    RemoveDuplicatedPlugins(x, false);
    EXPECT_TRUE(x.size() == 3);
}

TEST(PluginTest, AsyncStartSimulation_Long) {
    using namespace cma::cfg;
    using namespace wtools;
    namespace fs = std::filesystem;
    using namespace std::chrono;
    cma::OnStart(cma::AppType::test);
    PrepareStructures();

    std::error_code ec;
    ON_OUT_OF_SCOPE(for (auto& f : as_vp) fs::remove(f, ec););

    PluginMap pm;  // load from the groups::plugin
    UpdatePluginMap(pm, false, as_vp, exe_units_invalid, false);

    // async part
    for (auto& entry_pair : pm) {
        auto& entry_name = entry_pair.first;
        auto& entry = entry_pair.second;
        EXPECT_EQ(entry.failures(), 0);
        EXPECT_EQ(entry.failed(), 0);

        auto accu = entry.getResultsAsync(true);
        EXPECT_EQ(true, accu.empty());
        EXPECT_TRUE(entry.running());
        entry.breakAsync();
    }

    UpdatePluginMap(pm, false, as_vp, exe_units_valid, false);

    // async part
    for (auto& entry_pair : pm) {
        auto& entry_name = entry_pair.first;
        auto& entry = entry_pair.second;
        EXPECT_EQ(entry.failures(), 0);
        EXPECT_EQ(entry.failed(), 0);

        auto accu = entry.getResultsAsync(true);
        EXPECT_EQ(true, accu.empty());
        EXPECT_TRUE(entry.running());
    }

    ::Sleep(5000);  // funny windows
    {
        auto ready = GetEntrySafe(pm, as_files[0].u8string());
        ASSERT_NE(nullptr, ready);
        auto accu = ready->getResultsAsync(true);

        // something in result and running
        ASSERT_TRUE(!accu.empty());
        auto expected_header = GenerateCachedHeader(strings[0], ready);
        {
            auto [sz, ln1, ln2] = ParsePluginOut(accu);
            EXPECT_EQ(sz, 2);
            EXPECT_EQ(ln1, expected_header);
            EXPECT_EQ(ln2, SecondLine);
        }
        EXPECT_FALSE(ready->running());  // NOT restarted by getResultAsync 121
                                         // sec cache age
    }

    {
        auto still_running = GetEntrySafe(pm, as_files[1].u8string());
        ASSERT_TRUE(nullptr != still_running);
        auto accu = still_running->getResultsAsync(true);

        // nothing but still running
        EXPECT_TRUE(accu.empty());
        EXPECT_TRUE(still_running->running());

        still_running->breakAsync();
        EXPECT_FALSE(still_running->running());
    }

    // pinging and restaring
    {
        auto ready = GetEntrySafe(pm, as_files[0].u8string());
        ASSERT_NE(nullptr, ready);
        auto accu1 = ready->getResultsAsync(true);
        ::Sleep(100);
        auto accu2 = ready->getResultsAsync(true);

        // something in result and running
        ASSERT_TRUE(!accu1.empty());
        ASSERT_TRUE(!accu2.empty());
        ASSERT_TRUE(accu1 == accu2);

        auto expected_header = GenerateCachedHeader(strings[0], ready);
        {
            auto [sz, ln1, ln2] = ParsePluginOut(accu1);
            EXPECT_EQ(sz, 2);
            EXPECT_EQ(ln1, expected_header);
            EXPECT_EQ(ln2, SecondLine);
        }
        {
            auto [sz, ln1, ln2] = ParsePluginOut(accu2);
            EXPECT_EQ(sz, 2);
            EXPECT_EQ(ln1, expected_header);
            EXPECT_EQ(ln2, SecondLine);
        }

        ready->breakAsync();
        EXPECT_FALSE(ready->running());

        // we have no more running process still we should get real data
        {
            auto accu_after_break = ready->getResultsAsync(true);
            ASSERT_TRUE(!accu_after_break.empty());
            ASSERT_TRUE(accu_after_break == accu2);
            EXPECT_FALSE(ready->running())
                << "should not run. Cache age is big enough\n";
        }

        ready->breakAsync();
        EXPECT_FALSE(ready->running());

        // we have no more running process still we should get real and good
        // data
        {
            auto accu_after_break = ready->getResultsAsync(false);
            ASSERT_TRUE(!accu_after_break.empty());
            ASSERT_TRUE(accu_after_break == accu2);
            EXPECT_FALSE(ready->running());
        }

        ::Sleep(5000);
        {
            auto accu_new = ready->getResultsAsync(false);
            ASSERT_TRUE(!accu_new.empty());
            EXPECT_EQ(accu_new, accu2)
                << "without RESTART and we have to have SAME data";
            auto expected_header_new = GenerateCachedHeader(strings[0], ready);
            {
                auto [sz, ln1, ln2] = ParsePluginOut(accu_new);
                EXPECT_EQ(sz, 2);
                EXPECT_EQ(ln1, expected_header_new);
                EXPECT_EQ(ln2, SecondLine);
            }

            // RESTART
            EXPECT_FALSE(ready->isGoingOld());  // not enough time to be old
            ready->restartAsyncThreadIfFinished(L"x");
            EXPECT_TRUE(ready->running());
            accu_new = ready->getResultsAsync(false);
            ASSERT_TRUE(!accu_new.empty());
            EXPECT_EQ(accu_new, accu2)
                << "IMMEDIATELY after RESTART and we have to have SAME data";
            expected_header_new = GenerateCachedHeader(strings[0], ready);
            {
                auto [sz, ln1, ln2] = ParsePluginOut(accu_new);
                EXPECT_EQ(sz, 2);
                EXPECT_EQ(ln1, expected_header_new);
                EXPECT_EQ(ln2, SecondLine);
            }
            ::Sleep(6000);
            accu_new = ready->getResultsAsync(false);
            ASSERT_TRUE(!accu_new.empty());
            EXPECT_NE(accu_new, accu2)
                << "late after RESTART and we have to have different data";
            expected_header_new = GenerateCachedHeader(strings[0], ready);
            {
                auto [sz, ln1, ln2] = ParsePluginOut(accu_new);
                EXPECT_EQ(sz, 2);
                EXPECT_EQ(ln1, expected_header_new);
                EXPECT_EQ(ln2, SecondLine);
            }
        }
    }

    // changing to local
    {
        auto ready = GetEntrySafe(pm, as_files[0].u8string());
        auto still = GetEntrySafe(pm, as_files[1].u8string());

        UpdatePluginMap(pm, true, as_vp, exe_units_valid, true);
        EXPECT_EQ(pm.size(), 2);
        EXPECT_TRUE(ready->local());
        EXPECT_TRUE(still->local());
    }

    // changing to sync
    {
        auto ready = GetEntrySafe(pm, as_files[0].u8string());
        EXPECT_FALSE(ready->data().empty());

        auto still = GetEntrySafe(pm, as_files[1].u8string());
        EXPECT_FALSE(ready->running()) << "timeout 10 secs expired";
        still->restartAsyncThreadIfFinished(L"Id");

        UpdatePluginMap(pm, false, as_vp, exe_units_valid_SYNC, true);
        EXPECT_EQ(pm.size(), 2);
        EXPECT_FALSE(ready->running());
        EXPECT_TRUE(ready->data().empty());

        EXPECT_FALSE(still->running());
        EXPECT_TRUE(still->data().empty());

        auto data = ready->getResultsAsync(true);
        EXPECT_TRUE(data.empty());
    }
    // changing to local again
    {
        auto ready = GetEntrySafe(pm, as_files[0].u8string());
        auto still = GetEntrySafe(pm, as_files[1].u8string());

        UpdatePluginMap(pm, true, as_vp, exe_units_valid, true);
        EXPECT_EQ(pm.size(), 2);
        EXPECT_TRUE(ready->local());
        EXPECT_TRUE(still->local());
        EXPECT_TRUE(ready->cacheAge() >= kMinimumCacheAge);
        EXPECT_TRUE(still->cacheAge() >= kMinimumCacheAge);

        auto data = ready->getResultsAsync(true);
        EXPECT_TRUE(data.empty());
        ::Sleep(5000);
        data = ready->getResultsAsync(true);
        EXPECT_TRUE(!data.empty());
        std::string out(data.begin(), data.end());
        auto table = cma::tools::SplitString(out, "\n");
        ASSERT_EQ(table.size(), 2);
        EXPECT_EQ(table[0], "<<<async2>>>");
    }

}  // namespace cma

TEST(PluginTest, SyncStartSimulation_Long) {
    using namespace cma::cfg;
    using namespace wtools;
    namespace fs = std::filesystem;
    using namespace std::chrono;
    cma::OnStart(cma::AppType::test);
    std::vector<Plugins::ExeUnit> exe_units = {
        //
        {"*.cmd", false, 10, 0, 3, true},  //
        {"*", true, 10, 0, 3, false},      //
    };

    fs::path temp_folder = cma::cfg::GetTempDir();

    PathVector vp = {
        (temp_folder / "a.cmd").u8string(),  //
        (temp_folder / "b.cmd").u8string(),  //
        (temp_folder / "c.cmd").u8string(),  //
        (temp_folder / "d.cmd").u8string(),  //
    };
    CreatePluginInTemp(vp[0], 5, "a");
    CreatePluginInTemp(vp[1], 0, "b");
    CreatePluginInTemp(vp[2], 3, "c");
    CreatePluginInTemp(vp[3], 120, "d");

    std::vector<std::string> strings = {
        "<<<a>>>",  //
        "<<<b>>>",  //
        "<<<c>>>",  //
        "<<<d>>>",  //
    };

    ON_OUT_OF_SCOPE(for (auto& f : vp) fs::remove(f););

    PluginMap pm;  // load from the groups::plugin
    UpdatePluginMap(pm, false, vp, exe_units, false);

    // retry count test
    {
        PluginMap pm_1;  // load from the groups::plugin
        PathVector vp_1 = {vp[3]};

        UpdatePluginMap(pm_1, false, vp_1, exe_units, false);
        auto f = pm_1.begin();
        auto& entry = f->second;

        for (int i = 0; i < entry.retry(); ++i) {
            auto accu = entry.getResultsSync(L"id", 0);
            EXPECT_TRUE(accu.empty());
            EXPECT_EQ(entry.failures(), i + 1);
            EXPECT_FALSE(entry.failed());
        }

        auto accu = entry.getResultsSync(L"id", 0);
        EXPECT_TRUE(accu.empty());
        EXPECT_EQ(entry.failures(), 4);
        EXPECT_TRUE(entry.failed());
    }

    // sync part
    for (auto& entry_pair : pm) {
        auto& entry_name = entry_pair.first;
        auto& entry = entry_pair.second;
        EXPECT_EQ(entry.failures(), 0);
        EXPECT_EQ(entry.failed(), 0);

        if (entry_name == vp[0]) {
            auto accu = entry.getResultsSync(L"id", 0);
            EXPECT_TRUE(accu.empty());  // wait precise 0 sec, nothing should be
                                        // presented
        }

        if (entry_name == vp[3]) {
            auto accu = entry.getResultsSync(L"id", 1);
            EXPECT_TRUE(accu.empty());  // wait precise 0 sec, nothing should be
                                        // presented
        }

        auto accu = entry.getResultsSync(L"id");

        if (vp[3] == entry_name) {
            EXPECT_EQ(true, accu.empty());
            EXPECT_EQ(entry.failures(), 2);
            EXPECT_EQ(entry.failed(), 0);
        } else {
            std::string result(accu.begin(), accu.end());
            EXPECT_TRUE(!accu.empty());
            accu.clear();
            auto table = cma::tools::SplitString(result, "\r\n");
            ASSERT_EQ(table.size(), 2);
            EXPECT_TRUE(table[0] == strings[0] ||  //
                        table[0] == strings[1] ||  //
                        table[0] == strings[2]);
            EXPECT_EQ(table[1], SecondLine);
        }
    }
}

}  // namespace cma
