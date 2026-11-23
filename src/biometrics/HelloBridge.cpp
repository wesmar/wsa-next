// WSA-Next © 2026 Marek Wesołowski - wesmar
// Windows Hello bridge: Android's fingerprint HAL in WSA <-> Hyper-V socket ("WSFP") <-> Windows Hello.
#include <winsock2.h>
#include <Windows.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include "biometrics/HelloBridge.h"
#include "biometrics/Protocol.h"
#include "biometrics/WindowsHello.h"
#include "bluetooth/Log.h"
#include "host/Guest.h"
#include "host/Host.h"
#include "support/Common.h"
#include "support/Handle.h"
#include "support/Text.h"
#include <wtsapi32.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace WN::Bio {
namespace {

// GUID_DEVINTERFACE_BIOMETRIC_READER (winbio_ioctl.h).
constexpr GUID kBiometricReader{0xe2b5183a, 0x99ea, 0x4cc3, {0xad, 0x6b, 0x80, 0xca, 0x8d, 0x71, 0x5b, 0x80}};

// Blocking: a stop request shuts the socket down (Guest::Serve), which ends the read.
bool RecvExact(SOCKET s, void* dst, std::size_t size) {
    auto* out = static_cast<char*>(dst);
    std::size_t done = 0;
    while (done < size) {
        const int n = recv(s, out + done, static_cast<int>(size - done), 0);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

Status MapVerify(VerifyResult r) {
    switch (r) {
    case VerifyResult::Verified: return Status::Verified;
    case VerifyResult::Canceled: return Status::Canceled;
    case VerifyResult::NotAvailable: return Status::NotAvailable;
    case VerifyResult::Busy: return Status::Busy;
    default: return Status::Failed;
    }
}

// Signals `changed` whenever Windows Hello may have become available or unavailable: the
// Windows Biometric Service starting or stopping (service notification) and a biometric reader
// arriving or leaving (device interface notification). Nothing is polled.
class HelloChangeWatch {
public:
    explicit HelloChangeWatch(HANDLE changed) : m_changed(changed) {
        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof filter;
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        filter.u.DeviceInterface.ClassGuid = kBiometricReader;
        if (CM_Register_Notification(&filter, changed, &HelloChangeWatch::OnDevice, &m_device) != CR_SUCCESS) m_device = nullptr;
        m_service = std::jthread([this](std::stop_token stop) { WatchService(stop); });
    }
    ~HelloChangeWatch() {
        if (m_device) CM_Unregister_Notification(m_device);
        m_service.request_stop();
        QueueUserAPC([](ULONG_PTR) {}, m_service.native_handle(), 0);  // ends the alertable wait
    }
    HelloChangeWatch(const HelloChangeWatch&) = delete;
    HelloChangeWatch& operator=(const HelloChangeWatch&) = delete;

private:
    static DWORD CALLBACK OnDevice(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION, PCM_NOTIFY_EVENT_DATA, DWORD) {
        SetEvent(static_cast<HANDLE>(context));
        return ERROR_SUCCESS;
    }
    static void CALLBACK OnService(PVOID parameter) { SetEvent(static_cast<SERVICE_NOTIFYW*>(parameter)->pContext); }

    void WatchService(std::stop_token stop) {
        const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        const SC_HANDLE service = manager ? OpenServiceW(manager, L"WbioSrvc", SERVICE_QUERY_STATUS) : nullptr;
        while (service && !stop.stop_requested()) {
            SERVICE_NOTIFYW notify{};
            notify.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
            notify.pfnNotifyCallback = &HelloChangeWatch::OnService;
            notify.pContext = m_changed;
            if (NotifyServiceStatusChangeW(service, SERVICE_NOTIFY_RUNNING | SERVICE_NOTIFY_STOPPED, &notify) != ERROR_SUCCESS) break;
            SleepEx(INFINITE, TRUE);  // until the notification (or the destructor's) APC runs
        }
        if (service) CloseServiceHandle(service);  // also cancels a pending notification
        if (manager) CloseServiceHandle(manager);
    }

    HANDLE m_changed;
    HCMNOTIFICATION m_device = nullptr;
    std::jthread m_service;
};

// A remembered verification (setting "HelloRemember", Host::HelloRememberMinutes): when the last
// Windows Hello verification for Android apps succeeded, and in which locking period of this
// Windows session. Only this bridge uses it - root for Android apps always asks.
std::mutex g_rememberLock;
bool g_remembered = false;
ULONGLONG g_verifiedAt = 0;          // GetTickCount64: the time in sleep counts too
std::uint64_t g_verifiedEpoch = 0;
std::atomic<std::uint64_t> g_lockEpoch{0};  // advanced when Windows locks, signs out or disconnects
std::atomic<bool> g_lockWatched{false};     // without the lock watch nothing is remembered

// True when the setting lets a verification of this unlocked period stand for a new request.
bool Remembered() {
    const std::uint32_t minutes = Host::HelloRememberMinutes();
    std::lock_guard guard(g_rememberLock);
    if (minutes == 0 || !g_lockWatched || !g_remembered || g_verifiedEpoch != g_lockEpoch.load()) {
        g_remembered = false;
        return false;
    }
    return minutes == Host::kHelloUntilLocked || GetTickCount64() - g_verifiedAt < static_cast<ULONGLONG>(minutes) * 60'000;
}

void Remember(VerifyResult result) {
    std::lock_guard guard(g_rememberLock);
    if (result == VerifyResult::Verified) {
        g_remembered = true;
        g_verifiedAt = GetTickCount64();
        g_verifiedEpoch = g_lockEpoch.load();
    } else if (result == VerifyResult::Failed) {
        g_remembered = false;  // a wrong finger or PIN: ask again next time
    }
}

// Session notifications (WTSRegisterSessionNotification) on a message-only window: locking Windows
// (Win+L, the lock after sleep), signing out and disconnecting end the remembered verification.
class SessionLockWatch {
public:
    SessionLockWatch() {
        m_thread = std::jthread([this] { Run(); });
    }
    // Either this sees the window, or the thread sees the flag right after creating it.
    ~SessionLockWatch() {
        m_stop = true;
        if (const HWND hwnd = m_hwnd.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    SessionLockWatch(const SessionLockWatch&) = delete;
    SessionLockWatch& operator=(const SessionLockWatch&) = delete;

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_LOGOFF || wParam == WTS_CONSOLE_DISCONNECT ||
                wParam == WTS_REMOTE_DISCONNECT) {
                ++g_lockEpoch;
                Bt::Log("hello: Windows locked or left, a remembered verification is forgotten");
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            WTSUnRegisterSessionNotification(hwnd);
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void Run() {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &SessionLockWatch::Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WSA-Next.HelloLockWatch";
        RegisterClassExW(&wc);  // registered already when the bridge runs again
        const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!hwnd) return;
        if (!WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)) {
            Bt::Log("hello: no session notifications (error {}), Windows Hello is never remembered", GetLastError());
            DestroyWindow(hwnd);
            return;
        }
        m_hwnd = hwnd;
        if (m_stop) DestroyWindow(hwnd);  // the bridge stopped meanwhile: the loop ends at once
        else g_lockWatched = true;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        g_lockWatched = false;
        ++g_lockEpoch;  // the bridge stops: nothing carries over to the next one
        m_hwnd = nullptr;
    }

    std::atomic<HWND> m_hwnd{nullptr};
    std::atomic<bool> m_stop{false};
    std::jthread m_thread;  // last: joined first, while the members above still exist
};

// A worker thread in the multithreaded apartment that WinRT needs.
template <class Work>
std::jthread StartMta(Work work) {
    return std::jthread([work](std::stop_token stop) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        work(stop);
        if (SUCCEEDED(com)) CoUninitialize();
    });
}

// One connection to the HAL: availability pushed on connecting and on every change, verification
// requests answered (one at a time, cancelable).
void RunSession(std::uintptr_t handle, std::stop_token) {
    const auto socket = static_cast<SOCKET>(handle);
    std::mutex sendLock;
    auto send = [&](Op op, Status status) {
        const FrameHeader frame{kMagic, static_cast<std::uint8_t>(op), static_cast<std::uint8_t>(status), 0};
        const auto* bytes = reinterpret_cast<const char*>(&frame);
        std::lock_guard guard(sendLock);
        for (std::size_t done = 0; done < sizeof frame;) {  // a stream may take fewer bytes per call
            const int n = ::send(socket, bytes + done, static_cast<int>(sizeof frame - done), 0);
            if (n <= 0) return;
            done += static_cast<std::size_t>(n);
        }
    };

    const KernelHandle changed(CreateEventW(nullptr, FALSE, TRUE, nullptr));  // signaled: push the state at once
    const HelloChangeWatch watch(changed.get());
    std::jthread pusher = StartMta([&](std::stop_token stop) {
        const KernelHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        const std::stop_callback onStop(stop, [event = stopEvent.get()] { SetEvent(event); });
        int last = -1;
        for (;;) {
            const HANDLE events[] = {stopEvent.get(), changed.get()};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) return;
            const int now = CheckAvailability() == HelloStatus::Available ? 1 : 0;
            if (now == last) continue;
            last = now;
            Bt::Log("hello: Windows Hello {}", now ? "available" : "not available");
            send(Op::Availability, now ? Status::Ok : Status::NotAvailable);
        }
    });

    std::atomic<bool> verifying{false};
    std::stop_source cancel;
    std::jthread verifier;
    for (;;) {
        FrameHeader request{};
        if (!RecvExact(socket, &request, sizeof request)) break;
        if (request.magic != kMagic || request.payloadLen > kMaxPayload) {
            Bt::Log("hello: invalid frame (magic=0x{:08X}, len={}), dropping the connection", request.magic, request.payloadLen);
            break;
        }
        std::string payload(request.payloadLen, '\0');
        if (request.payloadLen > 0 && !RecvExact(socket, payload.data(), request.payloadLen)) break;

        switch (static_cast<Op>(request.op)) {
        case Op::Authenticate: {
            if (verifying.exchange(true)) {
                send(Op::Authenticate, Status::Busy);
                break;
            }
            if (verifier.joinable()) verifier.join();  // the previous verification has answered
            if (Remembered()) {
                Bt::Log("hello: verification requested, remembered (setting HelloRemember)");
                send(Op::Authenticate, Status::Verified);
                verifying = false;
                break;
            }
            cancel = std::stop_source();
            // The host's own text: what the guest sends is not shown in a trusted Windows dialog.
            const std::wstring prompt = Text::Get(IDS_HELLO_PROMPT);
            Bt::Log("hello: verification requested");
            verifier = StartMta([&, prompt, token = cancel.get_token()](std::stop_token) {
                const VerifyResult result = RequestVerification(prompt, token);
                Bt::Log("hello: verification -> {}", static_cast<int>(result));
                Remember(result);
                send(Op::Authenticate, MapVerify(result));
                verifying = false;
                SetEvent(changed.get());  // the dialog may have shown that Windows Hello is gone
            });
            break;
        }
        case Op::Cancel:
            cancel.request_stop();
            break;
        default:
            break;
        }
    }
    cancel.request_stop();  // the HAL is gone: close a dialog that is still open
}

}  // namespace

BridgeResult RunBridge(std::stop_token stop) {
    if (!IsElevated()) {
        Bt::Log("hello: administrator rights are required (Host Compute Service)");
        return BridgeResult::NotElevated;
    }
    const KernelHandle guard(CreateMutexW(nullptr, FALSE, L"Local\\WSA-Next.HelloBridge"));
    const DWORD error = GetLastError();
    if (!guard) {
        Bt::Log("hello: cannot create the instance mutex (error {})", error);
        return BridgeResult::Stopped;
    }
    if (error == ERROR_ALREADY_EXISTS) return BridgeResult::Stopped;  // another instance serves Windows Hello
    Bt::Log("Windows Hello bridge ready (vsock port 0x{:08X})", kBridgePort);
    const SessionLockWatch lockWatch;
    Guest::Serve(kBridgePort, "Android fingerprint HAL", RunSession, stop);
    Bt::Log("Windows Hello bridge stopped");
    return BridgeResult::Stopped;
}

}  // namespace WN::Bio
