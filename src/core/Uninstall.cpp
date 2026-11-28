// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "core/Uninstall.h"
#include "core/Release.h"
#include "host/Guest.h"
#include "bluetooth/Log.h"
#include "biometrics/Protocol.h"
#include "bluetooth/Bridge.h"
#include "host/Host.h"
#include "install/Package.h"
#include "install/Setup.h"
#include "install/Traces.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <Windows.h>
#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

namespace WN::Core {
namespace fs = std::filesystem;
namespace {

// Added by Windows to a folder it registers a package from (resources compiled for the package).
constexpr wchar_t kPackageMetadata[] = L"microsoft.system.package.metadata";

fs::path Normalized(const fs::path& folder) {
    std::error_code ec;
    fs::path p = fs::absolute(folder, ec).lexically_normal();
    if (!p.has_filename() && p.has_relative_path()) p = p.parent_path();  // "...\WSA-Next\" -> "...\WSA-Next"
    return p;
}

bool IsWsaNext(const Install::PackageInfo& package) {
    std::error_code ec;
    return package.developmentMode && fs::exists(package.location / Config::kStateFile, ec);
}

// "...\WSA-Next" holding the WSA package manifest: a folder WSA-Next created for itself.
bool OwnFolder(const fs::path& folder) {
    if (_wcsicmp(folder.filename().c_str(), Config::kInstallFolder) != 0) return false;
    std::ifstream in(folder / Config::kPackageManifest, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str().find(std::format("Name=\"{}\"", ToUtf8(Config::kPackageName))) != std::string::npos;
}

// A name directly inside the folder. wsa-next.json can be edited by whoever may write the folder,
// and the removal runs elevated: no paths, no "..".
bool PlainName(std::wstring_view name) {
    return !name.empty() && name != L"." && name != L".." && name.find_first_of(L"\\/:") == std::wstring_view::npos;
}

// A running executable cannot be deleted: when `path` is this program (the installed copy runs
// the removal), it goes at the next restart.
bool ScheduleIfSelf(const fs::path& path) {
    std::error_code ec;
    return fs::equivalent(path, fs::path(ModulePath()), ec) && MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

std::wstring Join(const std::vector<std::wstring>& items) {
    std::wstring out;
    for (const auto& item : items) out += (out.empty() ? L"" : L", ") + item;
    return out;
}

// The install folder: what the installer wrote into it (wsa-next.json lists it), what Windows adds
// to a registered package folder, the downloads; then the folder itself when nothing else is left.
// Installations from before the list existed are removed whole when the folder is WSA-Next's own.
// `setupOwnsProgram`: WSA-Next Setup installed WSA-Next.exe and its language files, and Windows
// Installer removes them (and the folder) after this.
void RemoveFolder(const fs::path& folder, const InstalledState& state, const Callbacks& cb, RemovalOutcome& outcome, bool setupOwnsProgram) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return;
    std::set<std::wstring> names;
    for (const auto& [id, list] : state.entries) names.insert(list.begin(), list.end());
    if (!state.EntriesComplete()) {
        if (OwnFolder(folder)) {
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) names.insert(it->path().filename().wstring());
        } else {
            cb.log(Level::Warning, Text::Format(IDS_LOG_FOLDER_UNKNOWN, folder.wstring()));
        }
    }
    std::set<std::wstring> all{Config::kStateFile, std::wstring(Config::kStateFile) + L".new", Config::kDownloadFolder, kPackageMetadata};
    for (const auto& name : names) {
        all.insert(name);
        all.insert(name + L".new");  // left by an interrupted unpacking
    }
    if (setupOwnsProgram) {
        for (const wchar_t* program : {Config::kExeName, L"language"}) {
            all.erase(program);
            all.erase(std::wstring(program) + L".new");
        }
    }
    const std::uint64_t before = Install::FolderSize(folder);
    bool selfScheduled = false;
    for (const auto& name : all) {
        if (!PlainName(name)) continue;
        const fs::path path = folder / name;
        if (ScheduleIfSelf(path)) {
            selfScheduled = true;
        } else if (auto r = Install::DeleteWithRetry(path); !r) {
            outcome.leftovers.push_back(path.wstring());
            cb.log(Level::Warning, r.error());
        }
    }
    const std::uint64_t after = Install::FolderSize(folder);
    cb.log(Level::Success, Text::Format(IDS_LOG_FILES_REMOVED, folder.wstring(), FormatBytes(before - std::min(before, after))));
    if (selfScheduled) {
        MoveFileExW(folder.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);  // empty by then
    } else if (!RemoveDirectoryW(folder.c_str()) && fs::exists(folder, ec) &&
               !(setupOwnsProgram && fs::equivalent(folder, HostFolder(), ec))) {
        cb.log(Level::Warning, Text::Format(IDS_LOG_FOLDER_KEPT, folder.wstring()));
    }
}

// The background host's copy in %ProgramFiles%\WSA-Next when Android was installed elsewhere
// (otherwise the install folder's list holds it): WSA-Next.exe, its language files, the folder
// when nothing else is in it.
void RemoveHostCopy(const fs::path& folder, const Callbacks& cb, RemovalOutcome& outcome) {
    const fs::path host = HostFolder();
    std::error_code ec;
    if (fs::equivalent(Normalized(folder), host, ec) || !fs::exists(host / Config::kExeName, ec)) return;
    const fs::path exe = host / Config::kExeName;
    const bool selfScheduled = ScheduleIfSelf(exe);
    if (!selfScheduled) {
        if (auto r = Install::DeleteWithRetry(exe); !r) {
            outcome.leftovers.push_back(exe.wstring());
            cb.log(Level::Warning, r.error());
        }
    }
    const fs::path languages = host / L"language";
    for (fs::directory_iterator it(languages, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == L".lng") DeleteFileW(it->path().c_str());
    RemoveDirectoryW(languages.c_str());
    if (selfScheduled) MoveFileExW(host.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    else RemoveDirectoryW(host.c_str());
}

// When its package is removed, WSA deletes the Start menu shortcuts, "Installed apps" entries,
// associations and icons of the Android apps, but keeps the marker that says it has created them
// ("pl-PL;" - the languages they were created for). With the Android data kept, the next
// installation would start without any of them. Without the marker WSA creates them again on its
// first start.
void ForgetPopulatedApps(const fs::path& data) {
    std::error_code ec;
    fs::remove(data / L"LocalState" / L"language_list_for_populated_apps", ec);
}

// HKCU\Software\KVC\WSA-Next, and HKCU\Software\KVC when no other KVC tool keeps anything in it.
void RemoveSettings(const Callbacks& cb) {
    Release::ForgetDigests();  // the background host's digest cache (HKLM)
    if (RegDeleteTreeW(HKEY_CURRENT_USER, Config::kRegKey) == ERROR_SUCCESS) {
        RegDeleteKeyW(HKEY_CURRENT_USER, Config::kRegKey);
        cb.log(Level::Success, Text::Format(IDS_LOG_SETTINGS_REMOVED, Config::kRegKey));
    }
    RegKey parent;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, Config::kRegParent, 0, KEY_READ, parent.put()) != ERROR_SUCCESS) return;
    DWORD keys = 1, values = 1;
    RegQueryInfoKeyW(parent.get(), nullptr, nullptr, nullptr, &keys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    parent.reset();
    if (keys == 0 && values == 0) RegDeleteKeyW(HKEY_CURRENT_USER, Config::kRegParent);
}

}  // namespace

bool Removable(const fs::path& folder, const SystemInfo& system) {
    std::error_code ec;
    return (system.package && IsWsaNext(*system.package)) || fs::exists(folder / Config::kStateFile, ec);
}

Result<RemovalOutcome> Uninstall(const fs::path& requested, Removal mode, const Callbacks& cb) {
    RemovalOutcome outcome;
    const bool everything = mode == Removal::Everything;
    auto log = [&](Level level, std::wstring text) { cb.log(level, std::move(text)); };
    auto info = [&](std::wstring text) { cb.log(Level::Info, std::move(text)); };
    auto warn = [&](std::wstring text) {
        outcome.leftovers.push_back(text);
        cb.log(Level::Warning, std::move(text));
    };
    auto phase = [&](UINT text, int permille = -1) {
        Progress p;
        p.phase = Text::Get(text);
        p.permille = permille;
        cb.progress(p);
    };

    phase(IDS_PHASE_CHECKING);
    std::error_code ec;
    fs::path folder = Normalized(requested);
    const auto package = Install::FindWsa();
    if (package && IsWsaNext(*package)) folder = Normalized(package->location);  // the installation that is registered
    const bool ours = package && package->developmentMode && fs::equivalent(package->location, folder, ec);
    const InstalledState state = ReadState(folder);
    const bool files = fs::exists(folder / Config::kStateFile, ec);
    if (!ours && !files && !everything) return Fail(Text::Format(IDS_UNINSTALL_NOTHING, folder.wstring()));
    if (package && !ours) log(Level::Warning, Text::Format(IDS_LOG_FOREIGN_PACKAGE, package->version, package->location.wstring()));
    // WSA registered from this folder for another Windows user (Windows lets one user at a time
    // register an unpacked package): its files, its host's logon task and what it needs in Windows
    // stay - removing them would leave that user a registration without files. That user removes
    // it on their own account. (WSA-Next Setup still removes its program: their WSA keeps running.)
    std::optional<Install::OtherRegistration> other;
    if (!ours) {
        other = Install::FindWsaOfOtherUser();
        if (other && !other->location.empty() && !fs::equivalent(Normalized(other->location), folder, ec)) other.reset();
        if (other) log(Level::Warning, Text::Format(IDS_LOG_OTHER_USER_KEPT, other->user, folder.wstring()));
    }

    // 1. The background host (Bluetooth and Windows Hello for Android) runs WSA-Next.exe from the
    //    install folder: it stops, and stops starting at logon (also the task name of the first
    //    test builds), in both ways of removing.
    const bool hostWasRunning = Host::Running(), hostWasAutostart = Host::AutostartEnabled();
    Host::Stop();
    if (!other) {
        if (auto r = Host::SetAutostart(false); !r && hostWasAutostart) warn(r.error());
    }

    // 2. The package. WSA is shut down first: its disks are about to go.
    const fs::path data = Install::PackageDataFolder();
    if (ours) {
        phase(IDS_PHASE_UNREGISTER);
        Install::Shutdown(folder, info);
        if (auto r = Install::Unregister(*package, !everything); !r) return Fail(r.error());
        if (everything) {
            log(Level::Success, Text::Get(IDS_LOG_UNREGISTERED_ALL));
        } else {
            if (!data.empty()) ForgetPopulatedApps(data);
            log(Level::Success, Text::Format(IDS_LOG_UNREGISTERED_KEEP, data.wstring(), FormatBytes(Install::FolderSize(data))));
        }
    }

    // 3. The install folder (this program too, when it runs from there: it goes at the next restart),
    //    except what WSA-Next Setup installed: Windows Installer removes that.
    phase(IDS_PHASE_REMOVE_FILES);
    const bool setupOwnsProgram = Install::SetupProduct().has_value();
    if (!other) RemoveFolder(folder, state, cb, outcome, setupOwnsProgram);
    if (!setupOwnsProgram && !other) RemoveHostCopy(folder, cb, outcome);
    // WSA-Next's own Start menu entry for Android's Settings goes in both kinds of removal: WsaClient
    // removes only the entries it made itself.
    Install::RemoveAndroidSettingsShortcut();
    if (!everything) {
        phase(IDS_PHASE_DONE, 1000);
        return outcome;
    }

    // 4. The Android data and everything WSA left in Windows - unless another WSA owns them.
    phase(IDS_PHASE_CLEANUP);
    const bool foreign = package && !ours;
    if (foreign) {
        log(Level::Warning, Text::Get(IDS_LOG_TRACES_KEPT));
    } else {
        // This user's Android data is this user's own, also when another user's WSA stays.
        if (!data.empty() && fs::exists(data, ec)) {
            const std::uint64_t size = Install::FolderSize(data);
            if (auto r = Install::DeleteWithRetry(data)) log(Level::Success, Text::Format(IDS_LOG_PACKAGE_DATA, data.wstring(), FormatBytes(size)));
            else warn(r.error());
        }
        if (!other) {
            const Install::TraceReport report = Install::RemoveTraces(folder, warn);
            log(Level::Success, Text::Format(IDS_LOG_TRACES, report.shortcuts, report.keys, report.values));
        }
    }

    // 5. What WSA-Next turned on in Windows, unless something else needs it now.
    const std::wstring otherWsa = other ? std::format(L"WSA ({})", other->user) : std::wstring();
    if (Settings::ReadDword(Config::kEnabledDeveloperMode, 0)) {
        if (other) log(Level::Info, Text::Format(IDS_LOG_DEVMODE_KEPT, otherWsa));
        else if (const auto others = Install::OtherDevelopmentPackages(); !others.empty()) log(Level::Info, Text::Format(IDS_LOG_DEVMODE_KEPT, Join(others)));
        else if (auto r = DisableDeveloperMode()) log(Level::Success, Text::Get(IDS_LOG_DEVMODE_OFF));
        else warn(r.error());
    }
    if (Settings::ReadDword(Config::kEnabledVmp, 0)) {
        if (WslDistributions() > 0) log(Level::Info, Text::Format(IDS_LOG_VMP_KEPT, L"WSL"));
        else if (other) log(Level::Info, Text::Format(IDS_LOG_VMP_KEPT, otherWsa));
        else if (foreign) log(Level::Info, Text::Format(IDS_LOG_VMP_KEPT, std::format(L"WSA {}", package->version)));
        else if (auto r = DisableVirtualMachinePlatform(info)) {
            outcome.restartNeeded = *r;
            log(Level::Success, Text::Get(IDS_LOG_VMP_OFF));
        } else {
            warn(r.error());
        }
    }

    // 6. What the bridges left: Hyper-V socket service registrations of test builds, the log folder.
    const bool bluetoothService = Guest::RemoveLegacyServiceRegistration(Bt::kBridgePort);
    const bool helloService = Guest::RemoveLegacyServiceRegistration(Bio::kBridgePort);
    bool logs = false;
    if (const fs::path logFolder = Bt::LogFolder(); !logFolder.empty() && fs::exists(logFolder, ec)) {
        if (auto r = Install::DeleteWithRetry(logFolder, 5)) logs = true;
        else warn(r.error());
    }
    if (hostWasRunning || hostWasAutostart || bluetoothService || helloService || logs) log(Level::Success, Text::Get(IDS_LOG_BLUETOOTH_REMOVED));

    // 7. WSA-Next's own settings.
    RemoveSettings(cb);
    phase(IDS_PHASE_DONE, 1000);
    return outcome;
}

}  // namespace WN::Core
