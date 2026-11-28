// WSA-Next © 2026 Marek Wesołowski - wesmar
#include <winsock2.h>
#include <Windows.h>
#include <shellapi.h>
#include "host/Host.h"
#include "host/Guest.h"
#include "host/Registration.h"
#include "host/Tray.h"
#include "host/Updates.h"
#include "biometrics/ElevationBridge.h"
#include "biometrics/HelloBridge.h"
#include "core/Install.h"
#include "core/Release.h"
#include "install/Package.h"
#include "bluetooth/Bridge.h"
#include "bluetooth/Log.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/HostEvents.h"
#include "support/Settings.h"
#include "support/Signature.h"
#include "support/Text.h"
#include "telephony/ModemBridge.h"
#include <objbase.h>
#include <taskschd.h>
#define SECURITY_WIN32
#include <security.h>
#include <wrl/client.h>
#include <cstring>
#include <filesystem>
#include <memory>
#include <format>
#include <stop_token>
#include <string>
#include <thread>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "oleaut32.lib")

namespace WN::Host {
using Microsoft::WRL::ComPtr;
namespace {

constexpr wchar_t kMutex[] = L"Local\\WSA-Next.Host";
constexpr wchar_t kStopEvent[] = L"Local\\WSA-Next.Host.Stop";
constexpr wchar_t kReloadEvent[] = L"Local\\WSA-Next.Host.Reload";
constexpr wchar_t kBluetoothSetting[] = L"Bluetooth";
constexpr wchar_t kElevationSetting[] = L"Elevation";
constexpr wchar_t kElevationSuSetting[] = L"ElevationSu";
constexpr wchar_t kTraySetting[] = L"Tray";
constexpr wchar_t kHelloRememberSetting[] = L"HelloRemember";
constexpr wchar_t kOldAutostartTask[] = L"WSA-Next Bluetooth";  // before the host process (2026-11-20)

void Signal(const wchar_t* name) {
    const KernelHandle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, name));
    if (event) SetEvent(event.get());
}

// A bridge on its own thread, in the multithreaded apartment its WinRT calls need.
template <class Bridge>
std::jthread StartBridge(Bridge bridge) {
    return std::jthread([bridge](std::stop_token stop) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bridge(stop);
        if (SUCCEEDED(com)) CoUninitialize();
    });
}

// BSTR owner for the Task Scheduler interfaces.
class Bstr {
public:
    explicit Bstr(const std::wstring& text) : m_value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
    ~Bstr() { SysFreeString(m_value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return m_value; }

private:
    BSTR m_value;
};

std::wstring XmlEscape(std::wstring_view text) {
    std::wstring out;
    for (wchar_t c : text) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::wstring CurrentUser() {
    ULONG size = 0;
    GetUserNameExW(NameSamCompatible, nullptr, &size);
    std::wstring name(size, L'\0');
    if (!GetUserNameExW(NameSamCompatible, name.data(), &size)) return {};
    name.resize(size);
    return name;
}

Result<ComPtr<ITaskFolder>> RootFolder() {
    const auto fail = [](HRESULT hr) { return Fail(ErrorText(Text::Get(IDS_CTX_BT_TASK), static_cast<unsigned long>(hr))); };
    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(service.GetAddressOf()));
    if (FAILED(hr)) return fail(hr);
    VARIANT empty;
    VariantInit(&empty);
    if (FAILED(hr = service->Connect(empty, empty, empty, empty))) return fail(hr);
    ComPtr<ITaskFolder> root;
    if (FAILED(hr = service->GetFolder(Bstr(L"\\"), root.GetAddressOf()))) return fail(hr);
    return root;
}

}  // namespace

int RunBackground() {
    const KernelHandle mutex(CreateMutexW(nullptr, TRUE, kMutex));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // another instance serves this session
    const KernelHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, kStopEvent));
    const KernelHandle reloadEvent(CreateEventW(nullptr, FALSE, FALSE, kReloadEvent));
    if (!stopEvent || !reloadEvent) return 1;
    ResetEvent(stopEvent.get());
    if (!Signature::OfficialFeatures()) {
        Bt::Log("host: this WSA-Next.exe is not an official build (no publisher signature): "
                "Bluetooth and Windows Hello for Android are not available");
        return 3;
    }
    Bt::Log("host: started");

    // The bridges serve an official WSA-Next release only, as published: the WSA package's folder
    // must hold the publisher-signed manifest and every Android disk with the digest listed in it.
    // Digests are remembered per file, so this reads the disks only after they changed.
    using Core::Release::State;
    Core::Release::SetHostVerdict(State::Checking);
    NotifyHostChanged();
    const auto package = Install::FindWsa();
    const State verdict = Core::Release::VerifyInstalled(package ? package->location : std::filesystem::path(),
                                                         [&] { return WaitForSingleObject(stopEvent.get(), 0) == WAIT_OBJECT_0; });
    Core::Release::SetHostVerdict(verdict);
    NotifyHostChanged();
    if (verdict == State::Unknown) return 0;  // stopped while checking
    if (verdict != State::Official) {
        Bt::Log("host: the installed Android is {} - Bluetooth and Windows Hello are not served",
                verdict == State::Modified  ? "an official release changed after installing"
                : verdict == State::Missing ? "missing"
                                            : "not an official WSA-Next release");
#ifndef _DEBUG
        return 4;
#endif
    }

    std::jthread hello = StartBridge([](std::stop_token stop) { (void)Bio::RunBridge(stop); });
    // Google Play registration of a new installation: offered in a Windows notification
    // (host/Registration.h); a click on Android's own notification does not come back to Android.
    // The window and the command line ask for it through kHostRegisterEvent.
    const KernelHandle registerEvent(CreateEventW(nullptr, FALSE, FALSE, kHostRegisterEvent));
    Settings::WriteDword(kGooglePlayIdKnownSetting, 0);  // until Android hands the ID to this host
    std::jthread registration = StartBridge([](std::stop_token stop) { RunRegistration(stop); });
    // The PC's modem as Android's SIM, network and SMS (ends at once on a PC without one).
    std::jthread modem = StartBridge([](std::stop_token stop) { Tel::RunBridge(stop); });
    // A newer WSA-Next or Android release: the icon's dot (host/Updates.h).
    std::jthread updates = StartBridge([](std::stop_token stop) { RunUpdateCheck(stop); });
    std::jthread bluetooth, elevation;
    std::unique_ptr<Tray> tray;
    NotifyHostChanged();
    // A bridge that follows its setting: started when turned on, stopped (and joined) when off.
    auto follow = [](std::jthread& bridge, bool wanted, auto run) {
        if (wanted && !bridge.joinable()) {
            bridge = StartBridge(run);
        } else if (!wanted && bridge.joinable()) {
            bridge.request_stop();
            bridge.join();
            bridge = {};
        }
    };
    bool su = ElevationSuEnabled();
    for (;;) {
        follow(bluetooth, BluetoothEnabled(), [](std::stop_token stop) { (void)Bt::RunBridge(stop); });
        if (const bool now = ElevationSuEnabled(); now != su) {
            su = now;
            if (elevation.joinable()) {  // reconnects below with the new setting
                elevation.request_stop();
                elevation.join();
                elevation = {};
            }
        }
        follow(elevation, ElevationEnabled(), [su](std::stop_token stop) { Bio::Elevation::RunBridge(stop, su); });
        if (TrayEnabled() && !tray) tray = std::make_unique<Tray>(stopEvent.get());
        else if (!TrayEnabled()) tray.reset();
        NotifyHostChanged();
        const HANDLE events[] = {stopEvent.get(), reloadEvent.get(), registerEvent.get()};
        const DWORD count = registerEvent ? 3 : 2;
        DWORD wait = WaitForMultipleObjects(count, events, FALSE, INFINITE);
        while (wait == WAIT_OBJECT_0 + 2) {
            // The browser is opened through Explorer's COM objects: this thread needs COM.
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            RequestRegistration();
            if (SUCCEEDED(com)) CoUninitialize();
            wait = WaitForMultipleObjects(count, events, FALSE, INFINITE);
        }
        if (wait != WAIT_OBJECT_0 + 1) break;
    }
    Bt::Log("host: stopping");
    tray.reset();
    updates = {};
    elevation = {};  // stops and joins the bridges
    bluetooth = {};
    modem = {};
    registration = {};
    hello = {};
    NotifyHostChanged();
    return 0;
}

bool Running() {
    const KernelHandle mutex(OpenMutexW(SYNCHRONIZE, FALSE, kMutex));
    return static_cast<bool>(mutex);
}

std::wstring Executable() {
    const std::filesystem::path installed = Core::HostFolder() / Config::kExeName;
    std::error_code ec;
    return std::filesystem::exists(installed, ec) ? installed.wstring() : ModulePath();
}

Result<> Start() {
    if (Running()) return {};
    const std::wstring exe = Executable();
    std::wstring command = std::format(L"\"{}\" host --background", exe);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        return Fail(ErrorText(Text::Get(IDS_CTX_BT_START), GetLastError()));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

void Stop() {
    // Waiting for the host's mutex returns when the host has ended (it is then abandoned); release
    // and close it at once so that a new host can create it.
    const KernelHandle mutex(OpenMutexW(SYNCHRONIZE, FALSE, kMutex));
    if (!mutex) return;
    Signal(kStopEvent);
    const DWORD wait = WaitForSingleObject(mutex.get(), 10000);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) ReleaseMutex(mutex.get());
}

Result<> Restart() {
    Stop();
    return Start();
}

void Reload() {
    if (Running()) Signal(kReloadEvent);
}

bool BluetoothEnabled() { return Settings::ReadDword(kBluetoothSetting, 0) != 0; }

Result<> SetBluetoothEnabled(bool enabled) {
    Settings::WriteDword(kBluetoothSetting, enabled ? 1 : 0);
    if (Running()) {
        Signal(kReloadEvent);
        return {};
    }
    return Start();
}

bool ElevationEnabled() { return Settings::ReadDword(kElevationSetting, 0) != 0; }

Result<> SetElevationEnabled(bool enabled) {
    Settings::WriteDword(kElevationSetting, enabled ? 1 : 0);
    if (Running()) {
        Signal(kReloadEvent);
        return {};
    }
    return Start();
}

bool ElevationSuEnabled() { return Settings::ReadDword(kElevationSuSetting, 0) != 0; }

Result<> SetElevationSuEnabled(bool enabled) {
    Settings::WriteDword(kElevationSuSetting, enabled ? 1 : 0);
    if (Running()) Signal(kReloadEvent);
    return {};
}

std::uint32_t HelloRememberMinutes() { return Settings::ReadDword(kHelloRememberSetting, 0); }

void SetHelloRememberMinutes(std::uint32_t minutes) { Settings::WriteDword(kHelloRememberSetting, minutes); }

bool TrayEnabled() { return Settings::ReadDword(kTraySetting, 1) != 0; }

Result<> SetTrayEnabled(bool enabled) {
    Settings::WriteDword(kTraySetting, enabled ? 1 : 0);
    if (Running()) Signal(kReloadEvent);
    return {};
}

bool AutostartEnabled() {
    auto root = RootFolder();
    if (!root) return false;
    ComPtr<IRegisteredTask> task;
    return SUCCEEDED((*root)->GetTask(Bstr(kAutostartTask), task.GetAddressOf()));
}

Result<> SetAutostart(bool enable) {
    auto root = RootFolder();
    if (!root) return Fail(root.error());
    (void)(*root)->DeleteTask(Bstr(kOldAutostartTask), 0);
    if (!enable) {
        const HRESULT hr = (*root)->DeleteTask(Bstr(kAutostartTask), 0);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            return Fail(ErrorText(Text::Get(IDS_CTX_BT_TASK), static_cast<unsigned long>(hr)));
        return {};
    }
    const std::wstring user = XmlEscape(CurrentUser());
    const std::wstring xml = std::format(
        LR"(<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><Description>Gives Android in WSA-Next this PC's Bluetooth adapter and Windows Hello.</Description></RegistrationInfo>
  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>{0}</UserId></LogonTrigger></Triggers>
  <Principals><Principal id="Author"><UserId>{0}</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Priority>7</Priority>
  </Settings>
  <Actions Context="Author"><Exec><Command>"{1}"</Command><Arguments>host --background</Arguments></Exec></Actions>
</Task>
)",
        user, XmlEscape(Executable()));
    VARIANT empty;
    VariantInit(&empty);
    ComPtr<IRegisteredTask> task;
    const HRESULT hr = (*root)->RegisterTask(Bstr(kAutostartTask), Bstr(xml), TASK_CREATE_OR_UPDATE, empty, empty,
                                             TASK_LOGON_INTERACTIVE_TOKEN, empty, task.GetAddressOf());
    if (FAILED(hr)) return Fail(ErrorText(Text::Get(IDS_CTX_BT_TASK), static_cast<unsigned long>(hr)));
    return {};
}

}  // namespace WN::Host
