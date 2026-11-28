// WSA-Next © 2026 Marek Wesołowski - wesmar
// Main window: what is known about Windows and the release, what is possible right now,
// status and log.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "app/Cli.h"
#include "bluetooth/Bridge.h"
#include "bluetooth/Log.h"
#include "core/Release.h"
#include "support/Signature.h"
#include "bluetooth/Scanner.h"
#include "host/Host.h"
#include "core/Uninstall.h"
#include "support/Config.h"
#include "support/HostEvents.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <objbase.h>
#include <algorithm>
#include <format>

namespace WN::Ui {
using namespace MainWindowDetail;

std::wstring MainWindow::SourceKey() const {
    const std::wstring key = Settings::Read(L"Source", L"github");
    return key == L"local" || key == L"mirror" || key == L"custom" ? key : std::wstring(L"github");
}

std::wstring MainWindow::SourceUrl() const {
    const std::wstring key = SourceKey();
    return key == L"custom" ? Settings::Read(L"CustomManifest") : Cli::SourceUrl(key);
}

std::wstring MainWindow::SourceSummary() const {
    const std::wstring key = SourceKey();
    if (key == L"local") return Text::Format(IDS_SOURCE_LOCAL, Config::kLocalManifest);
    if (key == L"mirror") return Text::Format(IDS_SOURCE_MIRROR, Config::kMirrorManifest);
    if (key == L"custom") return Text::Format(IDS_SOURCE_CUSTOM, Settings::Read(L"CustomManifest"));
    return Text::Format(IDS_SOURCE_GITHUB, Config::kGitHubRepo);
}

std::wstring MainWindow::ComponentName(const Core::Component& c) const {
    static constexpr std::pair<std::wstring_view, UINT> kNames[] = {
        {L"system", IDS_COMPNAME_SYSTEM}, {L"system_ext", IDS_COMPNAME_SYSTEM_EXT}, {L"product", IDS_COMPNAME_PRODUCT},
        {L"vendor", IDS_COMPNAME_VENDOR}, {L"kernel", IDS_COMPNAME_KERNEL},         {L"initrd", IDS_COMPNAME_INITRD},
        {L"windows", IDS_COMPNAME_WINDOWS}, {L"wsapatch", IDS_COMPNAME_WSAPATCH},
    };
    for (const auto& [id, text] : kNames)
        if (c.id == id) return Text::Get(text);
    return c.id;
}

// Windows and the manifest are read on a worker (WMI, package manager and the network block).
// After a job the bar keeps showing its result; otherwise it slides while reading.
void MainWindow::Refresh(bool keepResult) {
    if (m_busy || m_refreshing) return;
    m_refreshing = true;
    if (!keepResult) m_progress = {};
    m_animStart = GetTickCount64();
    StartAnimation();
    SetStatus(Level::Info, Text::Get(IDS_STATUS_READING));
    UpdateControls();
    if (m_worker.joinable()) m_worker.join();
    HWND hwnd = m_hwnd;
    const std::wstring url = SourceUrl();
    m_worker = std::jthread([hwnd, url](std::stop_token stop) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto msg = std::make_unique<RefreshMessage>();
        msg->system = Core::Inspect();
        msg->btAdapter = Bt::Scanner::AdapterAddress();
        msg->btAutostart = Host::AutostartEnabled();
        if (url.empty()) {
            msg->manifestError = Text::Get(IDS_CUSTOM_INVALID);
        } else if (auto m = Core::LoadManifest(url, stop)) {
            msg->manifest = std::move(*m);
        } else {
            msg->manifestError = m.error();
        }
        if (SUCCEEDED(com)) CoUninitialize();
        PostOwned(hwnd, WM_APP_REFRESHED, std::move(msg));
    });
}

void MainWindow::OnRefreshed(Core::SystemInfo system, std::optional<Core::Manifest> manifest, std::wstring error) {
    m_refreshing = false;
    m_system = std::move(system);
    m_manifest = std::move(manifest);
    m_manifestError = std::move(error);
    m_state = Core::ReadState(m_folder);
    if (m_manifest) {
        Log(Level::Info, Text::Format(IDS_LOG_MANIFEST, m_manifest->version, m_manifest->android, m_manifest->variant, m_manifest->source));
    } else {
        Log(Level::Error, Text::Format(IDS_STATUS_NO_MANIFEST, m_manifestError));
    }
    Log(Level::Info, Text::Format(IDS_LOG_WINDOWS, m_system->Version()));
    UpdatePlan();
    StatusFromPlan();
    Layout();  // the components card grows with the release
    UpdateControls();
}

void MainWindow::UpdatePlan() {
    m_plan = m_manifest && m_system ? Core::MakePlan(*m_manifest, m_state, *m_system, m_folder) : Core::Plan{};
    const bool installedBefore = !m_state.components.empty();
    const bool upToDate = installedBefore && m_plan.UpToDate();
    const UINT label = !installedBefore ? IDS_BTN_INSTALL : upToDate ? IDS_BTN_REINSTALL : IDS_BTN_UPDATE;
    SetWindowTextW(GetDlgItem(m_hwnd, CmdInstall), Text::Get(label).c_str());
    m_looks[CmdInstall].icon = label == IDS_BTN_REINSTALL ? Icon::Repair : label == IDS_BTN_UPDATE ? Icon::Sync : Icon::Download;
    m_looks[CmdInstall].kind = upToDate ? ButtonKind::Standard : ButtonKind::Accent;
    m_looks[CmdLaunch].kind = upToDate ? ButtonKind::Accent : ButtonKind::Standard;
    InvalidateRect(GetDlgItem(m_hwnd, CmdInstall), nullptr, FALSE);
    InvalidateRect(GetDlgItem(m_hwnd, CmdLaunch), nullptr, FALSE);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::StatusFromPlan() {
    if (m_system && !m_system->Supported()) {
        SetStatus(Level::Error, Text::Format(IDS_STATUS_UNSUPPORTED, Text::Get(IDS_SYS_UNSUPPORTED)));
    } else if (!m_manifest) {
        SetStatus(Level::Error, Text::Get(IDS_STATUS_NO_RELEASE));  // the reason is shown in the card
    } else if (m_plan.UpToDate()) {
        SetStatus(Level::Success, Text::Format(IDS_STATUS_UPTODATE, m_manifest->version));
    } else if (m_state.components.empty()) {
        SetStatus(Level::Info, Text::Format(IDS_STATUS_READY, m_manifest->version, FormatBytes(m_plan.downloadBytes)));
    } else {
        SetStatus(Level::Info, Text::Format(IDS_STATUS_UPDATE, m_manifest->version, FormatBytes(m_plan.downloadBytes)));
    }
}

bool MainWindow::Can(UINT command) const {
    const bool idle = !m_busy && !m_refreshing;
    switch (command) {
    case CmdInstall: return idle && m_manifest && m_system && m_system->Supported();
    case CmdCancel: return m_busy && !m_removing;
    case CmdUninstall: return idle && m_system && Core::Removable(m_folder, *m_system);
    case CmdBtStart: return !m_removing && m_btAdapter && !m_btRunning && Signature::OfficialFeatures();
    case CmdBtStop: return m_btRunning;
    case CmdBtToggle: return !m_removing && m_btAdapter != 0 && Signature::OfficialFeatures();
    case CmdBtAutostart: return !m_removing && m_btAdapter && Signature::OfficialFeatures();
    case CmdBtLog: { std::error_code ec; return std::filesystem::exists(Bt::LogFolder() / L"bluetooth.log", ec); }
    case CmdElevation: return !m_removing && Signature::OfficialFeatures();
    case CmdTray: return !m_removing && Signature::OfficialFeatures();
    case CmdHelloRememberOff: case CmdHelloRemember5: case CmdHelloRemember30: case CmdHelloRememberLock:
        return !m_removing && Signature::OfficialFeatures();
    case CmdElevationSu: return !m_removing && Signature::OfficialFeatures() && Host::ElevationEnabled();
    case CmdRegister: return !m_busy && ShowsGooglePlay() && Signature::OfficialFeatures();  // the host does it
    case CmdCheck: return idle;
    case CmdLaunch: return !m_busy && m_system && m_system->package;
    case CmdAndroidSettings: return !m_busy && m_system && m_system->package;
    case CmdStopWsa: return !m_busy && m_system && m_system->package;
    case CmdOpenFolder: { std::error_code ec; return std::filesystem::exists(m_folder, ec); }
    case CmdChangeFolder:
    case CmdSourceMenu:
    case CmdSourceLocal: case CmdSourceMirror: case CmdSourceGitHub: case CmdSourceCustom: return idle;
    case CmdCopyLog: case CmdClearLog: return m_log.Count() > 0;
    default: return true;
    }
}

void MainWindow::UpdateControls() {
    for (UINT id : {CmdInstall, CmdCancel, CmdLaunch, CmdChangeFolder, CmdSourceMenu, CmdBtToggle, CmdRegister}) {
        HWND b = GetDlgItem(m_hwnd, static_cast<int>(id));
        const bool enable = Can(id);
        if ((IsWindowEnabled(b) != FALSE) != enable) {
            // Never disable the control that has the focus without moving the focus first.
            if (!enable && GetFocus() == b) SendMessageW(m_hwnd, WM_NEXTDLGCTL, 0, FALSE);
            EnableWindow(b, enable);
        }
    }
    if (auto it = m_looks.find(CmdBtToggle); it != m_looks.end()) {
        const std::optional<D2D1_COLOR_F> glow = m_btRunning ? std::optional<D2D1_COLOR_F>(Color::Bluetooth) : std::nullopt;
        if (it->second.glowColor.has_value() != glow.has_value()) {
            it->second.glowColor = glow;
            InvalidateRect(GetDlgItem(m_hwnd, CmdBtToggle), nullptr, FALSE);
        }
    }
}

void MainWindow::SetStatus(Level level, std::wstring text) {
    m_statusLevel = level;
    m_status = std::move(text);
    const RECT r{static_cast<LONG>(m_statusRect.left), static_cast<LONG>(m_statusRect.top), static_cast<LONG>(m_statusRect.right),
                 static_cast<LONG>(m_statusRect.bottom)};
    InvalidateRect(m_hwnd, &r, FALSE);
}

void MainWindow::Log(Level level, std::wstring text) {
    m_log.Append(level, std::move(text));
    if (!m_logVisible) {
        ++m_unread;
        m_looks[CmdExpand].badge = m_unread > 99 ? std::wstring(L"99+") : std::to_wstring(m_unread);
        InvalidateRect(GetDlgItem(m_hwnd, CmdExpand), nullptr, FALSE);
    }
}

void MainWindow::SetTaskbar(TBPFLAG state, int permille) {
    if (!m_taskbar) return;
    m_taskbar->SetProgressState(m_hwnd, state);
    if (state == TBPF_NORMAL || state == TBPF_ERROR) m_taskbar->SetProgressValue(m_hwnd, static_cast<ULONGLONG>(std::max(permille, 0)), 1000);
}

void MainWindow::StartAnimation() { SetTimer(m_hwnd, kAnimationTimer, 15, nullptr); }

void MainWindow::UpdateBluetoothRunning() {
    const bool running = Host::Running() && Host::BluetoothEnabled();
    const bool connected = running && Bt::AndroidConnected();
    const Core::Release::State release = Core::Release::HostVerdict();
    // Google Play registration (host/Registration.cpp): registered from this PC, and - after the
    // button started Android - whether the host has the ID now and opened Google's page.
    const bool registered = !Settings::Read(kGooglePlayRegisteredSetting).empty();
    bool waiting = m_googleWaiting;
    if (waiting && Settings::ReadDword(kGooglePlayIdKnownSetting, 0) != 0) {
        waiting = false;
        SetStatus(Level::Info, Text::Get(IDS_STATUS_REGISTER));
    }
    if (running == m_btRunning && connected == m_btConnected && release == m_release && registered == m_googleRegistered &&
        waiting == m_googleWaiting)
        return;
    m_btRunning = running;
    m_btConnected = connected;
    m_release = release;
    m_googleRegistered = registered;
    m_googleWaiting = waiting;
    UpdateControls();
    InvalidateRect(GetDlgItem(m_hwnd, CmdBtToggle), nullptr, FALSE);
    const RECT r{static_cast<LONG>(m_systemCard.left), static_cast<LONG>(m_systemCard.top), static_cast<LONG>(m_systemCard.right),
                 static_cast<LONG>(m_systemCard.bottom)};
    InvalidateRect(m_hwnd, &r, FALSE);
}

}  // namespace WN::Ui
