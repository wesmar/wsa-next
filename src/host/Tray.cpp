// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "host/Tray.h"
#include "bluetooth/Bridge.h"
#include "bluetooth/Log.h"
#include "host/Guest.h"
#include "host/Host.h"
#include "host/Registration.h"
#include "install/Package.h"
#include "resources/resource.h"
#include "support/HostEvents.h"
#include "support/Text.h"
#include <shellapi.h>
#include <commctrl.h>
#include <memory>
#include <string>

#pragma comment(lib, "comctl32.lib")

namespace WN::Host {
namespace {

constexpr wchar_t kClass[] = L"WSA-Next.Tray";
constexpr UINT kIconId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;  // from Explorer: clicks on the icon
constexpr UINT kRefresh = WM_APP + 2;      // from the host: the state changed
constexpr UINT kNotice = WM_APP + 3;       // from a bridge: a notification to show
constexpr UINT kRegistration = WM_APP + 4; // from the registration helper: offer Google Play registration

enum : UINT { CmdOpen = 1, CmdRegister, CmdBluetooth, CmdRoot, CmdSu, CmdStopWsa, CmdLog, CmdHide, CmdExit };

struct Notice {
    std::wstring title, text;
};

// The one icon of the process; the listeners reach its window through this.
std::atomic<HWND> g_window{nullptr};

void OnHostChanged() {
    if (const HWND hwnd = g_window.load()) PostMessageW(hwnd, kRefresh, 0, 0);
}

void OnNotice(const std::wstring& title, const std::wstring& text) {
    const HWND hwnd = g_window.load();
    if (!hwnd) return;
    auto notice = std::make_unique<Notice>(Notice{title, text});
    if (PostMessageW(hwnd, kNotice, 0, reinterpret_cast<LPARAM>(notice.get()))) notice.release();
}

bool OnRegistration() {
    const HWND hwnd = g_window.load();
    return hwnd && PostMessageW(hwnd, kRegistration, 0, 0);
}

// A notification-area string: at most `size` characters with the terminator.
template <std::size_t N>
void Copy(wchar_t (&field)[N], const std::wstring& text) {
    wcsncpy_s(field, text.c_str(), _TRUNCATE);
}

}  // namespace

Tray::Tray(HANDLE stopHost) : m_stopHost(stopHost) {
    m_thread = std::jthread([this] { Run(); });
}

Tray::~Tray() {
    if (const HWND hwnd = g_window.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (m_thread.joinable()) m_thread.join();
}

void Tray::Run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &Tray::Proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // a second icon in the same process finds it registered
    // A hidden top-level window (a message-only one would miss "TaskbarCreated", a broadcast).
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"WSA-Next", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!m_hwnd) return;
    // Elevated process, Explorer at medium integrity: let its messages for the icon through.
    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(m_hwnd, kTrayMessage, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, m_taskbarCreated, MSGFLT_ALLOW, nullptr);
    AddIcon();
    g_window = m_hwnd;
    g_hostChangedListener = &OnHostChanged;
    g_hostNoticeListener = &OnNotice;
    g_hostRegistrationListener = &OnRegistration;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK Tray::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        // The first message, before CreateWindowExW returns: the window is known from here on
        // (DefWindowProc must get it - with no window it fails the creation).
        auto* created = reinterpret_cast<Tray*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        created->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Tray::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == m_taskbarCreated && m_taskbarCreated) {  // Explorer restarted: the icon is gone
        AddIcon();
        return 0;
    }
    switch (message) {
    case kTrayMessage:
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK: Command(CmdOpen); break;
        case WM_CONTEXTMENU: ShowMenu(); break;
        case NIN_BALLOONUSERCLICK:
            if (m_registrationShown) Command(CmdRegister);
            break;
        default: break;
        }
        return 0;
    case kRefresh:
        Refresh();
        return 0;
    case kNotice: {
        const std::unique_ptr<Notice> notice(reinterpret_cast<Notice*>(lParam));
        ShowBalloon(notice->title, notice->text);
        m_registrationShown = false;
        return 0;
    }
    case kRegistration:
        ShowBalloon(Text::Get(IDS_TRAY_REGISTER_TITLE), Text::Get(IDS_TRAY_REGISTER_TEXT));
        m_registrationShown = true;  // a click on it registers
        return 0;
    case WM_COMMAND:
        Command(LOWORD(wParam));
        return 0;
    case WM_CLOSE:
        DestroyWindow(m_hwnd);
        return 0;
    case WM_DESTROY: {
        g_hostChangedListener = nullptr;
        g_hostNoticeListener = nullptr;
        g_hostRegistrationListener = nullptr;
        g_window = nullptr;
        NOTIFYICONDATAW data{sizeof data};
        data.hWnd = m_hwnd;
        data.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        PostQuitMessage(0);
        return 0;
    }
    default: return DefWindowProcW(m_hwnd, message, wParam, lParam);
    }
}

void Tray::AddIcon() {
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = m_hwnd;
    data.uID = kIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &data.hIcon);
    Copy(data.szTip, L"WSA-Next");
    Shell_NotifyIconW(NIM_DELETE, &data);  // a leftover of an earlier instance
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    if (data.hIcon) DestroyIcon(data.hIcon);
    Refresh();
}

void Tray::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = m_hwnd;
    data.uID = kIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    Copy(data.szInfoTitle, title);
    Copy(data.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

// The tooltip: what runs. Read on each change notice (WSA's VM, the bridges, the settings).
void Tray::Refresh() {
    const bool android = Guest::FindWsaVm().has_value();
    const UINT bluetooth = !BluetoothEnabled() ? IDS_TRAY_BT_OFF : Bt::AndroidConnected() ? IDS_TRAY_BT_CONNECTED : IDS_TRAY_BT_ON;
    const std::wstring tip = L"WSA-Next\n" + Text::Get(android ? IDS_TRAY_ANDROID_ON : IDS_TRAY_ANDROID_OFF) + L"\n" + Text::Get(bluetooth) + L"\n" +
                             Text::Get(ElevationEnabled() ? IDS_TRAY_ROOT_ON : IDS_TRAY_ROOT_OFF);
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = m_hwnd;
    data.uID = kIconId;
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    Copy(data.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::ShowMenu() {
    const HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const bool root = ElevationEnabled();
    const bool android = Guest::FindWsaVm().has_value();
    auto item = [&](UINT id, UINT text, bool checked = false, bool enabled = true) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, Text::Get(text).c_str());
    };
    item(CmdOpen, IDS_TRAY_OPEN);
    SetMenuDefaultItem(menu, CmdOpen, FALSE);
    if (!PendingRegistrationId().empty()) item(CmdRegister, IDS_TRAY_REGISTER);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(CmdBluetooth, IDS_TRAY_BLUETOOTH, BluetoothEnabled());
    item(CmdRoot, IDS_MI_ELEVATION, root);
    item(CmdSu, IDS_MI_ELEVATION_SU, ElevationSuEnabled(), root);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(CmdStopWsa, IDS_MI_STOP_WSA, false, android);
    item(CmdLog, IDS_MI_BT_LOG);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    item(CmdHide, IDS_TRAY_HIDE);
    item(CmdExit, IDS_TRAY_EXIT);
    POINT at{};
    GetCursorPos(&at);
    SetForegroundWindow(m_hwnd);  // so that the menu closes when the user clicks elsewhere
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void Tray::Command(UINT id) {
    switch (id) {
    case CmdOpen:
        if (HWND existing = FindWindowW(L"WSANext.MainWindow", nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        } else {
            ShellExecuteW(nullptr, L"open", Executable().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    case CmdRegister:
        m_registrationShown = false;
        (void)RegisterWithGoogle(m_hwnd);
        break;
    case CmdBluetooth: (void)SetBluetoothEnabled(!BluetoothEnabled()); break;
    case CmdRoot:
        if (ElevationEnabled()) {
            (void)SetElevationEnabled(false);
        } else if (MessageBoxW(m_hwnd, Text::Get(IDS_ELEVATION_TEXT).c_str(), Text::Get(IDS_ELEVATION_HEADING).c_str(),
                               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDYES) {
            (void)SetElevationEnabled(true);
        }
        break;
    case CmdSu: (void)SetElevationSuEnabled(!ElevationSuEnabled()); break;
    case CmdStopWsa:
        // Up to half a minute: off the window's thread. The VM's end comes back as a change notice.
        std::thread([] { (void)Install::Stop(); }).detach();
        break;
    case CmdLog: ShellExecuteW(nullptr, L"open", (Bt::LogFolder() / L"bluetooth.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case CmdHide: (void)SetTrayEnabled(false); break;
    case CmdExit: SetEvent(m_stopHost); break;
    default: break;
    }
    Refresh();
}

}  // namespace WN::Host
