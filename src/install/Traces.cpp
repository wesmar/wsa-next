// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "install/Traces.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Text.h"
#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <format>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace WN::Install {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
namespace {

constexpr std::wstring_view kFamily = Config::kPackageFamily;
constexpr wchar_t kCurrentVersion[] = L"Software\\Microsoft\\Windows\\CurrentVersion";

bool SameChar(wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }

bool Contains(std::wstring_view text, std::wstring_view part) {
    return !part.empty() && !std::ranges::search(text, part, SameChar).empty();
}

bool StartsWith(std::wstring_view text, std::wstring_view prefix) {
    return !prefix.empty() && text.size() >= prefix.size() && std::ranges::equal(text.substr(0, prefix.size()), prefix, SameChar);
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw))) path = raw;
    CoTaskMemFree(raw);
    return path;
}

// --- Registry (HKCU only) ---------------------------------------------------------------------

std::vector<std::wstring> SubKeys(const std::wstring& path) {
    std::vector<std::wstring> names;
    RegKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, key.put()) != ERROR_SUCCESS) return names;
    wchar_t name[256];  // registry key names have at most 255 characters
    for (DWORD i = 0;; ++i) {
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS s = RegEnumKeyExW(key.get(), i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (s == ERROR_NO_MORE_ITEMS) break;
        if (s == ERROR_SUCCESS) names.emplace_back(name, length);
    }
    return names;
}

// Collected before anything is deleted: deleting a value shifts the enumeration indexes.
std::vector<std::wstring> ValueNames(const std::wstring& path) {
    std::vector<std::wstring> names;
    RegKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, key.put()) != ERROR_SUCCESS) return names;
    DWORD longest = 0;
    if (RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &longest, nullptr, nullptr, nullptr) !=
        ERROR_SUCCESS)
        return names;
    std::vector<wchar_t> name(longest + 1);
    for (DWORD i = 0;; ++i) {
        DWORD length = static_cast<DWORD>(name.size());
        const LSTATUS s = RegEnumValueW(key.get(), i, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (s == ERROR_NO_MORE_ITEMS) break;
        if (s == ERROR_SUCCESS) names.emplace_back(name.data(), length);
    }
    return names;
}

std::wstring StringValue(const std::wstring& path, const wchar_t* name) {
    constexpr DWORD kFlags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, kFlags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, kFlags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

class Cleaner {
public:
    explicit Cleaner(const LogFn& log) : m_log(log) {}

    void Key(const std::wstring& path) {
        const LSTATUS s = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        if (s == ERROR_SUCCESS) {
            RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());  // RegDeleteTree normally takes the key itself too
            ++m_report.keys;
        } else if (s != ERROR_FILE_NOT_FOUND) {
            m_log(ErrorText(Text::Format(IDS_CTX_DELETE, L"HKCU\\" + path), static_cast<unsigned long>(s)));
        }
    }

    void Value(const std::wstring& path, const std::wstring& name) {
        RegKey key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, key.put()) != ERROR_SUCCESS) return;
        const LSTATUS s = RegDeleteValueW(key.get(), name.c_str());
        if (s == ERROR_SUCCESS) ++m_report.values;
        else if (s != ERROR_FILE_NOT_FOUND)
            m_log(ErrorText(Text::Format(IDS_CTX_DELETE, std::format(L"HKCU\\{}\\{}", path, name)), static_cast<unsigned long>(s)));
    }

    void KeysStartingWith(const std::wstring& parent, std::wstring_view prefix) {
        for (const auto& name : SubKeys(parent))
            if (StartsWith(name, prefix)) Key(parent + L"\\" + name);
    }

    void ValuesStartingWith(const std::wstring& key, std::wstring_view prefix) {
        for (const auto& name : ValueNames(key))
            if (StartsWith(name, prefix)) Value(key, name);
    }

    void Shortcut(const fs::path& lnk) {
        std::error_code ec;
        if (fs::remove(lnk, ec)) ++m_report.shortcuts;
        else if (ec) m_log(ErrorText(Text::Format(IDS_CTX_DELETE, lnk.wstring()), static_cast<unsigned long>(ec.value())));
    }

    [[nodiscard]] const TraceReport& Report() const { return m_report; }

private:
    const LogFn& m_log;
    TraceReport m_report;
};

// A shortcut that starts an Android app (WsaClient through the package's app execution alias) or
// a program of the install folder.
bool StartsWsa(const fs::path& lnk, std::wstring_view folderPrefix) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.GetAddressOf()))) ||
        FAILED(link.As(&file)) || FAILED(file->Load(lnk.c_str(), STGM_READ)))
        return false;
    wchar_t target[MAX_PATH * 2]{};
    wchar_t arguments[1024]{};
    // The raw path: the alias it points to is gone once the package is removed.
    if (link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH) != S_OK) target[0] = 0;
    if (FAILED(link->GetArguments(arguments, static_cast<int>(std::size(arguments))))) arguments[0] = 0;
    const std::wstring_view path(target);
    return Contains(path, kFamily) || StartsWith(path, folderPrefix) ||
           (path.ends_with(L"\\WsaClient.exe") && Contains(arguments, L"wsa://"));
}

void RemoveShortcuts(Cleaner& clean, const fs::path& folder, bool recursive, std::wstring_view folderPrefix) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return;
    std::vector<fs::path> found;
    auto consider = [&](const fs::directory_entry& entry) {
        std::error_code e;
        if (entry.is_regular_file(e) && _wcsicmp(entry.path().extension().c_str(), L".lnk") == 0 && StartsWsa(entry.path(), folderPrefix))
            found.push_back(entry.path());
    };
    const auto options = fs::directory_options::skip_permission_denied;
    if (recursive) {
        for (fs::recursive_directory_iterator it(folder, options, ec), end; !ec && it != end; it.increment(ec)) consider(*it);
    } else {
        for (fs::directory_iterator it(folder, options, ec), end; !ec && it != end; it.increment(ec)) consider(*it);
    }
    for (const auto& lnk : found) clean.Shortcut(lnk);
}

}  // namespace

TraceReport RemoveTraces(const fs::path& folder, const LogFn& log) {
    Cleaner clean(log);
    const std::wstring cv = kCurrentVersion;
    const std::wstring family(kFamily);
    const std::wstring appPrefix = family + L"!";  // application user model IDs of the package
    std::wstring folderPrefix;
    if (!folder.empty()) {
        folderPrefix = folder.lexically_normal().wstring();
        if (!folderPrefix.ends_with(L'\\')) folderPrefix += L'\\';
    }

    // Android apps in "Installed apps": the entries whose icon lives in WSA's package data. Their
    // RegisteredApplications values point to the entries' Capabilities.
    const std::wstring uninstall = cv + L"\\Uninstall";
    std::set<std::wstring> apps;
    for (const auto& name : SubKeys(uninstall))
        if (Contains(StringValue(uninstall + L"\\" + name, L"DisplayIcon"), L"\\Packages\\" + family + L"\\")) apps.insert(name);
    const std::wstring registered = L"Software\\RegisteredApplications";
    const std::wstring capabilitiesPrefix = uninstall + L"\\";
    for (const auto& name : ValueNames(registered)) {
        const std::wstring target = StringValue(registered, name.c_str());
        if (!StartsWith(target, capabilitiesPrefix)) continue;
        const std::wstring app = target.substr(capabilitiesPrefix.size(), target.find(L'\\', capabilitiesPrefix.size()) - capabilitiesPrefix.size());
        if (apps.contains(app)) clean.Value(registered, name);
    }
    for (const auto& app : apps) clean.Key(uninstall + L"\\" + app);

    // File and link associations: ProgIDs "wsa.<hash>" that open an Android app, and the references
    // to them in the per-user "Open with" lists of file types.
    const std::wstring classes = L"Software\\Classes";
    const auto classKeys = SubKeys(classes);
    std::set<std::wstring> progIds;
    for (const auto& name : classKeys)
        if (StartsWith(name, L"wsa.") && Contains(StringValue(classes + L"\\" + name + L"\\Shell\\Open\\Command", nullptr), family))
            progIds.insert(name);
    if (!progIds.empty()) {
        for (const auto& type : classKeys) {
            if (!type.starts_with(L'.')) continue;
            const std::wstring openWith = classes + L"\\" + type + L"\\OpenWithProgids";
            for (const auto& value : ValueNames(openWith))
                if (progIds.contains(value)) clean.Value(openWith, value);
        }
    }
    for (const auto& id : progIds) clean.Key(classes + L"\\" + id);

    // Per-app records named after the package: notifications, Start tiles, window sizes, background
    // access, privacy consent (and the consent of the install folder's executables).
    clean.KeysStartingWith(cv + L"\\Notifications\\Settings", appPrefix);
    clean.KeysStartingWith(cv + L"\\Start\\TileProperties", L"P~" + appPrefix);
    clean.KeysStartingWith(L"Software\\Microsoft\\Phone\\ShellUI\\WindowSizing", appPrefix);
    clean.Key(cv + L"\\BackgroundAccessApplications\\" + family);
    const std::wstring consent = cv + L"\\CapabilityAccessManager\\ConsentStore";
    std::wstring consentPrefix = folderPrefix;  // "C:#Program Files#WSA-Next#..."
    std::ranges::replace(consentPrefix, L'\\', L'#');
    for (const auto& capability : SubKeys(consent)) {
        clean.Key(consent + L"\\" + capability + L"\\" + family);
        clean.KeysStartingWith(consent + L"\\" + capability + L"\\NonPackaged", consentPrefix);
    }

    // WsaClient's App Paths entry (written for the package's app execution alias).
    const std::wstring appPath = cv + L"\\App Paths\\WsaClient.exe";
    if (Contains(StringValue(appPath, nullptr), family)) clean.Key(appPath);

    // Windows' caches keyed by the install folder or the package: executable names and versions,
    // the program compatibility assistant, taskbar usage, the compiled resources of resources.pri
    // ("C:%5CProgram Files%5CWSA-Next%5Cresources.pri").
    clean.ValuesStartingWith(L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache", folderPrefix);
    clean.ValuesStartingWith(L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant\\Store", folderPrefix);
    const std::wstring usage = cv + L"\\Explorer\\FeatureUsage";
    for (const auto& kind : SubKeys(usage)) {
        clean.ValuesStartingWith(usage + L"\\" + kind, folderPrefix);
        clean.ValuesStartingWith(usage + L"\\" + kind, appPrefix);
    }
    std::wstring mrtPrefix;
    for (wchar_t c : folderPrefix) mrtPrefix += c == L'\\' ? std::wstring(L"%5C") : std::wstring(1, c);
    clean.KeysStartingWith(L"Software\\Classes\\Local Settings\\MrtCache", mrtPrefix);

    // Shortcuts: Start menu, desktop, taskbar pins.
    RemoveShortcuts(clean, KnownFolder(FOLDERID_Programs), true, folderPrefix);
    RemoveShortcuts(clean, KnownFolder(FOLDERID_Desktop), false, folderPrefix);
    RemoveShortcuts(clean, KnownFolder(FOLDERID_UserPinned), true, folderPrefix);

    const TraceReport& report = clean.Report();
    if (report.shortcuts || report.keys || report.values) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return report;
}

fs::path PackageDataFolder() {
    const fs::path local = KnownFolder(FOLDERID_LocalAppData);
    return local.empty() ? fs::path() : local / L"Packages" / kFamily;
}

std::uint64_t FolderSize(const fs::path& folder) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e;
        if (it->is_regular_file(e)) total += it->file_size(e);
    }
    return total;
}

Result<> DeleteWithRetry(const fs::path& path, int seconds) {
    std::error_code ec;
    for (int attempt = 0;; ++attempt) {
        ec.clear();
        if (!fs::exists(fs::symlink_status(path, ec))) return {};
        fs::remove_all(path, ec);
        if (!ec) return {};
        if (attempt >= seconds) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return Fail(ErrorText(Text::Format(IDS_CTX_DELETE, path.wstring()), static_cast<unsigned long>(ec.value())));
}

}  // namespace WN::Install
