// WSA-Next © 2026 Marek Wesołowski - wesmar
// Main window: creation, message loop and window procedure.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "host/Host.h"
#include "gui/Dialogs.h"
#include "support/Config.h"
#include "support/HostEvents.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace WN::Ui {
using namespace MainWindowDetail;

MainWindow::MainWindow(HINSTANCE instance) : m_instance(instance) {}

MainWindow::~MainWindow() {
    m_worker.request_stop();
    // jthread joins here; the window is gone, so anything the worker still posts is freed by PostOwned.
}

bool MainWindow::Create(int showCommand) {
    if (HWND existing = FindWindowW(kClassName, nullptr)) {
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return false;
    }
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = Proc;
    wc.hInstance = m_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));  // transparent on Mica
    wc.lpszClassName = kClassName;
    // Each size drawn from the best image in icon.ico, not a scaled 32 px one.
    LoadIconWithScaleDown(m_instance, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), m_icon.put());
    LoadIconWithScaleDown(m_instance, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                          m_smallIcon.put());
    wc.hIcon = m_icon ? m_icon.get() : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = m_smallIcon.get();
    if (!RegisterClassExW(&wc)) return false;
    if (!Graphics::Get().Ready()) return false;

    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    m_hwnd = CreateWindowExW(0, kClassName, Config::kAppTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 640,
                             720, nullptr, nullptr, m_instance, this);
    if (!m_hwnd) return false;
    RestorePlacement(showCommand);
    Log(Level::Info, Text::Format(IDS_LOG_STARTED, Config::kAppTitle, Config::kAppVersion));
    Refresh();  // Windows and the release are read after the window is visible
    return true;
}

int MainWindow::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Alt+letter opens the client-area menu bar (no native menu to do it).
        if (msg.message == WM_SYSCHAR && GetAncestor(msg.hwnd, GA_ROOT) == m_hwnd &&
            m_menu.OpenByMnemonic(m_hwnd, static_cast<wchar_t>(msg.wParam)))
            continue;
        if (m_accel && TranslateAcceleratorW(m_hwnd, m_accel.get(), &msg)) continue;
        if (IsDialogMessageW(m_hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

void MainWindow::OnCreate() {
    ApplyBackdrop(m_hwnd);
    m_fonts.Create(GetDpiForWindow(m_hwnd));
    BuildMenus();
    BuildAccelerators();

    // Creation order is the Tab order: source, folder, the main buttons, the log expander, the log.
    for (const ButtonSpec& b : kButtons) {
        CreateButton(m_hwnd, static_cast<int>(b.id), b.text ? Text::Get(b.text).c_str() : L"");
        m_looks[b.id] = b.look;
    }
    m_log.Create(m_hwnd, kIdLog);
    m_logVisible = Settings::ReadDword(L"LogVisible", 0) != 0;
    m_looks[CmdExpand].expanded = m_logVisible;
    ShowWindow(m_log.hwnd(), m_logVisible ? SW_SHOW : SW_HIDE);

    const std::wstring folder = Settings::Read(L"InstallFolder");
    m_folder = folder.empty() ? Core::DefaultFolder() : std::filesystem::path(folder);
    m_state = Core::ReadState(m_folder);
    SetWindowTextW(GetDlgItem(m_hwnd, CmdSourceMenu), SourceSummary().c_str());
    SetStatus(Level::Info, Text::Get(IDS_STATUS_READING));
    UpdateControls();
    // The background host runs in its own process: it signals its changes (support/HostEvents.h).
    m_hostChanged.reset(CreateEventW(nullptr, FALSE, FALSE, kHostChangedEvent));
    if (m_hostChanged)
        RegisterWaitForSingleObject(
            &m_hostWait, m_hostChanged.get(),
            [](void* hwnd, BOOLEAN) { PostMessageW(static_cast<HWND>(hwnd), WM_APP_HOST_CHANGED, 0, 0); }, m_hwnd, INFINITE,
            WT_EXECUTEDEFAULT);
    UpdateBluetoothRunning();
    // Windows Hello (and Bluetooth when it is on) for Android: the background host keeps serving
    // them after this window closes.
    if (auto r = Host::Start(); !r) Log(Level::Warning, r.error());
}

LRESULT MainWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == m_taskbarCreated && m_taskbarCreated) {
        if (!m_taskbar) CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(m_taskbar.GetAddressOf()));
        if (m_taskbar && FAILED(m_taskbar->HrInit())) m_taskbar.Reset();
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        OnCreate();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT r{0, 0, m_fonts.S(560), static_cast<LONG>(m_fonts.F(ContentHeight(false)))};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, m_fonts.dpi());
        info->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_DPICHANGED: {
        m_fonts.Create(HIWORD(wParam));
        m_menu.Remeasure();
        m_log.Relayout();
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOMPOSITIONCHANGED:
        ApplyBackdrop(m_hwnd);
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    case WM_MOUSEMOVE: {
        if (!m_trackingMouse) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, m_hwnd, 0};
            m_trackingMouse = TrackMouseEvent(&tme) != FALSE;
        }
        if (m_menu.SetHot(m_menu.HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}))) InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        if (m_menu.SetHot(-1)) InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const int index = m_menu.HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (index >= 0) m_menu.Open(m_hwnd, index, false);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0) {  // Alt alone or F10
            m_menu.Open(m_hwnd, 0, true);
            return 0;
        }
        if ((wParam & 0xFFF0) == SC_KEYMENU && lParam != L' ' && m_menu.OpenByMnemonic(m_hwnd, static_cast<wchar_t>(lParam))) return 0;
        break;
    case WM_MENUSELECT:
        m_menu.OnMenuSelect(wParam, lParam);
        return 0;
    case WM_INITMENUPOPUP:  // every popup (menu bar, source combo, context menus) is updated as it opens
        ApplyMenuState(reinterpret_cast<HMENU>(wParam));
        return 0;
    case WM_MEASUREITEM: {
        auto& mis = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis.CtlType == ODT_MENU) m_menu.Measure(mis);
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto& dis = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (dis.CtlType == ODT_MENU) m_menu.Draw(dis);
        else if (auto it = m_looks.find(dis.CtlID); it != m_looks.end()) DrawButton(dis, m_fonts, it->second);
        return TRUE;
    }
    case WM_CONTEXTMENU: {
        HWND target = reinterpret_cast<HWND>(wParam);
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (p.x == -1 && p.y == -1) {  // keyboard: below the focused control
            RECT r{};
            GetWindowRect(target, &r);
            p = {r.left + m_fonts.S(24), r.top + m_fonts.S(24)};
        }
        ShowContextMenu(target, p);
        return 0;
    }
    case WM_COMMAND:
        OnCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_APP_HOST_CHANGED:
        UpdateBluetoothRunning();
        return 0;
    case WM_ACTIVATE:
        // Besides the host's notifications: a host that ended without telling (killed) is seen here.
        if (LOWORD(wParam) != WA_INACTIVE) UpdateBluetoothRunning();
        return DefWindowProcW(m_hwnd, message, wParam, lParam);
    case WM_TIMER:
        if (wParam == kAnimationTimer) {
            const float target = m_progress.permille >= 0 ? static_cast<float>(m_progress.permille) / 1000.0f : m_shown;
            m_shown += (target - m_shown) * 0.18f;
            if (std::abs(target - m_shown) < 0.001f) m_shown = target;
            const bool indeterminate = (m_busy || m_refreshing) && m_progress.permille < 0;
            if (!indeterminate && m_shown == target) KillTimer(m_hwnd, kAnimationTimer);
            const RECT r{static_cast<LONG>(m_progressRect.left) - 1, static_cast<LONG>(m_progressRect.top) - 1,
                         static_cast<LONG>(m_progressRect.right) + 1, static_cast<LONG>(m_progressRect.bottom) + 1};
            InvalidateRect(m_hwnd, &r, FALSE);
        }
        return 0;
    case WM_APP_REFRESHED: {
        auto msg = TakeOwned<RefreshMessage>(lParam);
        if (m_worker.joinable() && !m_busy) m_worker.join();
        m_btAdapter = msg->btAdapter;
        m_btAutostart = msg->btAutostart;
        m_btKnown = true;
        OnRefreshed(std::move(msg->system), std::move(msg->manifest), std::move(msg->manifestError));
        return 0;
    }
    case WM_APP_LOG: {
        auto msg = TakeOwned<LogMessage>(lParam);
        if (msg->level == Level::Warning || msg->level == Level::Error) SetStatus(msg->level, msg->text);
        Log(msg->level, std::move(msg->text));
        return 0;
    }
    case WM_APP_PROGRESS: {
        auto p = TakeOwned<Core::Progress>(lParam);
        m_progress = *p;
        if (p->permille < 0) SetTaskbar(TBPF_INDETERMINATE);
        else SetTaskbar(TBPF_NORMAL, p->permille);
        SetStatus(Level::Info, ProgressText(*p));
        StartAnimation();
        return 0;
    }
    case WM_APP_DONE: {
        auto done = TakeOwned<DoneMessage>(lParam);
        if (m_worker.joinable()) m_worker.join();  // the worker has finished its job by now
        if (done->stop) OnStopDone(*done->stop);
        else if (done->removal) OnUninstallDone(done->ok, done->everything, done->restart, done->leftovers, done->error);
        else OnInstallDone(done->ok, done->canceled, done->restart, done->error);
        return 0;
    }
    case WM_APP_CONFIRM: {
        if (m_closing) return 0;
        const auto& question = *reinterpret_cast<const std::wstring*>(lParam);
        const bool yes = Confirm(m_hwnd, Text::Get(IDS_CONFIRM_HEADING), question, Text::Get(IDS_CONTINUE));
        return yes ? 1 : 0;
    }
    case WM_CLOSE:
        if (m_removing) return 0;  // a removal takes seconds and cannot stop half-way
        if (m_busy && !Confirm(m_hwnd, Text::Get(IDS_CLOSE_HEADING), Text::Get(IDS_CLOSE_MESSAGE), Text::Get(IDS_CLOSE_OK))) return 0;
        m_closing = true;
        m_worker.request_stop();
        if (!m_forgetSettings) SavePlacement();  // after "Remove everything" nothing is written again
        DestroyWindow(m_hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(m_hwnd, kAnimationTimer);
        if (m_hostWait) {
            UnregisterWaitEx(m_hostWait, INVALID_HANDLE_VALUE);  // waits for a callback in flight
            m_hostWait = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

}  // namespace WN::Ui
