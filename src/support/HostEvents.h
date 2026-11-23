// WSA-Next © 2026 Marek Wesołowski - wesmar
// Named events between the background host (src/host) and the window, instead of polling:
// the host and its bridges signal "something changed" (Bluetooth bridge started or stopped,
// Android connected or gone, WSA's VM up or down, host ending); the window waits for it and
// reads the state again. Inside the host, its notification-area icon listens the same way.
#pragma once
#include <Windows.h>
#include <atomic>
#include <string>

namespace WN {

// Auto-reset; created by the window (the only waiter). Signaling it without a window is a no-op.
inline constexpr wchar_t kHostChangedEvent[] = L"Local\\WSA-Next.Host.Changed";

// In-process listeners of the background host (its icon): set while it exists.
inline std::atomic<void (*)()> g_hostChangedListener{nullptr};
inline std::atomic<void (*)(const std::wstring& title, const std::wstring& text)> g_hostNoticeListener{nullptr};

inline void NotifyHostChanged() {
    if (const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kHostChangedEvent)) {
        SetEvent(event);
        CloseHandle(event);
    }
    if (const auto listener = g_hostChangedListener.load()) listener();
}

inline std::atomic<bool (*)()> g_hostRegistrationListener{nullptr};

// Something the user should see happen (a root grant): a notification from the host's icon.
inline void HostNotice(const std::wstring& title, const std::wstring& text) {
    if (const auto listener = g_hostNoticeListener.load()) listener(title, text);
}

// "Register this PC with Google Play" from the host's icon (host/Registration.h); false without
// the icon.
inline bool HostRegistrationNotice() {
    const auto listener = g_hostRegistrationListener.load();
    return listener && listener();
}

}  // namespace WN
