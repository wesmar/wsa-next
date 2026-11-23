// WSA-Next © 2026 Marek Wesołowski - wesmar
// Main window: layout, window placement and painting.
#include "gui/MainWindow.h"
#include "gui/MainWindowDetail.h"
#include <dwmapi.h>
#include "support/Settings.h"
#include "support/Signature.h"
#include "support/Text.h"
#include <algorithm>
#include <cmath>
#include <format>

namespace WN::Ui {
using namespace MainWindowDetail;
namespace {

// Heights inside the cards, DIP.
constexpr float kTitleH = 30, kMetaH = 20, kHeaderH = 22, kSystemRowH = 40, kComponentRowH = 24;
constexpr std::size_t kMinComponentRows = 7;
constexpr float kReleaseH = kPad + kTitleH + kMetaH + 12 + 3 * kRowH + 2 * 6 + 12 + 4 + 12 + kButtonH + kPad;

float LowerCardsHeight(std::size_t components) {
    const float system = kPad + kHeaderH + 8 + 5 * kSystemRowH + kPad - 6;
    const float list = kPad + kHeaderH + 8 + static_cast<float>(std::max(components, kMinComponentRows)) * kComponentRowH + kPad;
    return std::max(system, list);
}

D2D1_COLOR_F LevelColor(Level level) {
    return level == Level::Error     ? Color::Critical
           : level == Level::Warning ? Color::Caution
           : level == Level::Success ? Color::Success
                                     : Color::Text2;
}

}  // namespace

float MainWindow::ContentHeight(bool withLog) const {
    const std::size_t components = m_manifest ? m_manifest->components.size() : 0;
    return 4 + kBarH + 6 + kReleaseH + 10 + LowerCardsHeight(components) + 6 + 28 + kMargin + (withLog ? kLogH : 0);
}

float MainWindow::ButtonWidth() const {
    float widest = m_fonts.F(kButtonW);
    for (UINT id : {CmdCancel, CmdLaunch, CmdInstall, CmdChangeFolder, CmdBtToggle}) {
        wchar_t text[128]{};
        GetWindowTextW(GetDlgItem(m_hwnd, static_cast<int>(id)), text, static_cast<int>(std::size(text)));
        const auto look = m_looks.find(id);
        const float icon = look != m_looks.end() && look->second.icon ? m_fonts.F(22) : 0;
        widest = std::max(widest, TextWidth(m_fonts, Font::Body, text) + icon + m_fonts.F(30));
    }
    return std::ceil(widest);
}

void MainWindow::Layout() {
    if (!m_log.hwnd()) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const auto& f = m_fonts;
    const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);
    const float m = f.F(kMargin), p = f.F(kPad), bw = ButtonWidth(), bh = f.F(kButtonH), gap = f.F(kGap), row = f.F(kRowH);
    auto place = [&](UINT id, D2D1_RECT_F r) {
        SetWindowPos(GetDlgItem(m_hwnd, static_cast<int>(id)), nullptr, static_cast<int>(std::lround(r.left)),
                     static_cast<int>(std::lround(r.top)), static_cast<int>(std::lround(r.right - r.left)),
                     static_cast<int>(std::lround(r.bottom - r.top)), SWP_NOZORDER | SWP_NOACTIVATE);
    };

    m_menu.Layout(f.F(6), f.F(4), f.F(kBarH));
    // "Support" on the menu bar's line: centred on the menu's text, as far above it as below,
    // under the window's buttons - from the left edge of "minimize" to the cards' right edge, the
    // same in every language (wider only for a label that needs more).
    {
        wchar_t text[64]{};
        GetWindowTextW(GetDlgItem(m_hwnd, CmdSupport), text, static_cast<int>(std::size(text)));
        const float needed = std::ceil(TextWidth(f, Font::Body, text) + f.F(14 + 8) + f.F(24));
        float left = W - m - needed;
        RECT buttons{}, window{};
        POINT origin{};
        if (SUCCEEDED(DwmGetWindowAttribute(m_hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &buttons, sizeof buttons)) && buttons.right > buttons.left &&
            GetWindowRect(m_hwnd, &window) && ClientToScreen(m_hwnd, &origin)) {
            // Window-relative bounds -> client coordinates.
            left = std::min(left, static_cast<float>(buttons.left - (origin.x - window.left)));
        }
        const float h = f.F(26), mid = f.F(4 + kBarH / 2);
        place(CmdSupport, {left, mid - h / 2, W - m, mid + h / 2});
    }
    float y = f.F(4 + kBarH + 6);

    // Release card
    const float left = m + p, right = W - m - p;
    m_releaseCard = {m, y, W - m, y + f.F(kReleaseH)};
    float cy = y + p;
    m_titleRect = {left, cy, right, cy + f.F(kTitleH)};
    cy += f.F(kTitleH);
    m_metaRect = {left, cy, right, cy + f.F(kMetaH)};
    cy += f.F(kMetaH + 12);
    m_labelWidth = f.F(80);
    for (UINT id : {IDS_LABEL_SOURCE, IDS_LABEL_FOLDER, IDS_LABEL_DOWNLOAD})
        m_labelWidth = std::max(m_labelWidth, std::ceil(TextWidth(f, Font::Label, Text::Get(id)) + f.F(16)));
    m_rowSource = {left, cy, right, cy + row};
    place(CmdSourceMenu, {left + m_labelWidth, cy, right, cy + row});
    cy += row + f.F(6);
    m_rowFolder = {left, cy, right - bw - gap, cy + row};
    place(CmdChangeFolder, {right - bw, cy, right, cy + row});
    cy += row + f.F(6);
    m_rowDownload = {left, cy, right - bw - gap, cy + row};
    place(CmdBtToggle, {right - bw, cy, right, cy + row});
    cy += row + f.F(12);
    m_progressRect = {left, cy, right, cy + f.F(4)};
    cy += f.F(4 + 12);
    place(CmdInstall, {right - bw, cy, right, cy + bh});
    place(CmdLaunch, {right - 2 * bw - gap, cy, right - bw - gap, cy + bh});
    place(CmdCancel, {right - 3 * bw - 2 * gap, cy, right - 2 * bw - 2 * gap, cy + bh});
    m_statusRect = {left, cy, right - 3 * bw - 2 * gap - f.F(12), cy + bh};

    // Log expander and log at the bottom.
    const float logH = m_logVisible ? std::max(f.F(110), std::min(f.F(kLogH), H * 0.4f)) : 0;
    const float expanderH = f.F(28);
    const float expanderTop = H - m - logH - expanderH - (m_logVisible ? f.F(4) : 0);
    place(CmdExpand, {m - f.F(4), expanderTop, m + f.F(160), expanderTop + expanderH});
    if (m_logVisible) {
        m_logCard = {m, expanderTop + expanderH + f.F(4), W - m, H - m};
        place(kIdLog, Inset(m_logCard, f.F(4), f.F(4)));
    }

    // This PC (left) and Components (right) share the space between.
    y = m_releaseCard.bottom + f.F(10);
    const float bottom = std::max(y + f.F(80), expanderTop - f.F(6));
    const float split = m + (W - 2 * m - f.F(10)) * 0.46f;
    m_systemCard = {m, y, split, bottom};
    m_componentsCard = {split + f.F(10), y, W - m, bottom};
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindow::SetLogVisible(bool visible, bool resizeWindow) {
    if (visible == m_logVisible) return;
    m_logVisible = visible;
    Settings::WriteDword(L"LogVisible", visible ? 1 : 0);
    if (visible) m_unread = 0;
    m_looks[CmdExpand].expanded = visible;
    m_looks[CmdExpand].badge.clear();
    ShowWindow(m_log.hwnd(), visible ? SW_SHOW : SW_HIDE);
    if (resizeWindow && !IsZoomed(m_hwnd) && !IsIconic(m_hwnd)) {
        // Grow / shrink the window by the log height (like "More details" in the copy dialog).
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        const int delta = m_fonts.S(static_cast<int>(kLogH)) * (visible ? 1 : -1);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        int height = wr.bottom - wr.top + delta;
        int top = wr.top;
        if (top + height > mi.rcWork.bottom) top = std::max<int>(mi.rcWork.top, mi.rcWork.bottom - height);
        height = std::min<int>(height, mi.rcWork.bottom - mi.rcWork.top);
        SetWindowPos(m_hwnd, nullptr, wr.left, top, wr.right - wr.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout();
    InvalidateRect(GetDlgItem(m_hwnd, CmdExpand), nullptr, FALSE);
}

void MainWindow::RestorePlacement(int showCommand) {
    const UINT dpi = GetDpiForWindow(m_hwnd);
    RECT desired{0, 0, MulDiv(720, static_cast<int>(dpi), 96), MulDiv(static_cast<int>(ContentHeight(m_logVisible)), static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = desired.right - desired.left, h = desired.bottom - desired.top;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    h = std::min<int>(h, mi.rcWork.bottom - mi.rcWork.top);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;

    // Saved position (physical pixels) when it is still on a monitor.
    int sx = 0, sy = 0, sw = 0, sh = 0;
    const std::wstring saved = Settings::Read(L"WindowRect");
    if (swscanf_s(saved.c_str(), L"%d %d %d %d", &sx, &sy, &sw, &sh) == 4 && sw > 200 && sh > 200) {
        const RECT r{sx, sy, sx + sw, sy + sh};
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            x = sx; y = sy; w = sw; h = sh;
        }
    }
    SetWindowPos(m_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    ShowWindow(m_hwnd, showCommand == SW_SHOWMINIMIZED || showCommand == SW_MINIMIZE ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(m_hwnd);
}

void MainWindow::SavePlacement() {
    WINDOWPLACEMENT wp{sizeof wp};
    if (!GetWindowPlacement(m_hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    Settings::Write(L"WindowRect", std::format(L"{} {} {} {}", r.left, r.top, r.right - r.left, r.bottom - r.top));
}

void MainWindow::Paint() {
    PaintScope ps(m_hwnd);
    RECT client{};
    GetClientRect(m_hwnd, &client);
    Ui::Paint(ps.dc(), client, m_fonts, [&](Canvas& c) {
        c.Clear(Graphics::Get().Base());
        m_menu.Paint(c);
        PaintReleaseCard(c);
        PaintSystemCard(c);
        PaintComponentsCard(c);
        if (m_logVisible) {
            c.Fill(m_logCard, Color::Card, m_fonts.F(kRadius));
            c.Stroke(m_logCard, Color::CardStroke, m_fonts.F(kRadius));
        }
    });
}

void MainWindow::PaintReleaseCard(Canvas& c) {
    const auto& f = m_fonts;
    c.Fill(m_releaseCard, Color::Card, f.F(kRadius));
    c.Stroke(m_releaseCard, Color::CardStroke, f.F(kRadius));

    // Title with a green Android mark, installed release on the right.
    const float dot = f.F(5);
    c.Circle({m_titleRect.left + dot, (m_titleRect.top + m_titleRect.bottom) / 2}, dot, Color::AccentText);
    const D2D1_RECT_F title{m_titleRect.left + 3 * dot, m_titleRect.top, m_titleRect.right, m_titleRect.bottom};
    c.Text(Text::Get(IDS_CARD_RELEASE), title, Font::Title, Color::Text);
    const std::wstring installed =
        m_state.version.empty() ? Text::Get(IDS_INSTALLED_NONE) : Text::Format(IDS_INSTALLED_META, m_state.version);
    c.Text(installed, m_titleRect, Font::Caption, m_state.version.empty() ? Color::Text3 : Color::Success, Align::Right);

    std::wstring meta = Text::Get(IDS_RELEASE_NONE);
    if (m_manifest) meta = Text::Format(IDS_RELEASE_META, m_manifest->version, m_manifest->android, m_manifest->variant);
    else if (!m_manifestError.empty()) meta = m_manifestError;
    c.Text(meta, {title.left, m_metaRect.top, m_metaRect.right, m_metaRect.bottom}, Font::Caption,
           m_manifest || m_manifestError.empty() ? Color::Text2 : Color::Caution);

    auto label = [&](const D2D1_RECT_F& r, UINT id) {
        c.Text(Text::Get(id), {r.left, r.top, r.left + m_labelWidth, r.bottom}, Font::Label, Color::Text3);
    };
    label(m_rowSource, IDS_LABEL_SOURCE);
    label(m_rowFolder, IDS_LABEL_FOLDER);
    c.Text(m_folder.wstring(), {m_rowFolder.left + m_labelWidth, m_rowFolder.top, m_rowFolder.right, m_rowFolder.bottom}, Font::Body,
           Color::Text);
    label(m_rowDownload, IDS_LABEL_DOWNLOAD);
    std::wstring summary = L"\x2014";
    if (m_manifest)
        summary = m_plan.UpToDate() ? Text::Get(IDS_SUMMARY_NONE)
                                    : Text::Format(IDS_SUMMARY_DOWNLOAD, FormatBytes(m_plan.downloadBytes), FormatBytes(m_plan.diskBytes));
    c.Text(summary, {m_rowDownload.left + m_labelWidth, m_rowDownload.top, m_rowDownload.right, m_rowDownload.bottom}, Font::Body,
           Color::Text);

    // Progress: track, determinate fill (animated) or an indeterminate sliding segment.
    const D2D1_RECT_F& t = m_progressRect;
    const float radius = (t.bottom - t.top) / 2;
    c.Fill(t, Color::Track, radius);
    const D2D1_COLOR_F fill = m_failed ? Color::Critical : (m_finished && !m_busy) ? Color::Success : Color::AccentText;
    if ((m_busy || m_refreshing) && m_progress.permille < 0) {
        const float width = t.right - t.left, segment = width * 0.28f;
        const float phase = static_cast<float>((GetTickCount64() - m_animStart) % 1600) / 1600.0f;
        const float x = t.left - segment + (width + segment) * phase;
        c.PushClip(t);
        c.Fill({std::max(x, t.left), t.top, std::min(x + segment, t.right), t.bottom}, fill, radius);
        c.PopClip();
    } else if (m_shown > 0.001f) {
        c.Fill({t.left, t.top, t.left + (t.right - t.left) * std::min(m_shown, 1.0f), t.bottom}, fill, radius);
    }
    c.Text(m_status, m_statusRect, Font::Caption, LevelColor(m_statusLevel));
}

void MainWindow::PaintSystemCard(Canvas& c) {
    const auto& f = m_fonts;
    const D2D1_RECT_F& card = m_systemCard;
    c.Fill(card, Color::Card, f.F(kRadius));
    c.Stroke(card, Color::CardStroke, f.F(kRadius));
    const float left = card.left + f.F(kPad), right = card.right - f.F(kPad);
    float y = card.top + f.F(kPad);
    c.Text(Text::Get(IDS_CARD_SYSTEM), {left, y, right, y + f.F(kHeaderH)}, Font::Strong, Color::Text);
    y += f.F(kHeaderH + 8);

    // A row: status mark, label, value; `note` (what the installation will change) sits right of the label.
    enum class Mark { Ok, Todo, Bad, Wait };
    auto rowAt = [&](UINT labelId, const std::wstring& value, Mark mark, const std::wstring& note = {}) {
        const D2D1_COLOR_F color = mark == Mark::Ok ? Color::Success : mark == Mark::Todo ? Color::Caution
                                   : mark == Mark::Bad ? Color::Critical : Color::Text3;
        const wchar_t glyph = mark == Mark::Ok ? Icon::StatusOk : mark == Mark::Bad ? Icon::StatusError : mark == Mark::Todo ? Icon::Info : Icon::Refresh;
        const float icon = f.F(22);
        c.Text(std::wstring(1, glyph), {left, y + f.F(2), left + icon, y + f.F(20)}, Font::IconSmall, color, Align::Left);
        const std::wstring label = Text::Get(labelId);
        c.Text(label, {left + icon, y, right, y + f.F(18)}, Font::Label, Color::Text3);
        if (!note.empty()) {
            const float x = left + icon + c.Width(label, Font::Label) + f.F(12);
            c.Text(note, {x, y, right, y + f.F(18)}, Font::Caption, Color::Caution, Align::Right);
        }
        c.Text(value, {left + icon, y + f.F(18), right, y + f.F(kSystemRowH - 4)}, Font::Body, mark == Mark::Bad ? Color::Critical : Color::Text);
        y += f.F(kSystemRowH);
    };

    if (!m_system) {
        for (UINT id : {IDS_SYS_WINDOWS, IDS_SYS_VMP, IDS_SYS_DEVMODE, IDS_SYS_PACKAGE, IDS_SYS_BLUETOOTH})
            rowAt(id, Text::Get(IDS_SYS_READING), Mark::Wait);
        return;
    }
    const Core::SystemInfo& s = *m_system;
    if (!s.Supported()) rowAt(IDS_SYS_WINDOWS, s.Version(), Mark::Bad, Text::Get(IDS_SYS_UNSUPPORTED));
    else if (s.NeedsPatch()) rowAt(IDS_SYS_WINDOWS, s.Version(), Mark::Todo, Text::Get(IDS_SYS_PATCH));
    else rowAt(IDS_SYS_WINDOWS, s.Version(), Mark::Ok);

    if (!s.virtualMachinePlatform) rowAt(IDS_SYS_VMP, Text::Get(IDS_UNKNOWN), Mark::Wait);
    else if (*s.virtualMachinePlatform) rowAt(IDS_SYS_VMP, Text::Get(IDS_SYS_ENABLED), Mark::Ok);
    else rowAt(IDS_SYS_VMP, Text::Get(IDS_SYS_DISABLED), Mark::Todo, Text::Get(IDS_SYS_VMP_OFF));
    if (s.developerMode) rowAt(IDS_SYS_DEVMODE, Text::Get(IDS_SYS_DEVMODE_ON), Mark::Ok);
    else rowAt(IDS_SYS_DEVMODE, Text::Get(IDS_SYS_DEVMODE_DISABLED), Mark::Todo, Text::Get(IDS_SYS_DEVMODE_OFF));

    std::error_code ec;
    const std::wstring replaced = Text::Get(IDS_SYS_PKG_REPLACED);
    if (!s.package) rowAt(IDS_SYS_PACKAGE, Text::Get(IDS_SYS_PKG_NONE), Mark::Todo);
    else if (!s.package->developmentMode) rowAt(IDS_SYS_PACKAGE, Text::Format(IDS_SYS_PKG_STORE, s.package->version), Mark::Todo, replaced);
    else if (std::filesystem::equivalent(s.package->location, m_folder, ec)) rowAt(IDS_SYS_PACKAGE, Text::Format(IDS_SYS_PKG_OURS, s.package->version), Mark::Ok);
    else rowAt(IDS_SYS_PACKAGE, Text::Format(IDS_SYS_PKG_FOLDER, s.package->version, s.package->location.wstring()), Mark::Todo, replaced);

    // Bluetooth for Android: the PC's adapter through the background bridge.
    if (!m_btKnown) {
        rowAt(IDS_SYS_BLUETOOTH, Text::Get(IDS_SYS_READING), Mark::Wait);
    } else if (!m_btAdapter) {
        rowAt(IDS_SYS_BLUETOOTH, Text::Get(IDS_SYS_BT_NONE), Mark::Wait);
    } else if (!Signature::OfficialFeatures()) {
        rowAt(IDS_SYS_BLUETOOTH, Text::Get(IDS_SYS_BT_UNOFFICIAL), Mark::Wait);
    } else if (m_release == Core::Release::State::Checking) {
        rowAt(IDS_SYS_BLUETOOTH, Text::Get(IDS_SYS_BT_CHECKING), Mark::Wait);
    } else if (m_release == Core::Release::State::Unsigned || m_release == Core::Release::State::Modified) {
        rowAt(IDS_SYS_BLUETOOTH, Text::Get(IDS_SYS_BT_RELEASE), Mark::Wait);
    } else if (m_btRunning) {
        rowAt(IDS_SYS_BLUETOOTH, Text::Get(m_btConnected ? IDS_SYS_BT_CONNECTED : IDS_SYS_BT_ON), m_btConnected ? Mark::Ok : Mark::Wait,
              m_btAutostart ? Text::Get(IDS_SYS_BT_AUTOSTART) : std::wstring());
    } else {
        rowAt(IDS_SYS_BLUETOOTH, Text::Get(IDS_SYS_BT_OFF), Mark::Todo, Text::Get(IDS_SYS_BT_HINT));
    }
}

void MainWindow::PaintComponentsCard(Canvas& c) {
    const auto& f = m_fonts;
    const D2D1_RECT_F& card = m_componentsCard;
    c.Fill(card, Color::Card, f.F(kRadius));
    c.Stroke(card, Color::CardStroke, f.F(kRadius));
    const float left = card.left + f.F(kPad), right = card.right - f.F(kPad);
    float y = card.top + f.F(kPad);
    c.Text(Text::Get(IDS_CARD_COMPONENTS), {left, y, right, y + f.F(kHeaderH)}, Font::Strong, Color::Text);
    y += f.F(kHeaderH + 8);
    if (!m_manifest) {
        c.Paragraph(Text::Get(IDS_COMPONENTS_EMPTY), {left, y, right, card.bottom - f.F(kPad)}, Font::Caption, Color::Text3);
        return;
    }
    c.PushClip({card.left, y, card.right, card.bottom - f.F(6)});
    // The state pills share one width, wide enough for the longest state in this language.
    float badgeW = f.F(78);
    for (const UINT id : {IDS_COMP_INSTALLED, IDS_COMP_UPDATE, IDS_COMP_NEW}) badgeW = std::max(badgeW, c.Width(Text::Get(id), Font::Caption) + f.F(16));
    const float sizeW = f.F(70);
    for (const Core::PlanItem& item : m_plan.items) {
        const Core::Component& comp = *item.component;
        const bool installed = !item.needed;
        const bool known = m_state.components.contains(comp.id);
        const UINT stateId = installed ? IDS_COMP_INSTALLED : known ? IDS_COMP_UPDATE : IDS_COMP_NEW;
        const D2D1_COLOR_F stateColor = installed ? Color::Success : known ? Color::AccentText : Color::Text2;
        const D2D1_RECT_F rowRect{left, y, right, y + f.F(kComponentRowH)};
        c.Text(ComponentName(comp), {rowRect.left, rowRect.top, rowRect.right - badgeW - sizeW - f.F(8), rowRect.bottom}, Font::Body, Color::Text);
        c.Text(FormatBytes(comp.packedSize), {rowRect.right - badgeW - sizeW, rowRect.top, rowRect.right - badgeW - f.F(8), rowRect.bottom},
               Font::Caption, Color::Text3, Align::Right);
        // State as a small pill.
        const float pillH = f.F(18);
        const D2D1_RECT_F pill{rowRect.right - badgeW, (rowRect.top + rowRect.bottom - pillH) / 2, rowRect.right,
                               (rowRect.top + rowRect.bottom + pillH) / 2};
        c.Fill(pill, D2D1_COLOR_F{stateColor.r, stateColor.g, stateColor.b, 0.14f}, pillH / 2);
        c.Text(Text::Get(stateId), pill, Font::Caption, stateColor, Align::Center);
        y += f.F(kComponentRowH);
    }
    c.PopClip();
}

}  // namespace WN::Ui
