// WSA-Next © 2026 Marek Wesołowski - wesmar
// Main window: commands - installation, sources, the install folder, language and the help dialogs.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "app/Cli.h"
#include "gui/Dialogs.h"
#include "bluetooth/Log.h"
#include "host/Host.h"
#include "core/Uninstall.h"
#include "install/Package.h"
#include "install/Setup.h"
#include "install/Traces.h"
#include "support/Browser.h"
#include "support/Config.h"
#include "support/HostEvents.h"
#include "support/Settings.h"
#include "support/Text.h"
#include <ShObjIdl.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <format>

#pragma comment(lib, "shell32.lib")

namespace WN::Ui {
using namespace MainWindowDetail;

void MainWindow::OnCommand(UINT id, UINT /*code*/, HWND /*control*/) {
    if (id == IDCANCEL) id = CmdCancel;  // Esc
    if (id == IDOK) return;               // Enter: no default action
    if (id >= CmdLanguageAuto && id <= CmdLanguageLast) {
        SetLanguage(id);
        return;
    }
    if (!Can(id)) return;
    switch (id) {
    case CmdInstall: StartInstall(); break;
    case CmdUninstall: Uninstall(); break;
    case CmdStopWsa: StopWsa(); break;
    case CmdBtStart: BluetoothStart(); break;
    case CmdBtStop: BluetoothStop(); break;
    case CmdBtToggle:
        if (m_btRunning) BluetoothStop();
        else BluetoothStart();
        break;
    case CmdBtAutostart: BluetoothAutostart(); break;
    case CmdBtLog: BluetoothLog(); break;
    case CmdElevation: ToggleElevation(); break;
    case CmdElevationSu: ToggleElevationSu(); break;
    case CmdHelloRememberOff: SetHelloRemember(0, IDS_MI_HELLO_REMEMBER_OFF); break;
    case CmdHelloRemember5: SetHelloRemember(5, IDS_MI_HELLO_REMEMBER_5); break;
    case CmdHelloRemember30: SetHelloRemember(30, IDS_MI_HELLO_REMEMBER_30); break;
    case CmdHelloRememberLock: SetHelloRemember(Host::kHelloUntilLocked, IDS_MI_HELLO_REMEMBER_LOCK); break;
    case CmdTray:
        if (auto r = Host::SetTrayEnabled(!Host::TrayEnabled()); !r) Log(Level::Error, r.error());
        InvalidateRect(m_hwnd, nullptr, FALSE);
        break;
    case CmdCancel:
        m_worker.request_stop();
        SetStatus(Level::Warning, Text::Get(IDS_STATUS_CANCELING));
        break;
    case CmdCheck: Refresh(); break;
    case CmdLaunch: Launch(); break;
    case CmdRegister: RegisterWithGoogle(); break;
    case CmdAndroidSettings:
        if (auto r = Install::LaunchAndroidSettings(); !r) {
            Log(Level::Error, r.error());
            SetStatus(Level::Error, r.error());
        }
        break;
    case CmdOpenFolder: ShellExecuteW(m_hwnd, L"open", m_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case CmdChangeFolder: ChangeFolder(); break;
    case CmdExit: PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
    case CmdSourceMenu: {
        RECT r{};
        GetWindowRect(GetDlgItem(m_hwnd, CmdSourceMenu), &r);
        m_menu.TrackContext(m_hwnd, m_sourceMenu, {r.left, r.bottom + m_fonts.S(2)});
        break;
    }
    case CmdSourceGitHub: SetSource(L"github"); break;
    case CmdSourceMirror: SetSource(L"mirror"); break;
    case CmdSourceLocal: SetSource(L"local"); break;
    case CmdSourceCustom: EditCustomSource(); break;
    case CmdReleases: OpenInBrowser(std::format(L"https://github.com/{}/releases", Config::kGitHubRepo).c_str()); break;
    case CmdSupport: OpenInBrowser(Config::kSupportUrl); break;
    case CmdViewLog:
    case CmdExpand: SetLogVisible(!m_logVisible, true); break;
    case CmdCopyLog: CopyToClipboard(m_hwnd, m_log.Text()); break;
    case CmdClearLog: m_log.Clear(); UpdateControls(); break;
    case CmdHelpCli: ShowCommandLineHelp(); break;
    case CmdAbout: ShowAbout(); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------
void MainWindow::BeginWork() {
    if (m_worker.joinable()) m_worker.join();
    m_busy = true;
    m_failed = m_finished = false;
    m_progress = {};
    m_shown = 0;
    m_animStart = GetTickCount64();
    UpdateControls();
    SetTaskbar(TBPF_INDETERMINATE);
    StartAnimation();
}

void MainWindow::StartInstall() {
    if (!m_manifest) return;
    m_removing = false;
    BeginWork();

    HWND hwnd = m_hwnd;
    Core::Options options;
    options.folder = m_folder;
    m_worker = std::jthread([hwnd, manifest = *m_manifest, options](std::stop_token stop) {
        // The package manager's asynchronous operations block: a multithreaded apartment.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Core::Callbacks cb;
        cb.log = [hwnd](Level level, std::wstring text) {
            PostOwned(hwnd, WM_APP_LOG, std::make_unique<LogMessage>(LogMessage{level, std::move(text)}));
        };
        cb.progress = [hwnd](const Core::Progress& p) { PostOwned(hwnd, WM_APP_PROGRESS, std::make_unique<Core::Progress>(p)); };
        cb.confirm = [hwnd](const std::wstring& question) {
            // Synchronous: the worker waits for the answer. Fails (no) when the window is gone.
            return SendMessageW(hwnd, WM_APP_CONFIRM, 0, reinterpret_cast<LPARAM>(&question)) == 1;
        };
        auto result = Core::Run(manifest, options, cb, stop);
        auto done = std::make_unique<DoneMessage>();
        done->ok = result.has_value();
        done->canceled = stop.stop_requested();
        done->restart = result && result->restartNeeded;
        if (!result) done->error = result.error();
        if (SUCCEEDED(com)) CoUninitialize();
        PostOwned(hwnd, WM_APP_DONE, std::move(done));
    });
}

void MainWindow::OnInstallDone(bool ok, bool canceled, bool restart, const std::wstring& error) {
    m_busy = false;
    m_failed = !ok && !canceled;
    m_finished = ok;
    m_progress.permille = ok ? 1000 : m_progress.permille;
    StartAnimation();
    SetTaskbar(ok ? TBPF_NOPROGRESS : canceled ? TBPF_NOPROGRESS : TBPF_ERROR, 1000);
    m_state = Core::ReadState(m_folder);
    // The outcome stays in the status line while the window reads the new state.
    const Level level = ok ? Level::Success : canceled ? Level::Warning : Level::Error;
    const std::wstring text = ok         ? Text::Format(restart ? IDS_STATUS_RESTART : IDS_STATUS_DONE, m_manifest ? m_manifest->version : L"")
                              : canceled ? Text::Get(IDS_STATUS_CANCELED)
                                         : Text::Format(IDS_STATUS_FAILED, error);
    Log(level, text);
    SetStatus(level, text);
    if (ok) {
        FLASHWINFO flash{sizeof flash, m_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&flash);
        if (restart) {
            const bool now = Confirm(m_hwnd, Text::Get(IDS_RESTART_HEADING), text, Text::Get(IDS_RESTART_OK));
            if (now) ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/r /t 0", nullptr, SW_HIDE);
        }
    }
    Refresh(true);  // the package and the components changed
    SetStatus(level, text);
}

// ---------------------------------------------------------------------------
// Uninstallation: keep the Android data, or remove everything (asked twice)
// ---------------------------------------------------------------------------
void MainWindow::Uninstall() {
    const std::filesystem::path data = Install::PackageDataFolder();
    const std::wstring dataSize = FormatBytes(Install::FolderSize(data));
    DialogSpec spec;
    spec.heading = Text::Get(IDS_UNINSTALL_HEADING);
    spec.message = Text::Format(IDS_UNINSTALL_MESSAGE, m_folder.wstring(), dataSize);
    spec.ok = Text::Get(IDS_UNINSTALL_KEEP);
    spec.extra = Text::Get(IDS_UNINSTALL_ALL);
    spec.width = 560;
    const int choice = RunDialog(m_hwnd, spec).button;
    const bool everything = choice == 2;
    const bool go = choice == 1 || (everything && Confirm(m_hwnd, Text::Get(IDS_UNINSTALL_ALL_HEADING),
                                                          Text::Format(IDS_UNINSTALL_ALL_MESSAGE, dataSize),
                                                          Text::Get(IDS_UNINSTALL_ALL_OK), true));
    if (!go) return;

    // Installed with WSA-Next Setup: Windows Installer removes WSA-Next (with its own progress bar)
    // and this window closes, so that WSA-Next.exe is not in use.
    if (const auto product = Install::SetupProduct()) {
        if (auto r = Install::RemoveThroughSetup(*product, everything); !r) {
            Log(Level::Error, r.error());
            SetStatus(Level::Error, r.error());
            return;
        }
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        return;
    }

    m_removing = true;
    BeginWork();
    HWND hwnd = m_hwnd;
    m_worker = std::jthread([hwnd, folder = m_folder, everything](std::stop_token) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Core::Callbacks cb;
        cb.log = [hwnd](Level level, std::wstring text) {
            PostOwned(hwnd, WM_APP_LOG, std::make_unique<LogMessage>(LogMessage{level, std::move(text)}));
        };
        cb.progress = [hwnd](const Core::Progress& p) { PostOwned(hwnd, WM_APP_PROGRESS, std::make_unique<Core::Progress>(p)); };
        cb.confirm = [](const std::wstring&) { return true; };  // asked before the start
        auto result = Core::Uninstall(folder, everything ? Core::Removal::Everything : Core::Removal::KeepData, cb);
        auto done = std::make_unique<DoneMessage>();
        done->removal = true;
        done->everything = everything;
        done->ok = result.has_value();
        if (result) {
            done->restart = result->restartNeeded;
            done->leftovers = result->leftovers.size();
        } else {
            done->error = result.error();
        }
        if (SUCCEEDED(com)) CoUninitialize();
        PostOwned(hwnd, WM_APP_DONE, std::move(done));
    });
}

void MainWindow::StopWsa() {
    BeginWork();
    SetStatus(Level::Info, Text::Get(IDS_STATUS_WSA_STOPPING));
    HWND hwnd = m_hwnd;
    m_worker = std::jthread([hwnd](std::stop_token) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto done = std::make_unique<DoneMessage>();
        done->stop = Install::Stop();
        done->ok = *done->stop != Guest::VmStop::Failed;
        if (SUCCEEDED(com)) CoUninitialize();
        PostOwned(hwnd, WM_APP_DONE, std::move(done));
    });
}

void MainWindow::OnStopDone(Guest::VmStop result) {
    m_busy = false;
    m_failed = result == Guest::VmStop::Failed;
    m_finished = !m_failed;
    m_progress.permille = 1000;
    StartAnimation();
    SetTaskbar(m_failed ? TBPF_ERROR : TBPF_NOPROGRESS, 1000);
    const UINT id = result == Guest::VmStop::ShutDown     ? IDS_STATUS_WSA_STOPPED
                    : result == Guest::VmStop::Terminated ? IDS_STATUS_WSA_TERMINATED
                    : result == Guest::VmStop::NotRunning ? IDS_STATUS_WSA_NOT_RUNNING
                                                          : IDS_STATUS_WSA_STOP_FAILED;
    const Level level = result == Guest::VmStop::Failed ? Level::Error : result == Guest::VmStop::Terminated ? Level::Warning : Level::Success;
    Log(level, Text::Get(id));
    SetStatus(level, Text::Get(id));
    UpdateControls();
}

void MainWindow::OnUninstallDone(bool ok, bool everything, bool restart, std::size_t leftovers, const std::wstring& error) {
    m_busy = m_removing = false;
    const bool clean = ok && leftovers == 0;
    m_failed = !clean;
    m_finished = clean;
    m_progress.permille = 1000;
    StartAnimation();
    SetTaskbar(clean ? TBPF_NOPROGRESS : TBPF_ERROR, 1000);
    if (ok && everything) m_forgetSettings = true;
    m_state = Core::ReadState(m_folder);
    const std::wstring text = !ok        ? Text::Format(IDS_STATUS_REMOVE_FAILED, error)
                              : everything ? Text::Get(IDS_STATUS_REMOVED_ALL)
                                           : Text::Get(IDS_STATUS_REMOVED);
    const Level level = !ok ? Level::Error : leftovers ? Level::Warning : Level::Success;
    Log(level, text);
    Refresh(true);  // the package and the folder are gone; the release can be installed again
    SetStatus(level, text);
    if (ok && restart) {
        const bool now = Confirm(m_hwnd, Text::Get(IDS_RESTART_HEADING), Text::Get(IDS_LOG_VMP_OFF), Text::Get(IDS_RESTART_OK));
        if (now) ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/r /t 0", nullptr, SW_HIDE);
    }
}

// ---------------------------------------------------------------------------
// Bluetooth for Android: a bridge of the background host (src/host), which keeps working after
// this window is closed; "Start with Windows" adds the host's logon task.
// ---------------------------------------------------------------------------
void MainWindow::BluetoothStart() {
    if (auto r = Host::SetBluetoothEnabled(true); !r) {
        Log(Level::Error, r.error());
        SetStatus(Level::Error, r.error());
        return;
    }
    Log(Level::Success, Text::Get(IDS_LOG_BT_STARTED));
    SetStatus(Level::Success, Text::Get(IDS_LOG_BT_STARTED));
    UpdateBluetoothRunning();
}

void MainWindow::BluetoothStop() {
    if (auto r = Host::SetBluetoothEnabled(false); !r) Log(Level::Warning, r.error());
    Log(Level::Info, Text::Get(IDS_LOG_BT_STOPPED));
    SetStatus(Level::Info, Text::Get(IDS_LOG_BT_STOPPED));
    UpdateBluetoothRunning();
}

void MainWindow::BluetoothAutostart() {
    const bool enable = !m_btAutostart;
    if (auto r = Host::SetAutostart(enable); !r) {
        Log(Level::Error, r.error());
        SetStatus(Level::Error, r.error());
        return;
    }
    m_btAutostart = enable;
    Log(Level::Success, enable ? Text::Format(IDS_LOG_BT_AUTOSTART_ON, Host::kAutostartTask) : Text::Get(IDS_LOG_BT_AUTOSTART_OFF));
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// Root for Android apps: turning it on asks first (what it means is said once, plainly).
void MainWindow::ToggleElevation() {
    const bool enable = !Host::ElevationEnabled();
    if (enable && !Confirm(m_hwnd, Text::Get(IDS_ELEVATION_HEADING), Text::Get(IDS_ELEVATION_TEXT), Text::Get(IDS_ELEVATION_OK), true)) return;
    if (auto r = Host::SetElevationEnabled(enable); !r) {
        Log(Level::Error, r.error());
        SetStatus(Level::Error, r.error());
        return;
    }
    Log(Level::Success, Text::Get(enable ? IDS_LOG_ELEVATION_ON : IDS_LOG_ELEVATION_OFF));
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// "su for root apps": root apps look for su; while on, Android offers it (a link to 0).
void MainWindow::ToggleElevationSu() {
    const bool enable = !Host::ElevationSuEnabled();
    if (auto r = Host::SetElevationSuEnabled(enable); !r) {
        Log(Level::Error, r.error());
        SetStatus(Level::Error, r.error());
        return;
    }
    Log(Level::Success, Text::Get(enable ? IDS_LOG_ELEVATION_SU_ON : IDS_LOG_ELEVATION_SU_OFF));
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// Remember Windows Hello for Android apps: off by default; turning it on asks first, because
// whoever uses the unlocked PC meanwhile gets into those apps too. Read by the host per request.
void MainWindow::SetHelloRemember(std::uint32_t minutes, UINT choice) {
    if (minutes != 0 && Host::HelloRememberMinutes() == 0 &&
        !Confirm(m_hwnd, Text::Get(IDS_HELLO_REMEMBER_HEADING), Text::Get(IDS_HELLO_REMEMBER_TEXT), Text::Get(IDS_HELLO_REMEMBER_OK), true))
        return;
    Host::SetHelloRememberMinutes(minutes);
    Log(Level::Success, Text::Get(IDS_MI_HELLO_REMEMBER) + L": " + Text::Get(choice));
}

void MainWindow::BluetoothLog() {
    const std::filesystem::path log = Bt::LogFolder() / L"bluetooth.log";
    ShellExecuteW(m_hwnd, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::Launch() {
    Log(Level::Info, Text::Get(IDS_LOG_LAUNCH));
    if (auto r = Install::LaunchAndroid(); !r) {
        Log(Level::Error, r.error());
        SetStatus(Level::Error, r.error());
    }
}

// Google Play registration: the background host knows the device ID (Android hands it over at every
// start) and does the work - the ID to the clipboard, Google's page in the browser. Without the ID yet,
// Android is started and the host finishes the job when the ID arrives.
void MainWindow::RegisterWithGoogle() {
    if (!RequestHostRegistration()) {
        Log(Level::Error, Text::Get(IDS_STATUS_REGISTER_NOHOST));
        SetStatus(Level::Error, Text::Get(IDS_STATUS_REGISTER_NOHOST));
        return;
    }
    if (Settings::ReadDword(kGooglePlayIdKnownSetting, 0) != 0) {
        Log(Level::Info, Text::Get(IDS_STATUS_REGISTER));
        SetStatus(Level::Info, Text::Get(IDS_STATUS_REGISTER));
        return;
    }
    m_googleWaiting = true;
    Log(Level::Info, Text::Get(IDS_STATUS_REGISTER_WAIT));
    SetStatus(Level::Info, Text::Get(IDS_STATUS_REGISTER_WAIT));
    const RECT r{static_cast<LONG>(m_systemCard.left), static_cast<LONG>(m_systemCard.top), static_cast<LONG>(m_systemCard.right),
                 static_cast<LONG>(m_systemCard.bottom)};
    InvalidateRect(m_hwnd, &r, FALSE);
    Launch();
}

// ---------------------------------------------------------------------------
// Source and folder
// ---------------------------------------------------------------------------
void MainWindow::SetSource(const std::wstring& key) {
    Settings::Write(L"Source", key);
    SetWindowTextW(GetDlgItem(m_hwnd, CmdSourceMenu), SourceSummary().c_str());
    InvalidateRect(GetDlgItem(m_hwnd, CmdSourceMenu), nullptr, FALSE);
    Log(Level::Info, Text::Format(IDS_LOG_SOURCE, SourceSummary()));
    Refresh();
}

void MainWindow::EditCustomSource() {
    DialogSpec spec;
    spec.heading = Text::Get(IDS_CUSTOM_HEADING);
    spec.message = Text::Get(IDS_CUSTOM_MESSAGE);
    spec.fields = {{Text::Get(IDS_CUSTOM_FIELD), Settings::Read(L"CustomManifest"), L"https://example.org/wsa-next/manifest.json"}};
    spec.width = 560;
    spec.validate = [](const std::vector<std::wstring>& v) -> std::optional<std::wstring> {
        const std::wstring& url = v[0];
        if (url.starts_with(L"https://") || url.starts_with(L"http://localhost/") || url.starts_with(L"http://127.0.0.1/"))
            return std::nullopt;
        return Text::Get(IDS_CUSTOM_INVALID);
    };
    const auto result = RunDialog(m_hwnd, spec);
    if (result.button != 1) return;
    Settings::Write(L"CustomManifest", result.values[0]);
    SetSource(L"custom");
}

void MainWindow::ChangeFolder() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(Text::Get(IDS_FOLDER_PICK).c_str());
    ComPtr<IShellItem> start;
    std::error_code ec;
    const std::filesystem::path existing = std::filesystem::exists(m_folder, ec) ? m_folder : m_folder.parent_path();
    if (SUCCEEDED(SHCreateItemFromParsingName(existing.c_str(), nullptr, IID_PPV_ARGS(start.GetAddressOf())))) dialog->SetFolder(start.Get());
    const HRESULT shown = dialog->Show(m_hwnd);
    if (FAILED(shown)) return;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(item.GetAddressOf())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    std::filesystem::path chosen = path;
    CoTaskMemFree(path);
    // A new, empty folder for WSA-Next inside the chosen one, unless it already is one.
    if (!std::filesystem::exists(chosen / Config::kStateFile, ec) && !std::filesystem::is_empty(chosen, ec))
        chosen /= Config::kInstallFolder;
    m_folder = chosen;
    Settings::Write(L"InstallFolder", m_folder.wstring());
    m_state = Core::ReadState(m_folder);
    UpdatePlan();
    StatusFromPlan();
    UpdateControls();
}

// ---------------------------------------------------------------------------
// Language
// ---------------------------------------------------------------------------
void MainWindow::SetLanguage(UINT command) {
    const std::wstring code = command == CmdLanguageAuto                         ? std::wstring(L"auto")
                              : command - CmdLanguageFirst < m_languages.size() ? m_languages[command - CmdLanguageFirst].code
                                                                                 : std::wstring();
    if (code.empty() || !Text::Use(code, true)) return;
    ApplyLanguage();
    Log(Level::Info, Text::Format(IDS_LANGUAGE_CHANGED, LanguageName(Text::Current())));
}

std::wstring MainWindow::LanguageName(const std::wstring& code) const {
    for (const auto& l : m_languages)
        if (l.code == code) return l.name;
    return code;
}

// Everything that shows text is rebuilt; layout follows (labels change width, buttons stay equal).
void MainWindow::ApplyLanguage() {
    m_menu.Reset();
    BuildMenus();
    for (const ButtonSpec& b : kButtons)
        if (b.text) SetWindowTextW(GetDlgItem(m_hwnd, static_cast<int>(b.id)), Text::Get(b.text).c_str());
    SetWindowTextW(GetDlgItem(m_hwnd, CmdSourceMenu), SourceSummary().c_str());
    UpdatePlan();
    if (!m_busy) StatusFromPlan();
    Layout();
    RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------
void MainWindow::ShowCommandLineHelp() {
    DialogSpec spec;
    spec.heading = Text::Get(IDS_CLI_HEADING);
    spec.message = Text::Format(IDS_HELP_MESSAGE, Config::kExeName);
    spec.monospace = Cli::HelpText();  // the command line is English, so is its help
    spec.ok = Text::Get(IDS_CLOSE);
    spec.cancelButton = false;
    spec.extra = Text::Get(IDS_COPY);
    spec.width = 720;
    const auto result = RunDialog(m_hwnd, spec);
    if (result.button == 2) CopyToClipboard(m_hwnd, spec.monospace);
}

void MainWindow::ShowAbout() {
    Inform(m_hwnd, Text::Get(IDS_ABOUT_HEADING),
           Text::Format(IDS_ABOUT_TEXT, Config::kAppTitle, Config::kAppVersion, Config::kCopyright, Config::kGitHubRepo));
}

}  // namespace WN::Ui
