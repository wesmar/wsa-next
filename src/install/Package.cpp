// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "install/Package.h"
#include "host/Guest.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Text.h"
#include <Windows.h>
#include <ShlObj.h>
#include <sddl.h>
#include <shellapi.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Core.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <format>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <vector>

#pragma comment(lib, "windowsapp.lib")

namespace WN::Install {
namespace fs = std::filesystem;
using namespace winrt::Windows::Management::Deployment;
using winrt::Windows::Foundation::Uri;
namespace {

std::wstring VersionText(const winrt::Windows::ApplicationModel::PackageVersion& v) {
    return std::format(L"{}.{}.{}.{}", v.Major, v.Minor, v.Build, v.Revision);
}

// "1.2.3.4" -> comparable 64-bit value
std::uint64_t VersionKey(std::wstring_view text) {
    std::uint64_t key = 0;
    int parts = 0;
    std::size_t pos = 0;
    while (parts < 4) {
        std::uint64_t n = 0;
        while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') n = n * 10 + static_cast<std::uint64_t>(text[pos++] - L'0');
        key = (key << 16) | (n & 0xFFFF);
        ++parts;
        if (pos >= text.size() || text[pos] != L'.') break;
        ++pos;
    }
    return key << (16 * (4 - parts));
}

// Waits for a deployment operation and turns its failure into an error with the system's text.
Result<> Await(const winrt::Windows::Foundation::IAsyncOperationWithProgress<DeploymentResult, DeploymentProgress>& op,
               std::wstring_view what) {
    try {
        const DeploymentResult result = op.get();
        if (FAILED(result.ExtendedErrorCode())) {
            std::wstring text = std::wstring(result.ErrorText());
            return Fail(std::format(L"{}: {} (0x{:08X})", what, text, static_cast<unsigned>(result.ExtendedErrorCode())));
        }
        return {};
    } catch (const winrt::hresult_error& e) {
        return Fail(std::format(L"{}: {} (0x{:08X})", what, std::wstring(e.message()), static_cast<unsigned>(e.code())));
    }
}

struct Dependency {
    std::wstring name, minVersion;
};

std::vector<Dependency> ReadDependencies(const fs::path& manifest) {
    std::ifstream in(manifest, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::wstring xml = FromUtf8(buffer.str());
    std::vector<Dependency> out;
    const std::wregex dep(LR"rx(<PackageDependency\s+[^>]*Name="([^"]+)"[^>]*MinVersion="([^"]+)")rx");
    for (std::wsregex_iterator it(xml.begin(), xml.end(), dep), end; it != end; ++it) out.push_back({(*it)[1].str(), (*it)[2].str()});
    return out;
}

}  // namespace

std::optional<PackageInfo> FindWsa() {
    try {
        PackageManager manager;
        for (const auto& package : manager.FindPackagesForUser(L"")) {
            if (package.Id().FamilyName() != Config::kPackageFamily) continue;
            PackageInfo info;
            info.fullName = package.Id().FullName();
            info.version = VersionText(package.Id().Version());
            info.location = std::wstring(package.InstalledLocation().Path());
            info.developmentMode = package.IsDevelopmentMode();
            return info;
        }
    } catch (const winrt::hresult_error&) {
    }
    return std::nullopt;
}

namespace {

// The current user's SID as text ("S-1-5-21-...").
std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    const KernelHandle owned(token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (!size || !GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return {};
    PWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) return {};
    std::wstring sid(text);
    LocalFree(text);
    return sid;
}

// The account name of a SID ("Marek"); the SID itself when it has none.
std::wstring AccountName(const std::wstring& sidText) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(sidText.c_str(), &sid)) return sidText;
    wchar_t name[256]{}, domain[256]{};
    DWORD nameSize = 256, domainSize = 256;
    SID_NAME_USE use{};
    const bool found = LookupAccountSidW(nullptr, sid, name, &nameSize, domain, &domainSize, &use);
    LocalFree(sid);
    return found ? std::wstring(name) : sidText;
}

}  // namespace

std::optional<OtherRegistration> FindWsaOfOtherUser() {
    try {
        PackageManager manager;
        const std::wstring self = CurrentUserSid();
        std::optional<OtherRegistration> other;
        for (const auto& package : manager.FindPackages(Config::kPackageFamily)) {  // every user's
            for (const auto& user : manager.FindUsers(package.Id().FullName())) {
                const std::wstring sid(user.UserSecurityId());
                if (_wcsicmp(sid.c_str(), self.c_str()) == 0) return std::nullopt;  // the current user's too
                if (!other) {
                    std::wstring location;
                    try {
                        location = std::wstring(package.InstalledLocation().Path());
                    } catch (const winrt::hresult_error&) {
                    }
                    other = OtherRegistration{AccountName(sid), location};
                }
            }
        }
        return other;
    } catch (const winrt::hresult_error&) {
        return std::nullopt;  // not an administrator: Windows lists only the current user's packages
    }
}

Result<> InstallDependencies(const fs::path& folder, const LogFn& log) {
    PackageManager manager;
    for (const Dependency& dep : ReadDependencies(folder / Config::kPackageManifest)) {
        std::wstring installed;
        try {
            for (const auto& p : manager.FindPackagesForUser(L"")) {
                if (p.Id().Name() != dep.name || p.Id().Architecture() != winrt::Windows::System::ProcessorArchitecture::X64) continue;
                const std::wstring v = VersionText(p.Id().Version());
                if (VersionKey(v) > VersionKey(installed)) installed = v;
            }
        } catch (const winrt::hresult_error&) {
        }
        if (!installed.empty() && VersionKey(installed) >= VersionKey(dep.minVersion)) {
            log(Text::Format(IDS_LOG_DEP_OK, dep.name, installed));
            continue;
        }
        const fs::path appx = folder / (dep.name + L"_x64.appx");
        if (!fs::exists(appx)) return Fail(Text::Format(IDS_DEP_MISSING, dep.name, dep.minVersion));
        log(Text::Format(IDS_LOG_DEP_INSTALL, dep.name, dep.minVersion));
        auto r = Await(manager.AddPackageAsync(Uri(appx.wstring()), nullptr,
                                               DeploymentOptions::ForceApplicationShutdown | DeploymentOptions::ForceUpdateFromAnyVersion),
                       dep.name);
        if (!r) return r;
    }
    return {};
}

Result<> Register(const fs::path& folder, const LogFn& log) {
    PackageManager manager;
    const fs::path manifest = folder / Config::kPackageManifest;
    log(Text::Format(IDS_LOG_REGISTER, manifest.wstring()));
    return Await(manager.RegisterPackageAsync(Uri(manifest.wstring()), nullptr,
                                              DeploymentOptions::DevelopmentMode | DeploymentOptions::ForceApplicationShutdown |
                                                  DeploymentOptions::ForceUpdateFromAnyVersion),
                 Text::Get(IDS_CTX_REGISTER));
}

Result<> Unregister(const PackageInfo& package, bool keepData) {
    PackageManager manager;
    return Await(manager.RemovePackageAsync(package.fullName, keepData ? RemovalOptions::PreserveApplicationData : RemovalOptions::None),
                 Text::Get(IDS_CTX_UNREGISTER));
}

std::vector<std::wstring> OtherDevelopmentPackages() {
    std::vector<std::wstring> names;
    try {
        PackageManager manager;
        for (const auto& package : manager.FindPackagesForUser(L""))
            if (package.IsDevelopmentMode() && package.Id().FamilyName() != Config::kPackageFamily) names.emplace_back(package.Id().Name());
    } catch (const winrt::hresult_error&) {
    }
    return names;
}

namespace {

// WSA's own way of shutting Android down: "WsaClient /shutdown", through the package's app
// execution alias. Started as a plain file, WsaClient runs outside the package and does not find
// the DLLs of the package root (gfxstream_backend.dll) - Windows then shows "system error" instead
// of shutting anything down. No alias: WSA is not registered. Returns the WsaClient process.
fs::path WsaClientAlias() {
    PWSTR local = nullptr;
    fs::path client;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        client = fs::path(local) / L"Microsoft" / L"WindowsApps" / Config::kPackageFamily / L"WsaClient.exe";
    CoTaskMemFree(local);
    std::error_code ec;
    if (client.empty() || !fs::exists(fs::symlink_status(client, ec))) return {};
    return client;
}

KernelHandle StartShutdown(const fs::path& folder) {
    const fs::path client = WsaClientAlias();
    if (client.empty()) return {};
    std::error_code ec;
    std::wstring command = std::format(L"\"{}\" /shutdown", client.wstring());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    const wchar_t* directory = !folder.empty() && fs::is_directory(folder, ec) ? folder.c_str() : nullptr;
    if (!CreateProcessW(client.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory, &si, &pi)) return {};
    CloseHandle(pi.hThread);
    return KernelHandle(pi.hProcess);
}

}  // namespace

void Shutdown(const fs::path& folder, const LogFn& log) {
    const KernelHandle process = StartShutdown(folder);
    if (!process) return;
    log(Text::Get(IDS_LOG_SHUTDOWN));
    WaitForSingleObject(process.get(), 60000);
}

Guest::VmStop Stop() {
    const auto package = FindWsa();
    if (!package) return Guest::VmStop::NotRunning;
    KernelHandle client;
    return Guest::StopVm([&] { client = StartShutdown(package->location); }, 30000);
}

void DefaultWsaSettings() {
    // WSA Settings' own names, in the package's LocalSettings (Settings\settings.dat).
    constexpr const wchar_t* kOffByDefault[] = {L"DeveloperModeEnabled", L"OptionalDiagnosticDataEnabled"};
    try {
        const auto data = winrt::Windows::Management::Core::ApplicationDataManager::CreateForPackageFamily(Config::kPackageFamily);
        auto values = data.LocalSettings().Values();
        for (const wchar_t* name : kOffByDefault)
            if (!values.HasKey(name)) values.Insert(name, winrt::box_value(false));
    } catch (const winrt::hresult_error&) {
        // no data store yet: WSA keeps its own defaults
    }
}

Result<> LaunchAndroid() {
    // The installed release names its variant ("variant": "Google Play") in release.json.
    bool googlePlay = false;
    fs::path folder;
    if (const auto package = FindWsa()) {
        folder = package->location;
        std::ifstream file(folder / L"release.json", std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::smatch variant;
        static const std::regex kVariant(R"re("variant"\s*:\s*"([^"]*)")re");
        googlePlay = std::regex_search(raw, variant, kVariant) && variant[1].str().find("Google") != std::string::npos;
    }
    const std::wstring uri = googlePlay ? L"wsa://com.android.vending" : L"wsa://com.android.settings";
    if (const fs::path client = WsaClientAlias(); !client.empty()) {
        std::wstring command = std::format(L"\"{}\" /launch {}", client.wstring(), uri);
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        std::error_code ec;
        const wchar_t* directory = !folder.empty() && fs::is_directory(folder, ec) ? folder.c_str() : nullptr;
        if (CreateProcessW(client.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, directory, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return {};
        }
    }
    return OpenUri(uri);
}

Result<> OpenUri(const std::wstring& uri) {
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = uri.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return Fail(ErrorText(Text::Format(IDS_CTX_OPEN, uri), GetLastError()));
    return {};
}

}  // namespace WN::Install
