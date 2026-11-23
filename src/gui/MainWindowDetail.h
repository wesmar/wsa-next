// WSA-Next © 2026 Marek Wesołowski - wesmar
// Internal to the MainWindow*.cpp files: command identifiers, worker messages, layout constants.
#pragma once
#include "core/Install.h"
#include "gui/Widgets.h"
#include "host/Guest.h"
#include "support/Text.h"
#include <Windows.h>
#include <format>
#include <memory>
#include <string>

namespace WN::Ui::MainWindowDetail {

inline constexpr wchar_t kClassName[] = L"WSANext.MainWindow";
inline constexpr UINT_PTR kAnimationTimer = 1;

enum : UINT {
    WM_APP_LOG = WM_APP + 1,  // lParam: LogMessage*
    WM_APP_PROGRESS,          // lParam: Core::Progress*
    WM_APP_DONE,              // lParam: DoneMessage*
    WM_APP_CONFIRM,           // lParam: const std::wstring* (sent, result 1 = yes)
    WM_APP_REFRESHED,         // lParam: RefreshMessage*
    WM_APP_HOST_CHANGED,      // the background host signaled a change (support/HostEvents.h)
};

enum Command : UINT {
    CmdInstall = 1000, CmdCancel, CmdLaunch, CmdCheck, CmdOpenFolder, CmdChangeFolder, CmdExit,
    CmdSourceMenu, CmdSourceLocal, CmdSourceMirror, CmdSourceGitHub, CmdSourceCustom, CmdReleases,
    CmdViewLog, CmdCopyLog, CmdClearLog, CmdExpand,
    CmdHelpCli, CmdAbout, CmdUninstall,
    CmdBtStart, CmdBtStop, CmdBtToggle, CmdBtAutostart, CmdBtLog, CmdStopWsa, CmdElevation, CmdElevationSu, CmdTray,
    CmdHelloRememberOff, CmdHelloRemember5, CmdHelloRemember30, CmdHelloRememberLock,  // Android > Remember Windows Hello
    CmdSupport,  // the "Support" button on the menu bar's line
    CmdLanguageAuto = 1100, CmdLanguageFirst,  // View > language: automatic, then one per available language
    CmdLanguageLast = 1199,
};
inline constexpr int kIdLog = 2001;

struct LogMessage {
    Level level;
    std::wstring text;
};
struct DoneMessage {
    bool ok = false, canceled = false, restart = false;
    std::wstring error;
    bool removal = false, everything = false;  // an uninstallation (Core::Uninstall) instead of Core::Run
    std::size_t leftovers = 0;                 // removal: items that could not be removed
    std::optional<Guest::VmStop> stop;         // stopping WSA (Install::Stop) instead
};
struct RefreshMessage {
    Core::SystemInfo system;
    std::optional<Core::Manifest> manifest;
    std::wstring manifestError;
    std::uint64_t btAdapter = 0;  // address of the PC's Bluetooth adapter, 0 = none
    bool btAutostart = false;     // the bridge starts with Windows
};

// The worker hands objects to the UI thread through the message queue. If posting fails
// (window already gone) the unique_ptr still owns the object and frees it.
template <class T>
void PostOwned(HWND hwnd, UINT message, std::unique_ptr<T> payload) {
    if (PostMessageW(hwnd, message, 0, reinterpret_cast<LPARAM>(payload.get()))) (void)payload.release();
}
template <class T>
std::unique_ptr<T> TakeOwned(LPARAM lParam) {
    return std::unique_ptr<T>(reinterpret_cast<T*>(lParam));
}

inline std::wstring ProgressText(const Core::Progress& p) {
    if (p.permille < 0) return p.phase + L"\x2026";
    std::wstring text = std::format(L"{}%  \x00B7  ", p.permille / 10);
    if (p.steps > 1) text += std::format(L"{}/{}  ", p.step, p.steps);
    text += p.phase;
    if (p.total) text += std::format(L"  \x00B7  {} / {}", FormatBytes(p.done), FormatBytes(p.total));
    if (p.speed) text += std::format(L"  \x00B7  {}/s", FormatBytes(p.speed));
    if (p.eta > 0) text += L"  \x00B7  " + Text::Format(IDS_PROGRESS_LEFT, FormatDuration(static_cast<std::uint64_t>(p.eta)));
    return text;
}

// Every button of the window: command, string, look. The source "combo" gets its text at runtime.
struct ButtonSpec {
    UINT id;
    UINT text;  // string id, 0 = set later
    ButtonLook look;
};
inline const ButtonSpec kButtons[] = {
    {CmdSourceMenu, 0, {ButtonKind::Combo}},
    {CmdChangeFolder, IDS_BTN_CHANGE, {ButtonKind::Standard, Icon::Folder}},
    {CmdBtToggle, IDS_SYS_BLUETOOTH, {ButtonKind::Standard, Icon::Bluetooth, true, false, {}, Color::Bluetooth}},
    {CmdCancel, IDS_CANCEL, {ButtonKind::Standard, Icon::Cancel}},
    {CmdLaunch, IDS_BTN_LAUNCH, {ButtonKind::Standard, Icon::Play}},
    {CmdInstall, IDS_BTN_INSTALL, {ButtonKind::Accent, Icon::Download}},
    {CmdExpand, IDS_BTN_LOG, {ButtonKind::Expander, 0, false}},
    // Last in the Tab order; on the window background, a quiet button: its heart is grey like the
    // other icons and turns sponsor pink only under the mouse (green stays for actions and status).
    {CmdSupport, IDS_BTN_SUPPORT, {ButtonKind::Standard, Icon::HeartFill, false, false, {}, {}, {}, Color::Support}},
};

// Layout, in DIP (scaled with the window DPI).
inline constexpr float kMargin = 12, kPad = 14, kButtonW = 120, kButtonH = 30, kRowH = 26, kGap = 8;
inline constexpr float kBarH = 28, kLogH = 180, kRadius = 8;

}  // namespace WN::Ui::MainWindowDetail
