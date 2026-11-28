// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "core/Update.h"
#include "core/Install.h"
#include "host/Host.h"
#include "net/Downloader.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Settings.h"
#include "support/Signature.h"
#include "support/Text.h"
#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <format>

namespace WN::Core::Update {
namespace fs = std::filesystem;
namespace {

// Settings (HKCU\Software\KVC\WSA-Next).
constexpr wchar_t kAutomaticSetting[] = L"UpdateCheck";      // DWORD, 0 = no background check
constexpr wchar_t kCheckedSetting[] = L"UpdateChecked";      // seconds since 1970 of the last successful check
constexpr wchar_t kAppSetting[] = L"UpdateApp";              // the newer WSA-Next found, "" = none
constexpr wchar_t kReleaseSetting[] = L"UpdateRelease";      // the newer release found, "" = none
constexpr wchar_t kSetupPrefix[] = L"WSA-Next-";             // downloaded Setup: WSA-Next-<version>.msi

std::uint64_t Now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

fs::path SetupFolder() { return HostFolder() / Config::kDownloadFolder; }

// A version as part of a file name: digits and dots only.
bool PlainVersion(const std::wstring& v) {
    return !v.empty() && v.size() <= 32 && std::ranges::all_of(v, [](wchar_t c) { return std::iswdigit(c) || c == L'.'; });
}

}  // namespace

int CompareVersions(std::wstring_view a, std::wstring_view b) {
    // The next field of `s` as a number; `s` moves past it and its dot.
    auto next = [](std::wstring_view& s) {
        std::uint64_t value = 0;
        std::size_t i = 0;
        for (; i < s.size() && std::iswdigit(s[i]); ++i)
            if (value < 100'000'000'000ULL) value = value * 10 + static_cast<std::uint64_t>(s[i] - L'0');
        const std::size_t dot = s.find(L'.', i);
        s = dot == std::wstring_view::npos ? std::wstring_view() : s.substr(dot + 1);
        return value;
    };
    while (!a.empty() || !b.empty()) {
        const std::uint64_t x = next(a), y = next(b);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::wstring SavedSourceUrl() {
    // As the window offers them (MainWindow::SourceUrl): an unknown choice is GitHub.
    const std::wstring key = Settings::Read(L"Source", L"github");
    if (key == L"custom") return Settings::Read(L"CustomManifest");
    if (key == L"mirror") return Config::kMirrorManifest;
    if (key == L"local") return Config::kLocalManifest;
    return std::format(L"https://github.com/{}/releases/download/components/{}", Config::kGitHubRepo, Config::kManifestAsset);
}

fs::path SavedFolder() {
    const std::wstring folder = Settings::Read(L"InstallFolder");
    return folder.empty() ? DefaultFolder() : fs::path(folder);
}

bool AutomaticCheck() { return Settings::ReadDword(kAutomaticSetting, 1) != 0; }

void SetAutomaticCheck(bool on) { Settings::WriteDword(kAutomaticSetting, on ? 1 : 0); }

bool CheckDue() {
    const std::wstring text = Settings::Read(kCheckedSetting);
    std::uint64_t last = 0;
    for (wchar_t c : text) {
        if (!std::iswdigit(c)) return true;
        last = last * 10 + static_cast<std::uint64_t>(c - L'0');
    }
    const std::uint64_t now = Now();
    const auto interval = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(kCheckInterval).count());
    return last > now || now - last >= interval;  // a clock set back counts as due
}

const AppRelease* NewerApp(const Manifest& manifest) {
    // An official build only: a build from the sources is updated from the sources.
    if (!manifest.official || !manifest.app || !Signature::OfficialFeatures()) return nullptr;
    return CompareVersions(manifest.app->version, Config::kAppVersion) > 0 ? &*manifest.app : nullptr;
}

bool AppTooOld(const Manifest& manifest) {
    return manifest.app && !manifest.app->minimum.empty() && CompareVersions(Config::kAppVersion, manifest.app->minimum) < 0;
}

bool Record(const Manifest& manifest, const fs::path& folder) {
    if (!manifest.official) return false;
    std::wstring app, release;
    if (const AppRelease* newer = NewerApp(manifest)) app = newer->version;
    const InstalledState state = ReadState(folder);
    if (!state.version.empty() && CompareVersions(manifest.version, state.version) > 0) release = manifest.version;
    const bool changed = Settings::Read(kAppSetting) != app || Settings::Read(kReleaseSetting) != release;
    Settings::Write(kAppSetting, app);
    Settings::Write(kReleaseSetting, release);
    Settings::Write(kCheckedSetting, std::to_wstring(Now()));
    return changed;
}

Pending Available(const fs::path& folder) {
    // Compared again with what runs and is installed now: an update done since then clears it.
    Pending pending;
    if (std::wstring app = Settings::Read(kAppSetting); !app.empty() && CompareVersions(app, Config::kAppVersion) > 0) pending.app = std::move(app);
    if (std::wstring release = Settings::Read(kReleaseSetting); !release.empty()) {
        const InstalledState state = ReadState(folder);
        if (!state.version.empty() && CompareVersions(release, state.version) > 0) pending.release = std::move(release);
    }
    return pending;
}

Result<> DownloadAndStartSetup(const AppRelease& app, const ProgressFn& progress, const LogFn& log, std::stop_token stop) {
    const fs::path folder = SetupFolder();
    const fs::path setup = folder / (kSetupPrefix + (PlainVersion(app.version) ? app.version : std::wstring(L"update")) + L".msi");
    // Setup of another version left by an update that was stopped (this one's part is resumed).
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        const fs::path& file = entry.path();
        if (file != setup && file.filename().wstring().starts_with(kSetupPrefix) && file.extension() == L".msi") fs::remove(file, ec);
    }
    if (auto r = Net::Download(app.url, setup, app.size, app.sha256, Config::kDownloadAttempts, progress, log, stop); !r) return r;

    // From the signature check on, the file stays open and nobody can change it: Windows Installer
    // gets exactly the bytes that were checked. Closed when this process ends.
    static FileHandle s_checked;
    s_checked.reset(CreateFileW(setup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!s_checked) return Fail(ErrorText(Text::Format(IDS_CTX_OPEN, setup.wstring()), GetLastError()));
    if (Signature::CheckFile(setup.c_str(), s_checked.get()) != Signature::Origin::Official) {
        s_checked.reset();
        fs::remove(setup, ec);
        return Fail(Text::Get(IDS_APP_SETUP_UNSIGNED));
    }

    // cmd runs Windows Installer, deletes the Setup file only once Windows Installer is done with it
    // (and the download folder if that leaves it empty), then opens WSA-Next again - the new one, or
    // the old one if the update was canceled. It waits in no window of its own.
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring cmd = std::format(L"{}\\cmd.exe", system);
    std::wstring line = std::format(
        L"\"{0}\" /d /s /c \"start \"\" /wait \"{1}\\msiexec.exe\" /i \"{2}\" /passive /norestart & del /f /q \"{2}\" & rd \"{3}\" 2>nul & start \"\" \"{4}\"\"",
        cmd, system, setup.wstring(), folder.wstring(), (HostFolder() / Config::kExeName).wstring());
    // This session's background host holds WSA-Next.exe open: stopped first, so that Windows Installer
    // does not wait for it to give the file up (Setup starts it again when it is done).
    Host::Stop();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(cmd.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system, &si, &pi)) {
        const DWORD error = GetLastError();
        (void)Host::Start();
        return Fail(ErrorText(Text::Get(IDS_CTX_SETUP), error));
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

}  // namespace WN::Core::Update
