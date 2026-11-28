// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "core/Install.h"
#include "core/Update.h"
#include "install/Package.h"
#include "install/Setup.h"
#include "install/Traces.h"
#include "install/Unpack.h"
#include "net/Downloader.h"
#include "support/Config.h"
#include "core/Release.h"
#include "host/Host.h"
#include "support/Json.h"
#include "support/Signature.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <cwctype>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

namespace WN::Core {
namespace fs = std::filesystem;
namespace {

// Overall progress: downloads 0-80 %, unpacking 80-97 %, Windows registration 97-100 %.
constexpr int kDownloadShare = 800, kUnpackShare = 170;

class Meter {
public:
    Meter(const ProgressSink& sink) : m_sink(sink) {}
    void Step(std::wstring phase, int step, int steps) {
        m_p.phase = std::move(phase);
        m_p.step = step;
        m_p.steps = steps;
        m_p.done = m_p.total = m_p.speed = 0;
        m_p.eta = 0;
        m_start = Now();
        m_startBytes = 0;
        m_first = true;
        m_lastSent = 0;
    }
    // `base` + `share` * done/total -> permille of the whole job.
    void Bytes(std::uint64_t done, std::uint64_t total, int base, int share) {
        if (m_first) { m_startBytes = done; m_first = false; }  // resumed bytes do not count for the speed
        m_p.done = done;
        m_p.total = total;
        const double elapsed = Seconds(Now() - m_start);
        if (elapsed > 0.5 && done > m_startBytes) {
            m_p.speed = static_cast<std::uint64_t>(static_cast<double>(done - m_startBytes) / elapsed);
            m_p.eta = m_p.speed ? static_cast<double>(total - std::min(done, total)) / static_cast<double>(m_p.speed) : 0;
        }
        m_p.permille = base + (total ? static_cast<int>(static_cast<double>(share) * static_cast<double>(done) / static_cast<double>(total)) : 0);
        const auto now = GetTickCount64();
        if (now - m_lastSent >= 100 || done == total) {  // at most ten updates a second
            m_lastSent = now;
            m_sink(m_p);
        }
    }
    void Phase(std::wstring phase, int permille) {
        m_p.phase = std::move(phase);
        m_p.permille = permille;
        m_p.done = m_p.total = m_p.speed = 0;
        m_p.eta = 0;
        m_p.step = m_p.steps = 0;
        m_sink(m_p);
    }

private:
    using Clock = std::chrono::steady_clock;
    static Clock::time_point Now() { return Clock::now(); }
    static double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
    const ProgressSink& m_sink;
    Progress m_p;
    Clock::time_point m_start{};
    std::uint64_t m_startBytes = 0;
    bool m_first = true;
    ULONGLONG m_lastSent = 0;
};

fs::path StatePath(const fs::path& folder) { return folder / Config::kStateFile; }

Result<> WriteState(const fs::path& folder, const InstalledState& state) {
    Json::Object components, entries;
    for (const auto& [id, sha] : state.components) components.emplace(id, Json::Value(sha));
    for (const auto& [id, names] : state.entries) {
        Json::Array list;
        for (const auto& name : names) list.emplace_back(name);
        entries.emplace(id, Json::Value(std::move(list)));
    }
    Json::Object root{{L"format", Json::Value(std::uint64_t{1})},
                      {L"version", Json::Value(state.version)},
                      {L"source", Json::Value(state.source)},
                      {L"components", Json::Value(std::move(components))},
                      {L"entries", Json::Value(std::move(entries))}};
    const std::string text = Json::Write(Json::Value(std::move(root)));
    const fs::path path = StatePath(folder), temp = fs::path(path).concat(L".new");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return Fail(Text::Format(IDS_CANNOT_CREATE_FILE, temp.wstring()));
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return Fail(ErrorText(Text::Format(IDS_CTX_REPLACE, path.wstring()), GetLastError()));
    return {};
}

std::wstring TargetName(const Component& c) {
    return c.kind == ComponentKind::Archive ? Text::Format(IDS_COMPONENT_WINDOWS, c.id) : fs::path(c.target).filename().wstring();
}

// First element of a path relative to the install folder ("Tools/kernel" -> "Tools"; "." -> "").
std::wstring TopLevel(const fs::path& relative) {
    for (const auto& part : relative)
        if (!part.empty() && part != L".") return part.wstring();
    return {};
}

// What component `c` wrote into the folder: its target, or for an archive unpacked into the folder
// itself every top-level entry of the archive.
std::vector<std::wstring> EntriesOf(const Component& c, const std::set<std::wstring>* archiveTop) {
    const std::wstring top = TopLevel(c.target);
    if (!top.empty() || !archiveTop) return {top};
    return {archiveTop->begin(), archiveTop->end()};
}


// Replaces `target` with a copy of `source`; a running `target` (the host) is renamed out of the
// way first (allowed for a running executable) and deleted now or at the next restart.
Result<> ReplaceFile(const fs::path& source, const fs::path& target) {
    std::error_code ec;
    const fs::path old = fs::path(target).concat(L".old");
    const bool existed = fs::exists(target, ec);
    if (existed && !MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
        return Fail(ErrorText(Text::Format(IDS_CTX_REPLACE, target.wstring()), GetLastError()));
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        const DWORD error = GetLastError();
        // The logon task points at `target`: put the previous file back.
        if (existed) MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        return Fail(ErrorText(Text::Format(IDS_CANNOT_CREATE_FILE, target.wstring()), error));
    }
    if (fs::exists(old, ec) && !DeleteFileW(old.c_str())) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return {};
}

// WSA-Next.exe and its language files into %ProgramFiles%\WSA-Next, wherever Android is: the
// host's logon task runs with the highest privileges, so its executable must be in a folder only
// administrators may change. Then the task pointing there and the host restarted from there -
// for the current user only (`thisUser`); WSA registered for another user keeps that user's task.
// When WSA-Next Setup is installed, its copy there serves the host: Windows Installer owns it.
Result<> InstallHost(const fs::path& folder, InstalledState& state, bool thisUser) {
    const fs::path host = HostFolder();
    std::error_code ec;
    fs::create_directories(host, ec);
    const fs::path self = ModulePath();
    if (!fs::equivalent(self, host / Config::kExeName, ec) && !Install::SetupProduct()) {
        if (auto r = ReplaceFile(self, host / Config::kExeName); !r) return r;
        const fs::path languages = self.parent_path() / L"language";
        if (fs::is_directory(languages, ec)) {
            fs::create_directories(host / L"language", ec);
            for (fs::directory_iterator it(languages, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == L".lng") (void)ReplaceFile(it->path(), host / L"language" / it->path().filename());
        }
    }
    if (fs::equivalent(folder, host, ec)) {
        state.entries[L"host"] = {Config::kExeName, L"language"};
        if (auto w = WriteState(folder, state); !w) return w;
    }
    if (!thisUser) return {};
    if (auto r = Host::SetAutostart(true); !r) return r;
    return Host::Restart();
}

// A folder in %ProgramFiles% (its ACL lets only administrators change files there).
bool AdminOnlyFolder(const fs::path& folder) {
    PWSTR programFiles = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) return false;
    // Windows paths ignore case: compare them lower-cased.
    auto lower = [](fs::path p) {
        std::wstring s = p.lexically_normal().wstring();
        std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return fs::path(s);
    };
    const fs::path root = lower(programFiles);
    CoTaskMemFree(programFiles);
    const fs::path relative = lower(folder).lexically_relative(root);
    return !relative.empty() && *relative.begin() != L".." && relative != L".";
}

bool Present(const fs::path& folder, const Component& c) {
    std::error_code ec;
    if (c.kind == ComponentKind::Archive) return fs::exists(folder / c.target / Config::kPackageManifest, ec) || c.id != L"windows";
    return fs::exists(folder / c.target, ec);
}

std::uint64_t FreeBytes(const fs::path& folder) {
    fs::path probe = folder;
    std::error_code ec;
    while (!probe.empty() && !fs::exists(probe, ec)) probe = probe.parent_path();
    ULARGE_INTEGER free{};
    if (probe.empty() || !GetDiskFreeSpaceExW(probe.c_str(), &free, nullptr, nullptr)) return UINT64_MAX;
    return free.QuadPart;
}

}  // namespace

bool Plan::UpToDate() const {
    return std::ranges::none_of(items, [](const PlanItem& i) { return i.needed; });
}

fs::path DefaultFolder() {
    PWSTR programFiles = nullptr;
    fs::path folder = L"C:\\Program Files";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) folder = programFiles;
    CoTaskMemFree(programFiles);
    return folder / Config::kInstallFolder;
}

InstalledState ReadState(const fs::path& folder) {
    InstalledState state;
    std::ifstream in(StatePath(folder), std::ios::binary);
    if (!in) return state;
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto json = Json::Parse(buffer.str());
    if (!json) return state;
    state.version = json->StringOr(L"version");
    state.source = json->StringOr(L"source");
    for (const auto& [id, sha] : (*json)[L"components"].Members())
        if (auto s = sha.String()) state.components.emplace(id, *s);
    for (const auto& [id, list] : (*json)[L"entries"].Members()) {
        auto& names = state.entries[id];
        for (const auto& item : list.Items())
            if (auto s = item.String()) names.push_back(*s);
    }
    return state;
}

bool InstalledState::EntriesComplete() const {
    return std::ranges::all_of(components, [this](const auto& c) { return entries.contains(c.first); });
}

Plan MakePlan(const Manifest& manifest, const InstalledState& state, const SystemInfo& system, const fs::path& folder) {
    Plan plan;
    for (const Component& c : manifest.components) {
        if (c.optional && !(c.id == L"wsapatch" && system.NeedsPatch())) continue;
        const auto it = state.components.find(c.id);
        const bool same = it != state.components.end() && it->second == c.packedSha256 && Present(folder, c);
        plan.items.push_back({&c, !same});
        if (!same) {
            plan.downloadBytes += c.packedSize;
            plan.diskBytes += c.size;
        }
    }
    return plan;
}

Result<Outcome> Run(const Manifest& manifest, const Options& options, const Callbacks& cb, std::stop_token stop) {
    Outcome outcome;
    Meter meter(cb.progress);
    auto log = [&](Level level, std::wstring text) { cb.log(level, std::move(text)); };
    const fs::path& folder = options.folder;
    const fs::path downloads = folder / Config::kDownloadFolder;

    // --- What this Windows has, what has to be installed ---------------------------------------
    meter.Phase(Text::Get(IDS_PHASE_CHECKING), -1);
    if (Update::AppTooOld(manifest)) return Fail(Text::Format(IDS_APP_TOO_OLD, manifest.version, manifest.app->minimum));
    const SystemInfo system = Inspect();
    log(Level::Info, Text::Format(IDS_LOG_WINDOWS, system.Version()));
    if (!system.Supported()) return Fail(Text::Format(IDS_WINDOWS_TOO_OLD, system.build, Config::kMinWindowsBuild));
    if (manifest.minWindowsBuild > system.build)
        return Fail(Text::Format(IDS_WINDOWS_TOO_OLD, system.build, manifest.minWindowsBuild));

    // An official WSA-Next.exe installs official releases only; a build from the sources installs
    // any release (its own builds), with a warning.
    if (manifest.official) log(Level::Info, Text::Get(IDS_LOG_RELEASE_OFFICIAL));
    else if (Signature::Official()) return Fail(Text::Format(IDS_RELEASE_UNOFFICIAL, manifest.source));
    else log(Level::Warning, Text::Get(IDS_LOG_RELEASE_UNSIGNED));

    InstalledState state = ReadState(folder);
    const Plan plan = MakePlan(manifest, state, system, folder);
    if (plan.UpToDate()) log(Level::Info, Text::Format(IDS_LOG_UP_TO_DATE, manifest.version));
    else log(Level::Info, Text::Format(IDS_LOG_PLAN, manifest.version, FormatBytes(plan.downloadBytes), FormatBytes(plan.diskBytes)));

    // Downloads and unpacked disks live on the same volume at the same time (briefly).
    const std::uint64_t free = FreeBytes(folder);
    if (plan.downloadBytes + plan.diskBytes > free)
        return Fail(Text::Format(IDS_NO_SPACE, FormatBytes(plan.downloadBytes + plan.diskBytes), FormatBytes(free)));

    // WSA registered for another Windows user only. Windows lets one user at a time register an
    // unpacked package, so this account cannot register it. From this folder, the installation
    // updates the files for that user (they are shared); from another folder it cannot be done
    // here. Their running WSA is never cut off: they close it first.
    const bool forOtherUser = !system.package && system.otherUser;
    if (forOtherUser) {
        const Install::OtherRegistration& other = *system.otherUser;
        if (std::error_code ec; !fs::equivalent(other.location, folder, ec))
            return Fail(Text::Format(IDS_OTHER_USER_FOLDER, other.user, other.location.wstring()));
        if (Guest::FindWsaVm()) return Fail(Text::Format(IDS_OTHER_USER_RUNNING, other.user));
        log(Level::Info, Text::Format(IDS_LOG_OTHER_USER, other.user));
    }

    // --- Downloads (WSA may keep running meanwhile) -------------------------------------------
    const int steps = static_cast<int>(std::ranges::count_if(plan.items, [](const PlanItem& i) { return i.needed; }));
    std::uint64_t downloadedBefore = 0;
    int step = 0;
    for (const PlanItem& item : plan.items) {
        if (!item.needed) continue;
        const Component& c = *item.component;
        ++step;
        meter.Step(Text::Format(IDS_PHASE_DOWNLOAD, TargetName(c)), step, steps);
        log(Level::Info, Text::Format(IDS_LOG_DOWNLOAD, c.url, FormatBytes(c.packedSize)));
        const std::uint64_t before = downloadedBefore;
        auto r = Net::Download(
            c.url, downloads / (c.id + L".zst"), c.packedSize, c.packedSha256, Config::kDownloadAttempts,
            [&](std::uint64_t done, std::uint64_t total) {
                const std::uint64_t all = std::max<std::uint64_t>(plan.downloadBytes, 1);
                const int base = static_cast<int>(kDownloadShare * static_cast<double>(before) / static_cast<double>(all));
                const int share = static_cast<int>(kDownloadShare * static_cast<double>(c.packedSize) / static_cast<double>(all));
                meter.Bytes(done, total, base, share);
            },
            [&](std::wstring text) { log(Level::Info, std::move(text)); }, stop);
        if (!r) return Fail(Text::Format(IDS_DOWNLOAD_FAILED, TargetName(c), r.error()));
        downloadedBefore += c.packedSize;
        log(Level::Success, Text::Format(IDS_LOG_VERIFIED, TargetName(c)));
    }

    // --- Unpacking next to the installed files (WSA may keep running meanwhile) ---------------
    // Every component is unpacked to "<name>.new" first; the installed release changes only once
    // all of them are complete, so a failure or a cancellation midway leaves it as it was.
    Install::Staged staged;
    std::map<std::wstring, std::vector<std::wstring>> entries;
    std::vector<const Component*> digested;  // disks whose digest the unpacking has just checked
    step = 0;
    for (const PlanItem& item : plan.items) {
        if (!item.needed) continue;
        const Component& c = *item.component;
        ++step;
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        meter.Step(Text::Format(IDS_PHASE_UNPACK, TargetName(c)), step, steps);
        const int base = kDownloadShare + kUnpackShare * (step - 1) / std::max(steps, 1);
        const int share = kUnpackShare / std::max(steps, 1);
        auto progress = [&](std::uint64_t done, std::uint64_t total) { meter.Bytes(done, total, base, share); };
        const fs::path zst = downloads / (c.id + L".zst");
        if (c.kind == ComponentKind::Archive) {
            auto r = Install::UnpackArchive(zst, folder / c.target, c.size, progress, stop);
            if (!r) return Fail(Text::Format(IDS_UNPACK_FAILED, TargetName(c), r.error()));
            log(Level::Success, Text::Format(IDS_LOG_UNPACKED_ARCHIVE, TargetName(c), r->files, FormatBytes(r->bytes)));
            entries[c.id] = EntriesOf(c, &r->topLevel);
            staged.Append(std::move(r->staged));
        } else {
            auto r = Install::UnpackFile(zst, folder / c.target, c.size, c.sha256, progress, stop);
            if (!r) return Fail(Text::Format(IDS_UNPACK_FAILED, TargetName(c), r.error()));
            log(Level::Success, Text::Format(IDS_LOG_UNPACKED, TargetName(c), FormatBytes(r->bytes)));
            entries[c.id] = EntriesOf(c, nullptr);
            staged.Append(std::move(r->staged));
            if (!c.sha256.empty()) digested.push_back(&c);
        }
    }
    if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));

    // --- The switch: WSA must not use the disks while they are replaced ----------------------
    if (steps) {
        if (system.package) Install::Shutdown(system.package->location, [&](std::wstring t) { log(Level::Info, std::move(t)); });
        else if (forOtherUser && Guest::FindWsaVm()) return Fail(Text::Format(IDS_OTHER_USER_RUNNING, system.otherUser->user));  // started meanwhile
        meter.Phase(Text::Get(IDS_PHASE_SWITCH), kDownloadShare + kUnpackShare);
        if (auto r = staged.Commit(); !r) return Fail(r.error());
        // The host checks the installed disks by their digests, taken here while unpacking. They
        // are handed on only where nobody but an administrator could have changed the files since.
        if (AdminOnlyFolder(folder))
            for (const Component* c : digested) Release::Remember(folder / c->target, c->sha256);
    }
    for (const PlanItem& item : plan.items) {
        if (!item.needed) continue;
        const Component& c = *item.component;
        state.entries[c.id] = std::move(entries[c.id]);
        state.components[c.id] = c.packedSha256;
        ++outcome.installed;
    }
    state.version = manifest.version;
    state.source = manifest.source;
    if (auto w = WriteState(folder, state); !w) return Fail(w.error());  // the files have switched
    if (!options.keepDownloads) {
        std::error_code ec;
        for (const PlanItem& item : plan.items)
            if (item.needed) fs::remove(downloads / (item.component->id + L".zst"), ec);
        fs::remove(downloads, ec);  // only when empty
    }
    // The signed manifest stays with the release: the background host checks the images by it.
    if (auto r = Release::Record(folder, manifest); !r) return Fail(r.error());
    if (manifest.official) state.entries[L"release"] = {Release::kInstalledManifest, Release::kInstalledCatalog};
    else state.entries.erase(L"release");
    state.version = manifest.version;
    state.source = manifest.source;
    if (auto w = WriteState(folder, state); !w) return Fail(w.error());
    if (!options.registerPackage) {
        meter.Phase(Text::Get(IDS_PHASE_DONE), 1000);
        return outcome;
    }
    if (forOtherUser) {
        // Their registration and logon task stay as they are; the host's files in Program Files are
        // shared, so the new WSA-Next.exe goes there (their host takes it at their next sign-in).
        if (auto r = InstallHost(folder, state, false); !r) log(Level::Warning, r.error());
        log(Level::Success, Text::Format(IDS_LOG_OTHER_USER_DONE, manifest.version, system.otherUser->user));
        meter.Phase(Text::Get(IDS_PHASE_DONE), 1000);
        return outcome;
    }

    // --- Windows: developer mode, Virtual Machine Platform, the package ----------------------
    meter.Phase(Text::Get(IDS_PHASE_REGISTER), kDownloadShare + kUnpackShare);
    if (!system.developerMode) {
        if (!cb.confirm(Text::Get(IDS_ASK_DEVMODE))) return Fail(Text::Get(IDS_CANCELED));
        if (auto r = EnableDeveloperMode(); !r) return Fail(r.error());
        Settings::WriteDword(Config::kEnabledDeveloperMode, 1);
        log(Level::Success, Text::Get(IDS_LOG_DEVMODE));
    }
    if (system.virtualMachinePlatform == false) {
        if (!cb.confirm(Text::Get(IDS_ASK_VMP))) return Fail(Text::Get(IDS_CANCELED));
        auto r = EnableVirtualMachinePlatform([&](std::wstring t) { log(Level::Info, std::move(t)); });
        if (!r) return Fail(r.error());
        Settings::WriteDword(Config::kEnabledVmp, 1);
        outcome.restartNeeded = *r;
    }
    if (system.package && !system.package->developmentMode) {
        // WSA from the Microsoft Store: a store package cannot be updated by a development registration.
        if (!cb.confirm(Text::Format(IDS_ASK_REPLACE_STORE, system.package->version))) return Fail(Text::Get(IDS_CANCELED));
        if (auto r = Install::Unregister(*system.package, true); !r) return Fail(r.error());
        log(Level::Info, Text::Get(IDS_LOG_STORE_REMOVED));
    } else if (std::error_code ec; system.package && !fs::equivalent(system.package->location, folder, ec)) {
        // Another unpacked WSA (same identity): registering ours takes its place, the user data stays.
        log(Level::Info, Text::Format(IDS_LOG_REPLACES, system.package->location.wstring()));
    }
    auto logFn = [&](std::wstring t) { log(Level::Info, std::move(t)); };
    if (auto r = Install::InstallDependencies(folder, logFn); !r) return Fail(r.error());
    if (auto r = Install::Register(folder, logFn); !r) {
        // Windows may refuse to move a registration to another folder. Then the old registration is
        // removed with its data kept (a development package keeps LocalCache) and ours registered.
        const auto current = Install::FindWsa();
        std::error_code ec;
        if (!current || !current->developmentMode || fs::equivalent(current->location, folder, ec)) return Fail(r.error());
        log(Level::Warning, Text::Format(IDS_LOG_REREGISTER, r.error()));
        if (auto u = Install::Unregister(*current, true); !u) return Fail(u.error());
        if (auto again = Install::Register(folder, logFn); !again) return Fail(again.error());
    }
    log(Level::Success, Text::Format(IDS_LOG_REGISTERED, manifest.version));
    Install::DefaultWsaSettings();  // a new WSA starts without its diagnostics question
    if (system.NeedsPatch()) log(Level::Warning, Text::Format(IDS_LOG_PATCH_NEEDED, system.build));

    // The background host (Bluetooth and Windows Hello for Android) starts at logon with the
    // highest privileges, so it must run from a folder only administrators can change.
    if (auto r = InstallHost(folder, state, true); r) log(Level::Success, Text::Get(IDS_LOG_HOST_INSTALLED));
    else log(Level::Warning, r.error());
    // Android's own Settings in the Start menu next to the Android apps (WsaClient leaves it out);
    // its icon is icon number 1 of the installed WSA-Next.exe.
    {
        std::error_code ec;
        const fs::path hostExe = HostFolder() / Config::kExeName;
        const std::wstring name = Text::Get(IDS_START_ANDROID_SETTINGS);
        if (auto r = Install::CreateAndroidSettingsShortcut(name, fs::exists(hostExe, ec) ? hostExe : fs::path(ModulePath()), 1); r)
            log(Level::Info, Text::Format(IDS_LOG_SETTINGS_SHORTCUT, name));
        else
            log(Level::Warning, r.error());
    }
    meter.Phase(Text::Get(IDS_PHASE_DONE), 1000);
    return outcome;
}

}  // namespace WN::Core
