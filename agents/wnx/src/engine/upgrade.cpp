// Windows Tools
#include "stdafx.h"

#include "upgrade.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include "cvt.h"
#include "glob_match.h"
#include "logger.h"
#include "tools/_misc.h"
#include "tools/_raii.h"
#include "tools/_xlog.h"
#include "yaml-cpp/yaml.h"

namespace cma::cfg::upgrade {

// SERVICE_AUTO_START : SERVICE_DISABLED
enum class ServiceStartType {
    disable = SERVICE_DISABLED,
    auto_start = SERVICE_AUTO_START

};

// returns false if folder cannot be created
[[nodiscard]] bool CreateFolderSmart(
    const std::filesystem::path& tgt) noexcept {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (cma::tools::IsValidRegularFile(tgt)) fs::remove(tgt, ec);
    if (fs::exists(tgt, ec)) return true;

    auto ret = fs::create_directories(tgt, ec);
    // check created or already exists
    if (ret || ec.value() == 0) return true;

    XLOG::l("Can't create '{}' error = [{}]", tgt.u8string(), ec.value());
    return false;
}

bool IsPathProgramData(const std::filesystem::path& program_data) {
    std::filesystem::path mask = kAppDataCompanyName;
    mask /= kAppDataAppName;
    std::wstring mask_str = mask.wstring();
    cma::tools::WideLower(mask_str);

    auto test_path = program_data.lexically_normal().native();
    cma::tools::WideLower(test_path);

    return test_path.find(mask_str) != std::wstring::npos;
}

[[nodiscard]] bool IsFileNonCompatible(
    const std::filesystem::path& fname) noexcept {
    constexpr std::string_view forbidden_files[] = {"cmk-update-agent.exe"};

    auto name = fname.filename();

    auto text = name.u8string();
    cma::tools::StringLower(text);

    return std::any_of(std::begin(forbidden_files), std::end(forbidden_files),
                       // predicate:
                       [text](std::string_view file) { return file == text; }

    );
}

int CopyAllFolders(const std::filesystem::path& legacy_root,
                   const std::filesystem::path& program_data,
                   CopyFolderMode copy_mode) {
    namespace fs = std::filesystem;
    if (!IsPathProgramData(program_data)) {
        XLOG::d(XLOG_FUNC + " '{}' is bad folder, copy is not possible",
                program_data.u8string());
        return 0;
    }

    const std::wstring_view folders[] = {L"config", L"plugins", L"local",
                                         L"mrpe",   L"state",   L"bin"};
    auto count = 0;
    for_each(folders, std::end(folders),

             [legacy_root, program_data, &count,
              copy_mode](std::wstring_view sub_folder) {
                 auto src = legacy_root / sub_folder;
                 auto tgt = program_data / sub_folder;
                 XLOG::l.t("Processing '{}':", src.u8string());  //
                 if (copy_mode == CopyFolderMode::remove_old)
                     fs::remove_all(tgt);
                 auto folder_exists = CreateFolderSmart(tgt);
                 if (!folder_exists) return;

                 if (IsFileNonCompatible(src)) {
                     XLOG::l.i("File '{}' is skipped as not compatible",
                               src.string());
                     return;
                 }

                 auto c = CopyFolderRecursive(
                     src, tgt, fs::copy_options::skip_existing, [](fs::path P) {
                         XLOG::l.i("\tCopy '{}'", P.u8string());
                         return true;
                     });
                 count += c;
             });
    return count;
}

namespace details {

constexpr const std::string_view ignored_exts[] = {".ini", ".exe", ".log",
                                                   ".tmp"};

constexpr const std::string_view ignored_names[] = {
    "plugins.cap",
};

// single point entry to determine that file is ignored
bool IsIgnoredFile(const std::filesystem::path& filename) {
    using namespace cma::tools;

    // check extension
    auto extension = filename.extension();
    auto extension_string = extension.u8string();
    StringLower(extension_string);

    for (auto ext : ignored_exts)
        if (ext == extension_string) return true;

    // check name
    auto fname = filename.filename().u8string();
    StringLower(fname);

    for (auto name : ignored_names)
        if (fname == name) return true;

    // check mask
    std::string mask = "uninstall_*.bat";
    if (GlobMatch(mask, fname)) return true;

    return false;
}
}  // namespace details

// copies all files from root, exception is ini and exe
// returns count of files copied
int CopyRootFolder(const std::filesystem::path& LegacyRoot,
                   const std::filesystem::path& ProgramData) {
    namespace fs = std::filesystem;
    using namespace cma::tools;
    using namespace cma::cfg;

    auto count = 0;
    std::error_code ec;
    for (const auto& dir_entry : fs::directory_iterator(LegacyRoot, ec)) {
        const auto& p = dir_entry.path();
        if (fs::is_directory(p, ec)) continue;

        if (details::IsIgnoredFile(p)) {
            XLOG::l.i("File '{}' in root folder '{}' is ignored", p.u8string(),
                      LegacyRoot.u8string());
            continue;
        }

        // Copy to the targetParentPath which we just created.
        fs::copy(p, ProgramData, fs::copy_options::skip_existing, ec);

        if (ec.value() == 0) {
            count++;
            continue;
        }

        XLOG::l("during copy from '{}' to '{}' error {}", p.u8string(),
                wtools::ConvertToUTF8(cma::cfg::GetUserDir()), ec.value());
    }

    return count;
}

// Recursively copies those files and folders from src to target which matches
// predicate, and overwrites existing files in target.
int CopyFolderRecursive(
    const std::filesystem::path& source, const std::filesystem::path& target,
    std::filesystem::copy_options copy_mode,
    const std::function<bool(std::filesystem::path)>& predicate) noexcept {
    namespace fs = std::filesystem;
    int count = 0;
    XLOG::l.t("Copy from '{}' to '{}'", source.u8string(), target.u8string());

    try {
        std::error_code ec;
        for (const auto& dir_entry :
             fs::recursive_directory_iterator(source, ec)) {
            const auto& p = dir_entry.path();
            if (predicate(p)) {
                // Create path in target, if not existing.
                const auto relative_src = fs::relative(p, source);
                const auto target_parent_path = target / relative_src;
                if (fs::is_directory(p)) {
                    fs::create_directories(target_parent_path, ec);
                    if (ec.value() != 0) {
                        XLOG::l("Failed create folder '{} error {}",
                                target_parent_path.u8string(), ec.value());
                        continue;
                    }
                } else {
                    // Copy to the targetParentPath which we just created.
                    auto ret =
                        fs::copy_file(p, target_parent_path, copy_mode, ec);
                    if (ec.value() == 0) {
                        if (ret) count++;
                        continue;
                    }
                    XLOG::l("during copy from '{}' to '{}' error {}",
                            p.u8string(), target_parent_path.u8string(),
                            ec.value());
                }
            }
        }
    } catch (std::exception& e) {
        XLOG::l("Exception during copy file {}", e.what());
    }

    return count;
}

int GetServiceStatus(SC_HANDLE ServiceHandle) {
    DWORD bytes_needed = 0;
    SERVICE_STATUS_PROCESS ssp;
    auto buffer = reinterpret_cast<LPBYTE>(&ssp);

    if (FALSE == QueryServiceStatusEx(ServiceHandle, SC_STATUS_PROCESS_INFO,
                                      buffer, sizeof(SERVICE_STATUS_PROCESS),
                                      &bytes_needed)) {
        XLOG::l("QueryServiceStatusEx failed [{}]", GetLastError());
        return -1;
    }
    return ssp.dwCurrentState;
}

uint32_t GetServiceHint(SC_HANDLE ServiceHandle) {
    DWORD bytes_needed = 0;
    SERVICE_STATUS_PROCESS ssp;
    auto buffer = reinterpret_cast<LPBYTE>(&ssp);

    if (FALSE == QueryServiceStatusEx(ServiceHandle, SC_STATUS_PROCESS_INFO,
                                      buffer, sizeof(SERVICE_STATUS_PROCESS),
                                      &bytes_needed)) {
        XLOG::l("QueryServiceStatusEx failed [{}]", GetLastError());
        return 0;
    }
    return ssp.dwWaitHint;
}

int SendServiceCommand(SC_HANDLE Handle, uint32_t Command) {
    SERVICE_STATUS_PROCESS ssp;
    if (FALSE == ::ControlService(Handle, Command,
                                  reinterpret_cast<LPSERVICE_STATUS>(&ssp))) {
        XLOG::l("ControlService command [{}] failed [{}]", Command,
                GetLastError());
        return -1;
    }
    return ssp.dwCurrentState;
}

std::tuple<SC_HANDLE, SC_HANDLE, DWORD> OpenServiceForControl(
    const std::wstring& service_name) {
    auto manager_handle =
        ::OpenSCManager(nullptr,                 // local computer
                        nullptr,                 // ServicesActive database
                        SC_MANAGER_ALL_ACCESS);  // full access rights

    if (nullptr == manager_handle) {
        auto error = ::GetLastError();
        XLOG::l("OpenSCManager failed [{}]", error);
        return {nullptr, nullptr, error};
    }

    // Get a handle to the service.

    auto handle =
        ::OpenService(manager_handle,        // SCM database
                      service_name.c_str(),  // name of service
                      SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS |
                          SERVICE_ENUMERATE_DEPENDENTS);

    if (nullptr == handle) {
        auto error = ::GetLastError();
        XLOG::l("OpenService '{}' failed [{}]",
                wtools::ConvertToUTF8(service_name), error);
        return {manager_handle, handle, error};
    }

    return {manager_handle, handle, 0};
}

int GetServiceStatusByName(const std::wstring& Name) {
    auto [manager_handle, handle, err] = OpenServiceForControl(Name);

    ON_OUT_OF_SCOPE(if (manager_handle) CloseServiceHandle(manager_handle));
    ON_OUT_OF_SCOPE(if (handle) CloseServiceHandle(handle));

    if (nullptr == handle) return err;

    return GetServiceStatus(handle);
}

// from MS MSDN
static uint32_t CalcDelay(SC_HANDLE handle) noexcept {
    auto hint = GetServiceHint(handle);
    // Do not wait longer than the wait hint. A good interval is
    // one-tenth of the wait hint but not less than 1 second
    // and not more than 10 seconds.
    auto delay = hint / 10;
    if (delay < 1000)
        delay = 1000;
    else if (delay > 10000)
        delay = 10000;
    return delay;
}

// internal function based om MS logic from the MSDN, and the logic is not a
// so good as for 2019
static bool TryStopService(SC_HANDLE handle, const std::string& name_to_log,
                           DWORD current_status) noexcept {
    auto status = current_status;
    auto delay = CalcDelay(handle);
    constexpr DWORD timeout = 30000;  // 30-second time-out
    DWORD start_time = GetTickCount();
    // If a stop is pending, wait for it.
    if (status == SERVICE_STOP_PENDING) {
        XLOG::l.i("Service stop pending...");

        while (status == SERVICE_STOP_PENDING) {
            cma::tools::sleep(delay);

            status = GetServiceStatus(handle);

            if (status == -1) return false;

            if (status == SERVICE_STOPPED) {
                XLOG::l.i("Service '{}' stopped successfully.", name_to_log);
                return true;
            }

            if (GetTickCount() - start_time > timeout) {
                XLOG::l("Service stop timed out during pending");
                return false;
            }
        }
    }

    // Send a stop code to the service.
    status = SendServiceCommand(handle, SERVICE_CONTROL_STOP);

    if (status == -1) return false;

    // Wait for the service to stop.

    while (status != SERVICE_STOPPED) {
        cma::tools::sleep(delay);

        status = GetServiceStatus(handle);
        if (status == -1) return false;

        if (GetTickCount() - start_time > timeout) {
            XLOG::l("Wait timed out for '{}'", name_to_log);
            return false;
        }
    }

    XLOG::l.i("Service '{}' really stopped", name_to_log);

    return true;
}

bool StopWindowsService(const std::wstring& service_name) {
    auto name_to_log = wtools::ConvertToUTF8(service_name);
    XLOG::l.t("Service {} stopping ...", name_to_log);

    // Get a handle to the SCM database.
    auto [manager_handle, handle, error] = OpenServiceForControl(service_name);
    ON_OUT_OF_SCOPE(if (manager_handle) CloseServiceHandle(manager_handle));
    ON_OUT_OF_SCOPE(if (handle) CloseServiceHandle(handle));
    if (nullptr == handle) {
        XLOG::l("Cannot open service '{}' with error [{}]", name_to_log, error);
        return false;
    }

    // Make sure the service is not already stopped.
    auto status = GetServiceStatus(handle);
    if (status == -1) return false;

    if (status == SERVICE_STOPPED) {
        XLOG::l.i("Service '{}' is already stopped.", name_to_log);
        return true;
    }

    return TryStopService(handle, name_to_log, status);
}

static void LogStartStatus(const std::wstring& service_name,
                           DWORD last_error_code) {
    auto name = wtools::ConvertToUTF8(service_name);
    if (last_error_code == 0) {
        XLOG::l.i("Service '{}' started successfully ", name);
        return;
    }

    if (last_error_code == 1056) {
        XLOG::l.t("Service '{}' already started [1056]", name);
        return;
    }
    XLOG::l("Service '{}' start failed [{}]", name, last_error_code);
}

bool StartWindowsService(const std::wstring& service_name) {
    // Get a handle to the SCM database.
    auto [manager_handle, handle, error] = OpenServiceForControl(service_name);
    ON_OUT_OF_SCOPE(if (manager_handle) CloseServiceHandle(manager_handle));
    ON_OUT_OF_SCOPE(if (handle) CloseServiceHandle(handle));

    if (nullptr == handle) {
        XLOG::l("Cannot open service '{}' with error [{}]",
                wtools::ConvertToUTF8(service_name), error);
        return false;
    }

    // Make sure the service is not already started
    auto status = GetServiceStatus(handle);
    if (status == -1) return false;  // trash

    if (status == SERVICE_RUNNING) {
        XLOG::l.i("Service is already running.");
        return true;
    }

    if (status != SERVICE_STOPPED) {
        XLOG::l.i(
            "Service is in strange mode = [{}]. This is not a problem, just Windows Feature",
            status);
        // use hammer
        wtools::KillProcessFully(service_name + L".exe", 1);
    }

    // Send a start code to the service.
    auto ret = ::StartService(handle, 0, nullptr);
    LogStartStatus(service_name, ret == TRUE ? 0 : ::GetLastError());

    return true;
}

bool WinServiceChangeStartType(const std::wstring Name, ServiceStartType Type) {
    auto manager_handle = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (nullptr == manager_handle) {
        XLOG::l.crit("Cannot open SC MAnager {}", GetLastError());
        return false;
    }
    ON_OUT_OF_SCOPE(CloseServiceHandle(manager_handle));

    auto handle =
        OpenService(manager_handle, Name.c_str(), SERVICE_CHANGE_CONFIG);
    if (nullptr == handle) {
        XLOG::l.crit("Cannot open Service {}, error =  {}",
                     wtools::ConvertToUTF8(Name), GetLastError());
        return false;
    }
    ON_OUT_OF_SCOPE(CloseServiceHandle(handle));

    auto result =
        ChangeServiceConfig(handle,             // handle of service
                            SERVICE_NO_CHANGE,  // service type: no change
                            static_cast<DWORD>(Type),  // service start type
                            SERVICE_NO_CHANGE,  // error control: no change
                            nullptr,            // binary path: no change
                            nullptr,            // load order group: no change
                            nullptr,            // tag ID: no change
                            nullptr,            // dependencies: no change
                            nullptr,            // account name: no change
                            nullptr,            // password: no change
                            nullptr);           // display name: no change
    if (0 == result) {
        XLOG::l("ChangeServiceConfig '{}' failed [{}]",
                wtools::ConvertToUTF8(Name), GetLastError());
        return false;
    }

    return true;
}

std::wstring FindLegacyAgent() {
    namespace fs = std::filesystem;
    auto image_path = wtools::GetRegistryValue(
        L"SYSTEM\\CurrentControlSet\\Services\\check_mk_agent", L"ImagePath",
        L"");

    if (image_path.empty()) return {};

    // remove double quotes
    if (image_path.back() == L'\"') image_path.pop_back();
    auto path = image_path.c_str();
    if (*path == L'\"') ++path;

    fs::path p = path;
    if (!cma::tools::IsValidRegularFile(p)) {
        XLOG::d(
            "Agent is found in registry '{}', but absent on the disk."
            "Assuming that agent is installed",
            p.u8string());
        return {};
    }

    return p.parent_path().wstring();
}

bool IsLegacyAgentActive() {
    auto path = FindLegacyAgent();
    if (path.empty()) return false;

    namespace fs = std::filesystem;
    auto service_type = wtools::GetRegistryValue(
        L"SYSTEM\\CurrentControlSet\\Services\\check_mk_agent", L"StartType",
        SERVICE_DISABLED);
    return service_type != SERVICE_DISABLED;
}

bool ActivateLegacyAgent() {
    wtools::SetRegistryValue(
        L"SYSTEM\\CurrentControlSet\\Services\\check_mk_agent", L"StartType",
        SERVICE_AUTO_START);
    return WinServiceChangeStartType(L"check_mk_agent",
                                     ServiceStartType::auto_start);
}
bool DeactivateLegacyAgent() {
    wtools::SetRegistryValue(
        L"SYSTEM\\CurrentControlSet\\Services\\check_mk_agent", L"StartType",
        SERVICE_DISABLED);
    return WinServiceChangeStartType(L"check_mk_agent",
                                     ServiceStartType::disable);
}

int WaitForStatus(std::function<int(const std::wstring&)> StatusChecker,
                  const std::wstring& ServiceName, int ExpectedStatus,
                  int Time) {
    int status = -1;
    while (true) {
        status = StatusChecker(ServiceName);
        if (status == ExpectedStatus) return status;
        if (Time >= 0) {
            cma::tools::sleep(1000);
            XLOG::l.i("1 second is over status is {}, t=required {}...", status,
                      ExpectedStatus);
        } else
            break;
        Time -= 1000;
    }

    return status;
}

static void LogAndDisplayErrorMessage(int status) {
    auto driver_body =
        cma::cfg::details::FindServiceImagePath(L"winring0_1_2_0");

    using namespace xlog::internal;
    if (!driver_body.empty()) {
        xlog::sendStringToStdio("Probably you have : ", Colors::kGreen);
        XLOG::l.crit("Failed to stop kernel legacy driver winring0_1_2_0 [{}]",
                     status);
        return;
    }

    if (status == SERVICE_STOP_PENDING) {
        XLOG::l.crit(
            "Can't stop windows kernel driver 'winring0_1_2_0', integral part of Open Hardware Monitor\n"
            "'winring0_1_2_0' registry entry is absent, but driver is running having 'SERVICE_STOP_PENDING' state\n"
            "THIS IS ABNORMAL. You must REBOOT Windows. And repeat action.");
        return;
    }

    // this may be ok
    xlog::sendStringToStdio("This is just info: ", Colors::kGreen);
    XLOG::l.w(
        "Can't stop winring0_1_2_0 [{}], probably you have no 'Open Hardware Monitor' running.",
        status);
}

bool FindStopDeactivateLegacyAgent() {
    XLOG::l.t("Find, stop and deactivate");
    if (!cma::tools::win::IsElevated()) {
        XLOG::l(
            "You have to be in elevated to use this function.\nPlease, run as Administrator");
        return false;
    }
    namespace fs = std::filesystem;
    auto path = FindLegacyAgent();
    if (path.empty()) {
        XLOG::l.t("There is no legacy Check Mk agent installed");
        return true;
    }

    XLOG::l.t("Stopping check_mk_agent...");
    auto ret = StopWindowsService(L"check_mk_agent");
    if (!ret) {
        XLOG::l.crit("Failed to stop check_mk_agent");
        if (!wtools::KillProcessFully(L"check_mk_agent.exe", 9)) return false;
    }

    XLOG::l.t("Checking check_mk_agent status...");
    auto status = GetServiceStatusByName(L"check_mk_agent");
    if (status != SERVICE_STOPPED) {
        XLOG::l.crit("Wrong status of check_mk_agent {}", status);
        return false;
    }

    XLOG::l.t("Deactivate check_mk_agent ...");
    DeactivateLegacyAgent();
    if (IsLegacyAgentActive()) {
        XLOG::l.crit("Failed to deactivate check_mk_agent");
        return false;
    }

    XLOG::l.t("Killing open hardware monitor...");
    wtools::KillProcess(L"Openhardwaremonitorcli.exe", 1);
    wtools::KillProcess(L"Openhardwaremonitorcli.exe",
                        1);  // we may have two :)

    XLOG::l.t("Stopping winring0_1_2_0...");
    StopWindowsService(L"winring0_1_2_0");
    status = WaitForStatus(GetServiceStatusByName, L"WinRing0_1_2_0",
                           SERVICE_STOPPED, 5000);

    if (status == SERVICE_STOPPED) return true;
    if (status == 1060) return true;  // case when driver killed by OHM

    // below we have variants when damned OHM kill and remove damned
    // driver before we have a chance to check its stop
    if (status == 1060) return true;
    if (status == -1) return true;

    LogAndDisplayErrorMessage(status);

    return false;
}

static bool RunOhm(const std::filesystem::path& lwa_path) noexcept {
    namespace fs = std::filesystem;
    fs::path ohm = lwa_path;
    ohm /= "bin";
    ohm /= "OpenHardwareMonitorCLI.exe";
    std::error_code ec;
    if (!fs::exists(ohm, ec)) {
        XLOG::l.crit(
            "OpenHardwareMonitor not installed,"
            "please, add it to the Legacy Agent folder");
        return false;
    }

    XLOG::l.t("Starting open hardware monitor...");
    RunDetachedProcess(ohm.wstring());
    WaitForStatus(GetServiceStatusByName, L"WinRing0_1_2_0", SERVICE_RUNNING,
                  5000);
    return true;
}

bool FindActivateStartLegacyAgent(AddAction action) {
    XLOG::l.t("Find, activate and start");
    namespace fs = std::filesystem;
    if (!cma::tools::win::IsElevated()) {
        XLOG::l(
            "You have to be in elevated to use this function.\nPlease, run as Administrator");
        return false;
    }

    auto path = FindLegacyAgent();
    if (path.empty()) {
        XLOG::l.t("There is no legacy Check Mk agent installed");
        return true;
    }

    XLOG::l.t("Activating check_mk_agent...");
    ActivateLegacyAgent();
    if (!IsLegacyAgentActive()) {
        XLOG::l.crit("Failed to Activate check_mk_agent");
        return false;
    }

    XLOG::l.t("Starting check_mk_agent...");
    auto ret = StartWindowsService(L"check_mk_agent");
    if (!ret) {
        XLOG::l.crit("Failed to stop check_mk_agent");
        return false;
    }

    XLOG::l.t("Checking check_mk_agent...");
    auto status = WaitForStatus(GetServiceStatusByName, L"check_mk_agent",
                                SERVICE_RUNNING, 5000);
    if (status != SERVICE_RUNNING) {
        XLOG::l.crit("Wrong status of check_mk_agent {}", status);
        return false;
    }

    // mostly for test and security
    if (action == AddAction::start_ohm) RunOhm(path);

    return true;
}

bool RunDetachedProcess(const std::wstring& Name) {
    // start process
    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    std::wstring name = Name;
    auto windows_name = const_cast<LPWSTR>(name.c_str());

    auto ret = CreateProcessW(
        nullptr,       // application name
        windows_name,  // Command line options
        nullptr,       // Process handle not inheritable
        nullptr,       // Thread handle not inheritable
        FALSE,         // Set handle inheritance to FALSE
        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,  // No creation flags
        nullptr,  // Use parent's environment block
        nullptr,  // Use parent's starting directory
        &si,      // Pointer to STARTUPINFO structure
        &pi);     // Pointer to PROCESS_INFORMATION structure
    if (ret != TRUE) {
        XLOG::l("Cant start the process {}, error is {}",
                wtools::ConvertToUTF8(Name), GetLastError());
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return ret == TRUE;
}

std::string GetTimeString() {
    using namespace std::chrono;
    auto cur_time = system_clock::now();
    auto in_time_t = system_clock::to_time_t(cur_time);
    std::stringstream sss;
    auto ms = duration_cast<milliseconds>(cur_time.time_since_epoch()) % 1000;
    auto loc_time = std::localtime(&in_time_t);
    auto p_time = std::put_time(loc_time, "%Y-%m-%d %T");
    sss << p_time << "." << std::setfill('0') << std::setw(3) << ms.count()
        << std::ends;

    return sss.str();
}

bool CreateProtocolFile(std::filesystem::path& ProtocolFile,
                        std::string_view OptionalContent) {
    try {
        std::ofstream ofs(ProtocolFile, std::ios::binary);

        if (ofs) {
            ofs << "Upgraded:\n";
            ofs << "  time: '" << GetTimeString() << "'\n";
            if (!OptionalContent.empty()) {
                ofs << OptionalContent;
                ofs << "\n";
            }
        }
    } catch (const std::exception& e) {
        XLOG::l.crit("Exception during creatin protocol file {}", e.what());
        return false;
    }
    return true;
}

// The only API entry DIRECTLY used in production
bool UpgradeLegacy(Force force_upgrade) {
    XLOG::l.i("Starting upgrade(migration) process...");
    if (!cma::tools::win::IsElevated()) {
        XLOG::l(
            "You have to be in elevated to use this function.\nPlease, run as Administrator");
        return false;
    }

    bool force = Force::yes == force_upgrade;

    if (force) {
        XLOG::SendStringToStdio(
            "Upgrade(migration) is forced by command line\n",
            XLOG::Colors::kYellow);
    }

    namespace fs = std::filesystem;
    fs::path protocol_file = cma::cfg::GetRootDir();
    protocol_file /= cma::cfg::files::kUpgradeProtocol;
    std::error_code ec;

    if (fs::exists(protocol_file, ec) && !force) {
        XLOG::l.i("Protocol File '{}' exists, upgrade(migration) not required",
                  protocol_file.u8string());
        return false;
    }

    auto path = FindLegacyAgent();
    if (path.empty()) {
        XLOG::l.t("Legacy Agent not found Upgrade is not possible");
        return true;
    }
    XLOG::l.i("Legacy Agent is found in '{}'", wtools::ConvertToUTF8(path));

    auto success = FindStopDeactivateLegacyAgent();
    if (!success) {
        XLOG::l("Legacy Agent is not possible to stop");
    }

    auto count =
        CopyAllFolders(path, cma::cfg::GetUserDir(), CopyFolderMode::keep_old);

    count += CopyRootFolder(path, cma::cfg::GetUserDir());

    XLOG::l.i("Converting ini file");
    ConvertIniFiles(path, cma::cfg::GetUserDir());

    XLOG::l.i("Saving protocol file");
    // making protocol file:
    CreateProtocolFile(protocol_file, {});

    return true;
}

std::optional<YAML::Node> LoadIni(std::filesystem::path File) {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(File, ec)) {
        XLOG::l.i("File not found '{}', this is may be ok", File.u8string());
        return {};
    }
    if (!fs::is_regular_file(File, ec)) {
        XLOG::l.w("File '{}' is not a regular file, this is wrong",
                  File.u8string());
        return {};
    }

    cma::cfg::cvt::Parser p;
    p.prepare();
    if (!p.readIni(File, false)) {
        XLOG::l.e("File '{}' is not a valid INI file, this is wrong",
                  File.u8string());
        return {};
    }

    return p.emitYaml();
}

bool ConvertLocalIniFile(const std::filesystem::path& LegacyRoot,
                         const std::filesystem::path& ProgramData) {
    namespace fs = std::filesystem;
    const std::string local_ini = "check_mk_local.ini";
    auto local_ini_file = LegacyRoot / local_ini;
    std::error_code ec;
    if (fs::exists(local_ini_file, ec)) {
        XLOG::l.i("Converting local ini file '{}'", local_ini_file.u8string());
        auto user_yaml_file =
            wtools::ConvertToUTF8(files::kDefaultMainConfigName);

        auto out_file = CreateYamlFromIniSmart(
            local_ini_file, ProgramData, user_yaml_file, CfgFileType::user);
        if (!out_file.empty() && fs::exists(out_file, ec)) {
            XLOG::l.i("Local File '{}' was converted as user YML file '{}'",
                      local_ini_file.u8string(), out_file.u8string());
            return true;
        }
    }

    XLOG::l.t("Local ini File is absent or has no data",
              local_ini_file.u8string());
    return false;
}

bool ConvertUserIniFile(const std::filesystem::path& LegacyRoot,
                        const std::filesystem::path& ProgramData,
                        bool LocalFileExists) {
    namespace fs = std::filesystem;

    const std::string root_ini = "check_mk.ini";
    auto user_ini_file = LegacyRoot / root_ini;

    std::error_code ec;
    if (!fs::exists(user_ini_file, ec)) {
        XLOG::l.i("User ini File {} is absent", user_ini_file.u8string());
        return false;
    }

    // check_mk.user.yml or check_mk.bakery.yml
    const std::wstring name = files::kDefaultMainConfigName;

    // generate
    auto out_folder = ProgramData;

    // if local.ini file exists, then second file must be a bakery file(pure
    // logic)
    auto mode = LocalFileExists ? CfgFileType::bakery : CfgFileType::automatic;
    auto yaml_file = CreateYamlFromIniSmart(user_ini_file, out_folder,
                                            wtools::ConvertToUTF8(name), mode);
    // check
    if (!yaml_file.empty() && fs::exists(yaml_file, ec)) {
        XLOG::l.t("User ini File {} was converted to YML file {}",
                  user_ini_file.u8string(), yaml_file.u8string());
        return true;
    }

    XLOG::l.w("User ini File {} has no useful data", user_ini_file.u8string());
    return false;
}

// intermediate API, used indirectly
bool ConvertIniFiles(const std::filesystem::path& legacy_root,
                     const std::filesystem::path& program_data) {
    namespace fs = std::filesystem;
    using namespace cma::cfg;

    {
        std::error_code ec;
        auto bakery_ini =
            (program_data / dirs::kBakery / files::kDefaultMainConfig)
                .replace_extension(files::kDefaultBakeryExt);
        XLOG::l.t("Removing '{}'", bakery_ini.u8string());
        fs::remove(bakery_ini, ec);
    }

    bool local_file_exists = ConvertLocalIniFile(legacy_root, program_data);

    auto user_or_bakery_exists =
        ConvertUserIniFile(legacy_root, program_data, local_file_exists);

    return local_file_exists || user_or_bakery_exists;
}

// read first line and check for a marker
bool IsBakeryIni(const std::filesystem::path& Path) noexcept {
    if (!cma::tools::IsValidRegularFile(Path)) return false;

    try {
        std::ifstream ifs(Path, std::ios::binary);
        if (!ifs) return false;

        char buffer[kBakeryMarker.size()];
        ifs.read(buffer, sizeof(buffer));
        if (!ifs) return false;
        return 0 == memcmp(buffer, kBakeryMarker.data(), sizeof(buffer));

    } catch (const std::exception& e) {
        XLOG::l(XLOG_FLINE + " Exception {}", e.what());
        return false;
    }
}

std::string MakeComments(const std::filesystem::path& source_file_path,
                         bool file_from_bakery) noexcept {
    return fmt::format(
        "# Converted to YML from the file '{}'\n"
        "{}\n",
        source_file_path.u8string(),
        file_from_bakery ? "# original INI file was managed by WATO\n"
                         : "# original INI file was managed by user\n");
}

bool StoreYaml(const std::filesystem::path& filename, YAML::Node yaml_node,
               const std::string& comment) noexcept {
    std::ofstream ofs(
        filename);  // text mode, required to have normal carriage return
    if (ofs) {
        ofs << comment;
        ofs << yaml_node;
    }

    return true;
}

std::filesystem::path CreateYamlFromIniSmart(
    const std::filesystem::path& ini_file,      // ini file to use
    const std::filesystem::path& program_data,  // directory to send
    const std::string& yaml_name,               // name to be used in output
    CfgFileType cfg_file_type) noexcept {       // hard bakery or soft

    namespace fs = std::filesystem;

    // conversion
    auto yaml = LoadIni(ini_file);
    if (!yaml.has_value() || !yaml.value().IsMap()) {
        XLOG::l.w("File '{}' is empty, no yaml created", ini_file.u8string());
        return {};
    }

    // storing
    auto bakery_file =
        (cfg_file_type != CfgFileType::user) &&
        (cfg_file_type == CfgFileType::bakery || IsBakeryIni(ini_file));

    auto comments = MakeComments(ini_file, bakery_file);
    auto yaml_file = program_data;
    if (bakery_file) yaml_file /= dirs::kBakery;

    std::error_code ec;
    if (!fs::exists(yaml_file, ec)) {
        fs::create_directories(yaml_file, ec);
    }

    yaml_file /= yaml_name;
    yaml_file.replace_extension(bakery_file ? files::kDefaultBakeryExt
                                            : files::kDefaultUserExt);

    StoreYaml(yaml_file, yaml.value(), comments);
    XLOG::l.i("File '{}' is successfully converted", ini_file.u8string());

    return yaml_file;
}

}  // namespace cma::cfg::upgrade
