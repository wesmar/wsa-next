// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "core/System.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Text.h"
#include "support/Wmi.h"
#include <Windows.h>
#include <format>

namespace WN::Core {
namespace {

constexpr wchar_t kCurrentVersion[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
constexpr wchar_t kAppModelUnlock[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock";

std::wstring RegString(const wchar_t* key, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < 2) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

DWORD RegDword(const wchar_t* key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0, bytes = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS) return fallback;
    return value;
}

// RtlGetVersion is not subject to the manifest-based version lie of GetVersionEx.
RTL_OSVERSIONINFOW RealVersion() {
    RTL_OSVERSIONINFOW info{sizeof info};
    using Fn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (const auto fn = reinterpret_cast<Fn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&info);
    return info;
}

std::optional<bool> FeatureEnabled(const wchar_t* name) {
    const Wmi::ComScope com;
    auto svc = Wmi::Service::Connect(L"ROOT\\CIMV2");
    if (!svc) return std::nullopt;
    auto rows = svc->Query(std::format(L"SELECT InstallState FROM Win32_OptionalFeature WHERE Name = '{}'", Wmi::Quote(name)));
    if (!rows || rows->empty()) return std::nullopt;
    const auto state = Wmi::GetU32((*rows)[0].get(), L"InstallState");
    if (!state) return std::nullopt;
    return *state == 1;  // 1 enabled, 2 disabled, 3 absent
}

}  // namespace

bool SystemInfo::Supported() const { return build >= Config::kMinWindowsBuild; }
bool SystemInfo::NeedsPatch() const { return build >= Config::kPatchFromBuild; }

std::wstring SystemInfo::Version() const {
    std::wstring name = edition.empty() ? std::wstring(L"Windows") : edition;
    // The registry still says "Windows 10" in ProductName on Windows 11.
    if (build >= 22000 && name.starts_with(L"Windows 10")) name.replace(8, 2, L"11");
    if (!displayVersion.empty()) name += L" " + displayVersion;
    return std::format(L"{} ({}.{})", name, build, revision);
}

SystemInfo Inspect() {
    SystemInfo info;
    const RTL_OSVERSIONINFOW v = RealVersion();
    info.build = v.dwBuildNumber;
    info.revision = RegDword(kCurrentVersion, L"UBR", 0);
    info.edition = RegString(kCurrentVersion, L"ProductName");
    info.displayVersion = RegString(kCurrentVersion, L"DisplayVersion");
    info.virtualMachinePlatform = FeatureEnabled(L"VirtualMachinePlatform");
    info.developerMode = RegDword(kAppModelUnlock, L"AllowDevelopmentWithoutDevLicense", 0) != 0;
    info.package = Install::FindWsa();
    if (!info.package) info.otherUser = Install::FindWsaOfOtherUser();
    return info;
}

Result<> EnableDeveloperMode() {
    const DWORD one = 1;
    const LSTATUS s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, kAppModelUnlock, L"AllowDevelopmentWithoutDevLicense", REG_DWORD, &one, sizeof one);
    if (s != ERROR_SUCCESS) return Fail(ErrorText(Text::Get(IDS_CTX_DEVMODE), static_cast<unsigned long>(s)));
    return {};
}

Result<> DisableDeveloperMode() {
    const DWORD zero = 0;
    const LSTATUS s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, kAppModelUnlock, L"AllowDevelopmentWithoutDevLicense", REG_DWORD, &zero, sizeof zero);
    if (s != ERROR_SUCCESS) return Fail(ErrorText(Text::Get(IDS_CTX_DEVMODE), static_cast<unsigned long>(s)));
    return {};
}

namespace {

// "dism.exe /online /<verb>-feature /featurename:VirtualMachinePlatform"; true = Windows must restart.
Result<bool> VirtualMachinePlatform(bool enable, const LogFn& log) {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring dism = std::format(L"{}\\dism.exe", system);
    std::wstring command = std::format(L"\"{}\" /online /{}-feature /featurename:VirtualMachinePlatform{} /norestart /quiet", dism,
                                       enable ? L"enable" : L"disable", enable ? L" /all" : L"");
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    log(Text::Get(enable ? IDS_LOG_ENABLE_VMP : IDS_LOG_DISABLE_VMP));
    if (!CreateProcessW(dism.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return Fail(ErrorText(Text::Get(IDS_CTX_VMP), GetLastError()));
    const KernelHandle process(pi.hProcess), thread(pi.hThread);
    WaitForSingleObject(process.get(), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    if (code == ERROR_SUCCESS_REBOOT_REQUIRED) return true;
    if (code != 0) return Fail(ErrorText(Text::Get(IDS_CTX_VMP), code));
    return false;
}

}  // namespace

Result<bool> EnableVirtualMachinePlatform(const LogFn& log) { return VirtualMachinePlatform(true, log); }
Result<bool> DisableVirtualMachinePlatform(const LogFn& log) { return VirtualMachinePlatform(false, log); }

unsigned WslDistributions() {
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &raw) != ERROR_SUCCESS) return 0;
    const RegKey key(raw);
    DWORD subKeys = 0;
    if (RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) !=
        ERROR_SUCCESS)
        return 0;
    return subKeys;
}

}  // namespace WN::Core
