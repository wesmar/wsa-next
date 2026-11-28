// WSA-Next © 2026 Marek Wesołowski - wesmar
// Main window: compact, Per-Monitor-V2 aware, Mica backdrop, every action also in the menus.
//
//   Release card     what the source offers, the install folder, progress and the main buttons
//   This PC          Windows version, Virtual Machine Platform, developer mode, the WSA package
//   Components       every part of the release with its size and whether it is up to date
//   Log              expandable, timestamped
#pragma once
#include "core/Install.h"
#include "core/Manifest.h"
#include "core/Release.h"
#include "core/System.h"
#include "gui/Menus.h"
#include "gui/Theme.h"
#include "gui/Widgets.h"
#include "support/Handle.h"
#include "support/Text.h"
#include <ShObjIdl.h>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace WN::Ui {

class MainWindow {
public:
    explicit MainWindow(HINSTANCE instance);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(int showCommand);
    int Run();  // message loop

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    // Construction / layout / painting (MainWindow.cpp, MainWindowLayout.cpp)
    void OnCreate();
    void Layout();
    void Paint();
    void PaintReleaseCard(Canvas& c);
    void PaintSystemCard(Canvas& c);
    void PaintComponentsCard(Canvas& c);
    [[nodiscard]] float ContentHeight(bool withLog) const;  // DIP
    [[nodiscard]] float ButtonWidth() const;
    void SetLogVisible(bool visible, bool resizeWindow);
    void RestorePlacement(int showCommand);
    void SavePlacement();

    // Menus (MainWindowMenus.cpp)
    void BuildMenus();
    void BuildAccelerators();
    void ApplyMenuState(HMENU menu) const;
    void ShowContextMenu(HWND target, POINT screen);

    // State (MainWindowState.cpp)
    void Refresh(bool keepResult = false);  // reads Windows and the manifest on a worker thread
    void OnRefreshed(Core::SystemInfo system, std::optional<Core::Manifest> manifest, std::wstring error);
    void UpdatePlan();
    void UpdateControls();
    [[nodiscard]] bool Can(UINT command) const;
    void SetStatus(Level level, std::wstring text);
    void StatusFromPlan();
    void Log(Level level, std::wstring text);
    void SetTaskbar(TBPFLAG state, int permille = 0);
    void StartAnimation();
    [[nodiscard]] std::wstring SourceKey() const;
    [[nodiscard]] std::wstring SourceUrl() const;
    [[nodiscard]] std::wstring SourceSummary() const;
    [[nodiscard]] std::wstring ComponentName(const Core::Component& c) const;

    // Commands (MainWindowCommands.cpp)
    void OnCommand(UINT id, UINT code, HWND control);
    void StartInstall();
    void OnInstallDone(bool ok, bool canceled, bool restart, const std::wstring& error);
    void Uninstall();
    void OnUninstallDone(bool ok, bool everything, bool restart, std::size_t leftovers, const std::wstring& error);
    void StopWsa();
    void OnStopDone(Guest::VmStop result);
    void BeginWork();  // busy state, progress animation, taskbar
    void BluetoothStart();
    void BluetoothStop();
    void BluetoothAutostart();
    void ToggleElevation();
    void ToggleElevationSu();
    void SetHelloRemember(std::uint32_t minutes, UINT choice);
    void BluetoothLog();
    void UpdateBluetoothRunning();  // on the host's change notification (and on activation)
    void RegisterWithGoogle();      // Google Play registration through the background host
    void UpdateApp();               // WSA-Next Setup of the newer version (core/Update.h)
    void OnSetupDone(bool ok, bool canceled, const std::wstring& error);
    [[nodiscard]] bool ShowsGooglePlay() const;  // the "This PC" card has the Google Play row
    void SetSource(const std::wstring& key);
    void EditCustomSource();
    void ChangeFolder();
    void Launch();
    void ShowCommandLineHelp();
    void ShowAbout();
    void SetLanguage(UINT command);
    void ApplyLanguage();
    [[nodiscard]] std::wstring LanguageName(const std::wstring& code) const;

    HINSTANCE m_instance;
    HWND m_hwnd = nullptr;
    Fonts m_fonts;
    MenuBar m_menu{m_fonts};
    HMENU m_sourceMenu = nullptr, m_logMenu = nullptr;
    std::vector<Text::Language> m_languages;
    AccelHandle m_accel;
    IconHandle m_icon, m_smallIcon;
    LogView m_log{m_fonts};
    std::unordered_map<UINT, ButtonLook> m_looks;
    ComPtr<ITaskbarList3> m_taskbar;
    UINT m_taskbarCreated = 0;

    // Layout (client pixels)
    D2D1_RECT_F m_releaseCard{}, m_systemCard{}, m_componentsCard{}, m_logCard{};
    D2D1_RECT_F m_titleRect{}, m_metaRect{}, m_rowSource{}, m_rowFolder{}, m_rowDownload{}, m_progressRect{}, m_statusRect{};
    float m_registerWidth = 0;  // the Google Play row's button, so its value text stops before it
    float m_labelWidth = 0;
    bool m_trackingMouse = false;

    // What is known
    std::optional<Core::SystemInfo> m_system;
    std::optional<Core::Manifest> m_manifest;
    std::wstring m_manifestError;
    std::filesystem::path m_folder;
    Core::InstalledState m_state;
    Core::Plan m_plan;
    bool m_refreshing = false;

    // View state
    bool m_logVisible = false, m_closing = false;
    std::size_t m_unread = 0;
    Level m_statusLevel = Level::Info;
    std::wstring m_status;

    // Installation
    bool m_busy = false;
    Core::Progress m_progress;
    float m_shown = 0;  // animated bar position 0..1
    bool m_failed = false, m_finished = false;
    // Bluetooth for Android (src/bluetooth): adapter, background bridge, start with Windows
    std::uint64_t m_btAdapter = 0;
    bool m_btKnown = false, m_btRunning = false, m_btConnected = false, m_btAutostart = false;
    Core::Release::State m_release = Core::Release::State::Unknown;  // the host's check of the installed release
    bool m_googleRegistered = false;  // Google Play: an ID was registered from this PC (host/Registration.cpp)
    bool m_googleWaiting = false;     // asked to register while Android had not handed over the ID yet
    std::wstring m_appUpdate;         // a newer WSA-Next the source offers, "" = none
    KernelHandle m_hostChanged;  // kHostChangedEvent
    HANDLE m_hostWait = nullptr;  // its thread-pool wait

    bool m_removing = false;        // the running job is an uninstallation
    bool m_forgetSettings = false;  // "Remove everything" deleted the settings: do not write them again
    ULONGLONG m_animStart = 0;
    std::jthread m_worker;
};

}  // namespace WN::Ui
