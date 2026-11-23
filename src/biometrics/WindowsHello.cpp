// WSA-Next © 2026 Marek Wesołowski - wesmar
// Windows Hello user verification through WinRT UserConsentVerifier.
#include "biometrics/WindowsHello.h"
#include <Windows.h>
#include <UserConsentVerifierInterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>
#include <memory>

#pragma comment(lib, "windowsapp.lib")

namespace WN::Bio {
using namespace winrt::Windows::Security::Credentials::UI;

HelloStatus CheckAvailability() {
    try {
        switch (UserConsentVerifier::CheckAvailabilityAsync().get()) {
        case UserConsentVerifierAvailability::Available: return HelloStatus::Available;
        case UserConsentVerifierAvailability::DeviceNotPresent: return HelloStatus::DeviceNotPresent;
        case UserConsentVerifierAvailability::NotConfiguredForUser: return HelloStatus::NotConfigured;
        case UserConsentVerifierAvailability::DisabledByPolicy: return HelloStatus::DisabledByPolicy;
        case UserConsentVerifierAvailability::DeviceBusy: return HelloStatus::DeviceBusy;
        default: return HelloStatus::Error;
        }
    } catch (...) {
        return HelloStatus::Error;
    }
}

std::wstring StatusText(HelloStatus status) {
    switch (status) {
    case HelloStatus::Available: return L"available (fingerprint / Windows Hello ready)";
    case HelloStatus::DeviceNotPresent: return L"no biometric / Windows Hello device present";
    case HelloStatus::NotConfigured: return L"not configured for this Windows user (set up Windows Hello PIN/fingerprint in Settings)";
    case HelloStatus::DisabledByPolicy: return L"disabled by Windows policy";
    case HelloStatus::DeviceBusy: return L"biometric device is busy";
    default: return L"unavailable (WinRT check failed)";
    }
}

namespace {

// A desktop process has no CoreWindow: Windows Hello wants a window to own its dialog
// (IUserConsentVerifierInterop). The background host has none either, so a small invisible
// top-level window serves as the owner, centred on the primary monitor.
class OwnerWindow {
public:
    OwnerWindow() {
        const int x = GetSystemMetrics(SM_CXSCREEN) / 2, y = GetSystemMetrics(SM_CYSCREEN) / 2;
        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, L"STATIC", L"WSA-Next", WS_POPUP, x, y, 1, 1,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (m_hwnd) {
            SetLayeredWindowAttributes(m_hwnd, 0, 0, LWA_ALPHA);  // fully transparent
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            SetForegroundWindow(m_hwnd);
        }
    }
    ~OwnerWindow() {
        if (m_hwnd) DestroyWindow(m_hwnd);
    }
    OwnerWindow(const OwnerWindow&) = delete;
    OwnerWindow& operator=(const OwnerWindow&) = delete;
    [[nodiscard]] HWND get() const { return m_hwnd; }

private:
    HWND m_hwnd = nullptr;
};

// Waits for the dialog's answer; `cancel` closes the dialog (the answer is then "canceled").
UserConsentVerificationResult Verify(const winrt::hstring& prompt, std::stop_token cancel) {
    const OwnerWindow owner;
    winrt::Windows::Foundation::IAsyncOperation<UserConsentVerificationResult> operation;
    if (owner.get()) {
        const auto interop = winrt::get_activation_factory<UserConsentVerifier, IUserConsentVerifierInterop>();
        winrt::check_hresult(interop->RequestVerificationForWindowAsync(owner.get(), static_cast<HSTRING>(winrt::get_abi(prompt)),
                                                                         winrt::guid_of<decltype(operation)>(), winrt::put_abi(operation)));
    } else {
        operation = UserConsentVerifier::RequestVerificationAsync(prompt);
    }
    const std::stop_callback onCancel(cancel, [operation] { operation.Cancel(); });
    // The owner window lives on this thread: keep its messages flowing until Windows Hello answers.
    // The completion handler owns the event too, so it never signals a closed (or reused) handle.
    const auto done = std::make_shared<winrt::handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!*done) winrt::throw_last_error();
    operation.Completed([done](auto&&, auto&&) { SetEvent(done->get()); });
    for (;;) {
        const HANDLE events[] = {done->get()};
        const DWORD wait = MsgWaitForMultipleObjects(1, events, FALSE, INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) winrt::throw_last_error();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Canceled) return UserConsentVerificationResult::Canceled;
    return operation.GetResults();
}

}  // namespace

VerifyResult RequestVerification(std::wstring_view message, std::stop_token cancel) {
    try {
        const winrt::hstring prompt = message.empty() ? winrt::hstring(L"Android (WSA-Next) requests Windows Hello verification")
                                                      : winrt::hstring(message);
        switch (Verify(prompt, cancel)) {
        case UserConsentVerificationResult::Verified: return VerifyResult::Verified;
        case UserConsentVerificationResult::Canceled: return VerifyResult::Canceled;
        case UserConsentVerificationResult::DeviceNotPresent:
        case UserConsentVerificationResult::NotConfiguredForUser:
        case UserConsentVerificationResult::DisabledByPolicy: return VerifyResult::NotAvailable;
        case UserConsentVerificationResult::DeviceBusy: return VerifyResult::Busy;
        default: return VerifyResult::Failed;
        }
    } catch (...) {
        return VerifyResult::Failed;
    }
}

}  // namespace WN::Bio
