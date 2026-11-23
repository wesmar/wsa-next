// WSA-Next © 2026 Marek Wesołowski - wesmar
#include <winsock2.h>
#include <hvsocket.h>
#include <Windows.h>
#include <computecore.h>
#include <objbase.h>
#include "host/Guest.h"
#include "bluetooth/Log.h"
#include "support/Handle.h"
#include "support/HostEvents.h"
#include "support/Json.h"
#include <string>

#pragma comment(lib, "computecore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace WN::Guest {
namespace {

// While WSA is not running: how often to look for its VM (no HCS notification exists for that).
constexpr DWORD kVmLookupMs = 5000;
// While Android boots and its HAL does not listen yet: the connect attempt waits this long.
constexpr DWORD kConnectTimeoutMs = 2000;

GUID ServiceGuid(std::uint32_t port) {
    // vsock port P of a Linux guest = Hyper-V service "PPPPPPPP-FACB-11E6-BD58-64006A7986D3".
    return GUID{port, 0xFACB, 0x11E6, {0xBD, 0x58, 0x64, 0x00, 0x6A, 0x79, 0x86, 0xD3}};
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string GuidText(const GUID& guid) {
    wchar_t text[64]{};
    StringFromGUID2(guid, text, 64);
    return Utf8(text);
}

bool ContainsNoCase(std::wstring text, std::wstring part) {
    for (auto* s : {&text, &part})
        for (auto& c : *s) c = static_cast<wchar_t>(towlower(c));
    return text.find(part) != std::wstring::npos;
}

// Signals `event` when the compute system ends (HCS notification). Without access to the system
// (watching() false) the caller looks at the VM list instead.
class VmExitWatch {
public:
    VmExitWatch(const GUID& id, HANDLE event) {
        wchar_t text[64]{};
        StringFromGUID2(id, text, 64);
        const std::wstring bare(text + 1, wcslen(text) - 2);  // HCS ids have no braces
        if (FAILED(HcsOpenComputeSystem(bare.c_str(), GENERIC_ALL, &m_system))) {
            m_system = nullptr;
            return;
        }
        if (FAILED(HcsSetComputeSystemCallback(m_system, HcsEventOptionNone, event, &VmExitWatch::OnEvent))) {
            HcsCloseComputeSystem(m_system);
            m_system = nullptr;
        }
    }
    ~VmExitWatch() {
        if (m_system) HcsCloseComputeSystem(m_system);  // also ends the callbacks
    }
    VmExitWatch(const VmExitWatch&) = delete;
    VmExitWatch& operator=(const VmExitWatch&) = delete;
    [[nodiscard]] bool watching() const { return m_system != nullptr; }

    // Ends the compute system at once (like a power cut); its end is then notified as usual.
    bool Terminate() {
        const HCS_OPERATION operation = HcsCreateOperation(nullptr, nullptr);
        if (!operation) return false;
        HRESULT hr = HcsTerminateComputeSystem(m_system, operation, nullptr);
        if (SUCCEEDED(hr)) {
            PWSTR result = nullptr;
            hr = HcsWaitForOperationResult(operation, 15000, &result);
            if (result) LocalFree(result);
        }
        HcsCloseOperation(operation);
        return SUCCEEDED(hr);
    }

private:
    static void CALLBACK OnEvent(HCS_EVENT* event, void* context) {
        if (event->Type == HcsEventSystemExited || event->Type == HcsEventServiceDisconnect) SetEvent(static_cast<HANDLE>(context));
    }
    HCS_SYSTEM m_system = nullptr;
};

// One connection attempt to the guest's port (blocking, at most kConnectTimeoutMs).
SOCKET Connect(const GUID& vm, std::uint32_t port) {
    SOCKET s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return s;
    const DWORD timeout = kConnectTimeoutMs;
    setsockopt(s, HV_PROTOCOL_RAW, HVSOCKET_CONNECT_TIMEOUT, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    SOCKADDR_HV address{};
    address.Family = AF_HYPERV;
    address.VmId = vm;
    address.ServiceId = ServiceGuid(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

}  // namespace

std::wstring ComputeSystemsJson() {
    HCS_OPERATION operation = HcsCreateOperation(nullptr, nullptr);
    if (!operation) return {};
    PWSTR result = nullptr;
    HRESULT hr = HcsEnumerateComputeSystems(nullptr, operation);
    if (SUCCEEDED(hr)) hr = HcsWaitForOperationResult(operation, 10000, &result);
    HcsCloseOperation(operation);
    std::wstring json = SUCCEEDED(hr) && result ? std::wstring(result) : std::wstring();
    if (result) LocalFree(result);
    return json;
}

std::optional<WsaVm> FindWsaVm() {
    const std::wstring text = ComputeSystemsJson();
    if (text.empty()) return std::nullopt;
    const auto json = Json::Parse(Utf8(text));
    if (!json) return std::nullopt;

    // An array of compute system summaries: {"Id":..., "SystemType":..., "Owner":..., "State":...}.
    // WSA's VM has no name; its owner is WSA's localized product name ("Windows Subsystem for
    // Android™", "Podsystem Windows dla systemu Android™" ...): "Android" is in every language.
    // Hyper-V Manager's VMs are owned by "VMMS", whatever their name.
    for (const Json::Value& system : json->Items()) {
        const std::wstring owner = system.StringOr(L"Owner"), state = system.StringOr(L"State");
        if (!ContainsNoCase(owner, L"android") || system.StringOr(L"SystemType") != L"VirtualMachine") continue;
        if (!state.empty() && state != L"Running") continue;
        const std::wstring id = L"{" + system.StringOr(L"Id") + L"}";
        WsaVm vm;
        if (IIDFromString(id.c_str(), &vm.id) != S_OK) continue;
        vm.name = Utf8(owner);
        return vm;
    }
    return std::nullopt;
}

void Serve(std::uint32_t port, const char* peerName, const std::function<void(std::uintptr_t, std::stop_token)>& session,
           std::stop_token stop) {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const KernelHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const std::stop_callback onStop(stop, [event = stopEvent.get()] { SetEvent(event); });
    const KernelHandle vmExited(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    bool waitingLogged = false;
    while (!stop.stop_requested()) {
        const auto vm = FindWsaVm();
        if (!vm) {
            if (!waitingLogged) Bt::Log("host: waiting for WSA to start ({})", peerName);
            waitingLogged = true;
            WaitForSingleObject(stopEvent.get(), kVmLookupMs);
            continue;
        }
        waitingLogged = false;
        ResetEvent(vmExited.get());
        const VmExitWatch exitWatch(vm->id, vmExited.get());
        Bt::Log("host: WSA VM {} ({}), connecting to vsock port 0x{:08X}", GuidText(vm->id), vm->name, port);
        NotifyHostChanged();  // Android is up (the window and the icon show it)

        while (!stop.stop_requested() && WaitForSingleObject(vmExited.get(), 0) != WAIT_OBJECT_0) {
            const ULONGLONG attempt = GetTickCount64();
            const SOCKET s = Connect(vm->id, port);
            if (s == INVALID_SOCKET) {
                // Android is booting (its HAL does not listen yet) or WSA is ending. A refusal can
                // come at once: wait out the rest of the attempt so retries stay paced.
                const ULONGLONG spent = GetTickCount64() - attempt;
                if (spent < kConnectTimeoutMs) {
                    const HANDLE events[] = {stopEvent.get(), vmExited.get()};
                    WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(kConnectTimeoutMs - spent));
                }
                if (!exitWatch.watching()) {
                    const auto now = FindWsaVm();
                    if (!now || !IsEqualGUID(now->id, vm->id)) break;
                }
                continue;
            }
            Bt::Log("host: {} connected", peerName);
            {
                // Ends the session's blocking reads when stopping or when the VM ends.
                const std::stop_callback cancel(stop, [s] { shutdown(s, SD_BOTH); });
                session(static_cast<std::uintptr_t>(s), stop);
            }
            closesocket(s);
            Bt::Log("host: {} disconnected", peerName);
        }
        if (!stop.stop_requested()) Bt::Log("host: WSA VM ended ({})", peerName);
        NotifyHostChanged();
    }
    WSACleanup();
}

VmStop StopVm(const std::function<void()>& request, unsigned graceMs) {
    const auto vm = FindWsaVm();
    if (!vm) return VmStop::NotRunning;
    const KernelHandle exited(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!exited) return VmStop::Failed;
    VmExitWatch watch(vm->id, exited.get());
    if (!watch.watching()) return VmStop::Failed;
    request();
    if (WaitForSingleObject(exited.get(), graceMs) == WAIT_OBJECT_0) return VmStop::ShutDown;
    if (!watch.Terminate()) return VmStop::Failed;
    return WaitForSingleObject(exited.get(), 15000) == WAIT_OBJECT_0 ? VmStop::Terminated : VmStop::Failed;
}

bool RemoveLegacyServiceRegistration(std::uint32_t port) {
    wchar_t guid[64]{};
    StringFromGUID2(ServiceGuid(port), guid, 64);
    const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\") + guid;
    if (RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str()) != ERROR_SUCCESS) return false;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key.c_str());
    return true;
}

}  // namespace WN::Guest
