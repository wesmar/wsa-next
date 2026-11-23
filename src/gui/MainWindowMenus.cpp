// WSA-Next © 2026 Marek Wesołowski - wesmar
// Main window: menu bar, context menus, accelerators and the enabled / checked state of items.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include "host/Host.h"
#include "support/Text.h"
#include <format>

namespace WN::Ui {
using namespace MainWindowDetail;

void MainWindow::BuildMenus() {
    auto& m = m_menu;
    auto item = [&m](HMENU menu, UINT id, UINT text, const wchar_t* shortcut = L"", bool radio = false) {
        m.AddItem(menu, id, Text::Get(text), shortcut, radio);
    };

    HMENU file = m.AddMenu(Text::Get(IDS_MENU_FILE));
    item(file, CmdInstall, IDS_MI_INSTALL, L"Ctrl+I");
    item(file, CmdCancel, IDS_CANCEL, L"Esc");
    item(file, CmdCheck, IDS_MI_CHECK, L"F5");
    m.AddSeparator(file);
    item(file, CmdLaunch, IDS_MI_LAUNCH, L"Ctrl+R");
    item(file, CmdStopWsa, IDS_MI_STOP_WSA, L"Ctrl+Shift+R");
    item(file, CmdOpenFolder, IDS_MI_OPEN_FOLDER);
    item(file, CmdChangeFolder, IDS_MI_CHANGE_FOLDER);
    m.AddSeparator(file);
    item(file, CmdUninstall, IDS_MI_UNINSTALL);
    m.AddSeparator(file);
    item(file, CmdExit, IDS_MI_EXIT, L"Ctrl+Q");

    m_sourceMenu = m.AddMenu(Text::Get(IDS_MENU_SOURCE));
    item(m_sourceMenu, CmdSourceGitHub, IDS_MI_SRC_GITHUB, L"", true);
    item(m_sourceMenu, CmdSourceMirror, IDS_MI_SRC_MIRROR, L"", true);
    item(m_sourceMenu, CmdSourceLocal, IDS_MI_SRC_LOCAL, L"", true);
    item(m_sourceMenu, CmdSourceCustom, IDS_MI_SRC_CUSTOM, L"Ctrl+E", true);
    m.AddSeparator(m_sourceMenu);
    item(m_sourceMenu, CmdReleases, IDS_MI_RELEASES);

    HMENU bluetooth = m.AddMenu(Text::Get(IDS_MENU_BLUETOOTH));
    item(bluetooth, CmdBtStart, IDS_MI_BT_START, L"Ctrl+B");
    item(bluetooth, CmdBtStop, IDS_MI_BT_STOP);
    m.AddSeparator(bluetooth);
    item(bluetooth, CmdBtAutostart, IDS_MI_BT_AUTOSTART);
    m.AddSeparator(bluetooth);
    item(bluetooth, CmdBtLog, IDS_MI_BT_LOG);

    HMENU android = m.AddMenu(Text::Get(IDS_MENU_ANDROID));
    item(android, CmdElevation, IDS_MI_ELEVATION);
    item(android, CmdElevationSu, IDS_MI_ELEVATION_SU);
    m.AddSeparator(android);
    HMENU remember = m.AddSubmenu(android, Text::Get(IDS_MI_HELLO_REMEMBER));
    item(remember, CmdHelloRememberOff, IDS_MI_HELLO_REMEMBER_OFF, L"", true);
    item(remember, CmdHelloRemember5, IDS_MI_HELLO_REMEMBER_5, L"", true);
    item(remember, CmdHelloRemember30, IDS_MI_HELLO_REMEMBER_30, L"", true);
    item(remember, CmdHelloRememberLock, IDS_MI_HELLO_REMEMBER_LOCK, L"", true);
    m.AddSeparator(android);
    item(android, CmdTray, IDS_MI_TRAY);

    HMENU view = m.AddMenu(Text::Get(IDS_MENU_VIEW));
    item(view, CmdViewLog, IDS_MI_LOG, L"Ctrl+L");
    item(view, CmdCopyLog, IDS_MI_COPY_LOG);
    item(view, CmdClearLog, IDS_MI_CLEAR_LOG);
    // Language: follow Windows, or English (built in) / one of the language files, each named in itself
    // and with its coverage, so a half-done translation is visible. Long lists continue in columns.
    m.AddSeparator(view);
    HMENU language = m.AddSubmenu(view, Text::Get(IDS_MI_LANGUAGE));
    m_languages = Text::Available();
    m.AddItem(language, CmdLanguageAuto, Text::Format(IDS_LANGUAGE_AUTO, LanguageName(Text::Detect())), {}, true);
    m.AddSeparator(language);
    constexpr std::size_t kPerColumn = 20;
    for (std::size_t i = 0; i < m_languages.size() && CmdLanguageFirst + i <= CmdLanguageLast; ++i) {
        const Text::Language& l = m_languages[i];
        m.AddItem(language, static_cast<UINT>(CmdLanguageFirst + i), l.name, l.code == L"en" ? std::wstring() : std::format(L"{}%", l.Coverage()),
                  true, i > 0 && i % kPerColumn == 0);
    }

    HMENU help = m.AddMenu(Text::Get(IDS_MENU_HELP));
    item(help, CmdHelpCli, IDS_MI_HELP_CLI, L"F1");
    item(help, CmdAbout, IDS_MI_ABOUT);

    m_logMenu = m.CreatePopup();
    item(m_logMenu, CmdCopyLog, IDS_MI_COPY_LOG, L"Ctrl+C");
    item(m_logMenu, CmdClearLog, IDS_MI_CLEAR_LOG);

    RECT client{};
    GetClientRect(m_hwnd, &client);
    if (client.right > 0) Layout();  // the menu bar width depends on the language
}

void MainWindow::BuildAccelerators() {
    ACCEL keys[] = {
        {FVIRTKEY | FCONTROL, 'I', CmdInstall}, {FVIRTKEY, VK_F5, CmdCheck},          {FVIRTKEY | FCONTROL, 'R', CmdLaunch},
        {FVIRTKEY | FCONTROL, 'Q', CmdExit},    {FVIRTKEY | FCONTROL, 'E', CmdSourceCustom}, {FVIRTKEY | FCONTROL, 'L', CmdViewLog},
        {FVIRTKEY, VK_F1, CmdHelpCli},          {FVIRTKEY | FCONTROL, 'B', CmdBtStart},      {FVIRTKEY | FCONTROL | FSHIFT, 'R', CmdStopWsa},
    };
    m_accel.reset(CreateAcceleratorTableW(keys, static_cast<int>(std::size(keys))));
}

// Enabled / checked state of every item of `menu`, from the same rules as the buttons.
void MainWindow::ApplyMenuState(HMENU menu) const {
    const std::wstring source = SourceKey();
    const bool automatic = Text::Automatic();
    const std::wstring language = Text::Current();
    const std::uint32_t remember = Host::HelloRememberMinutes();
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        const UINT id = GetMenuItemID(menu, i);
        if (id == static_cast<UINT>(-1) || id == 0) continue;  // separator or submenu
        EnableMenuItem(menu, id, MF_BYCOMMAND | (Can(id) ? MF_ENABLED : MF_GRAYED));
        bool checked = false;
        if (id == CmdLanguageAuto) {
            checked = automatic;
        } else if (id >= CmdLanguageFirst && id <= CmdLanguageLast) {
            const std::size_t index = id - CmdLanguageFirst;
            checked = !automatic && index < m_languages.size() && m_languages[index].code == language;
        } else {
            switch (id) {
            case CmdSourceGitHub: checked = source == L"github"; break;
            case CmdSourceMirror: checked = source == L"mirror"; break;
            case CmdSourceLocal: checked = source == L"local"; break;
            case CmdSourceCustom: checked = source == L"custom"; break;
            case CmdViewLog: checked = m_logVisible; break;
            case CmdBtAutostart: checked = m_btAutostart; break;
            case CmdElevation: checked = Host::ElevationEnabled(); break;
            case CmdElevationSu: checked = Host::ElevationSuEnabled(); break;
            case CmdTray: checked = Host::TrayEnabled(); break;
            case CmdHelloRememberOff: checked = remember == 0; break;
            case CmdHelloRemember5: checked = remember == 5; break;
            case CmdHelloRemember30: checked = remember == 30; break;
            case CmdHelloRememberLock: checked = remember == Host::kHelloUntilLocked; break;
            default: continue;
            }
        }
        CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    }
}

void MainWindow::ShowContextMenu(HWND target, POINT screen) {
    if (target == m_log.hwnd()) m_menu.TrackContext(m_hwnd, m_logMenu, screen);
}

}  // namespace WN::Ui
