// WSA-Next © 2026 Marek Wesołowski - wesmar
// The background host's icon in the notification area: the state at a glance (Android, Bluetooth,
// root), quick switches, and a notification whenever an Android app was granted root - so every
// grant stays visible after the fact. Its own thread and a hidden window; it reacts to the host's
// change notices and never polls. The host runs elevated: the icon's callback message and
// "TaskbarCreated" (Explorer restarted) are let through the message filter explicitly.
#pragma once
#include <Windows.h>
#include <string>
#include <thread>

namespace WN::Host {

class Tray {
public:
    explicit Tray(HANDLE stopHost);  // shows the icon (its thread starts at once)
    ~Tray();                         // removes it
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

private:
    void Run();
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    void AddIcon();
    void ShowBalloon(const std::wstring& title, const std::wstring& text);
    void Refresh();
    void ShowMenu();
    void Command(UINT id);

    HANDLE m_stopHost;
    HWND m_hwnd = nullptr;
    UINT m_taskbarCreated = 0;
    bool m_registrationShown = false;  // the last notification offers Google Play registration
    std::jthread m_thread;
};

}  // namespace WN::Host
