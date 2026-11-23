// WSA-Next © 2026 Marek Wesołowski - wesmar
// Controls drawn with the Direct2D canvas: owner-drawn push buttons and the log view. All of them are real child windows, so Tab, focus and keyboard work as usual.
#pragma once
#include "gui/Theme.h"
#include "support/Common.h"
#include <Windows.h>
#include <optional>
#include <string>
#include <vector>

namespace WN::Ui {

// ---------------------------------------------------------------------------
// Buttons (BS_OWNERDRAW, drawn from the parent's WM_DRAWITEM)
// ---------------------------------------------------------------------------
enum class ButtonKind { Standard, Accent, Combo, Expander, Icon };

struct ButtonLook {
    ButtonKind kind = ButtonKind::Standard;
    wchar_t icon = 0;        // Segoe Fluent Icons code point
    bool onCard = true;      // sits on a card layer (else directly on the window background)
    bool expanded = false;   // Expander
    std::wstring badge;      // Expander: small count next to the label
    std::optional<D2D1_COLOR_F> iconColor;  // override icon colour when enabled
    std::optional<D2D1_COLOR_F> glowColor;  // active glow tint + border when enabled
    std::optional<D2D1_COLOR_F> iconHotColor;  // icon colour under the mouse, pressed or focused
};

HWND CreateButton(HWND parent, int id, const wchar_t* text);
void DrawButton(const DRAWITEMSTRUCT& dis, const Fonts& fonts, const ButtonLook& look);

// Common icon code points (Segoe Fluent Icons / Segoe MDL2 Assets).
namespace Icon {
inline constexpr wchar_t ChevronDown = 0xE70D, ChevronUp = 0xE70E, ChevronRight = 0xE76C;
inline constexpr wchar_t ArrowUp = 0xE74A, ArrowDown = 0xE74B, Delete = 0xE74D, Add = 0xE710;
inline constexpr wchar_t Play = 0xE768, Pin = 0xE718, Download = 0xE896, Cancel = 0xE711;
inline constexpr wchar_t Folder = 0xE8B7, Check = 0xE73E, Refresh = 0xE72C, Power = 0xE7E8;
inline constexpr wchar_t Sync = 0xE895, Repair = 0xE90F, Rocket = 0xF3B3, OpenInNew = 0xE8A7;
inline constexpr wchar_t StatusOk = 0xE930, StatusWarn = 0xE7BA, StatusError = 0xEA39, Info = 0xE946;
inline constexpr wchar_t Bluetooth = 0xE702, Heart = 0xEB51, HeartFill = 0xEB52;
}  // namespace Icon

// ---------------------------------------------------------------------------
// Vertical scrolling shared by the list and the log.
// ---------------------------------------------------------------------------
struct Scroller {
    float offset = 0, content = 0, view = 0;
    bool dragging = false, hot = false;
    float grabY = 0, grabOffset = 0;

    [[nodiscard]] float Max() const { return content > view ? content - view : 0; }
    [[nodiscard]] bool Visible() const { return content > view + 0.5f; }
    void Clamp();
    void ScrollTo(float y) { offset = y; Clamp(); }
    void EnsureVisible(float top, float bottom);
    [[nodiscard]] D2D1_RECT_F Track(const RECT& client, const Fonts& f) const;
    [[nodiscard]] D2D1_RECT_F Thumb(const RECT& client, const Fonts& f) const;
    void Draw(Canvas& c, const RECT& client) const;
    bool MouseDown(POINT p, const RECT& client, const Fonts& f);  // true = handled (capture)
    bool MouseMove(POINT p, const RECT& client, const Fonts& f);  // true = offset changed
    bool Wheel(int delta, float step);                              // true = offset changed
};

// ---------------------------------------------------------------------------
// Log view: timestamped, coloured, wrapped lines. Ctrl+C / context menu copy the whole log.
// ---------------------------------------------------------------------------
class LogView {
public:
    explicit LogView(const Fonts& fonts) : m_fonts(fonts) {}
    LogView(const LogView&) = delete;
    LogView& operator=(const LogView&) = delete;

    HWND Create(HWND parent, int id);
    [[nodiscard]] HWND hwnd() const noexcept { return m_hwnd; }

    void Append(Level level, std::wstring text);
    void Clear();
    [[nodiscard]] std::wstring Text() const;
    [[nodiscard]] std::size_t Count() const noexcept { return m_lines.size(); }
    void Relayout();

private:
    struct Line {
        std::wstring time, text;
        Level level;
        float height = 0;  // cached for m_measuredWidth
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Measure(Canvas* c);
    void Paint();

    const Fonts& m_fonts;
    HWND m_hwnd = nullptr;
    std::vector<Line> m_lines;
    float m_measuredWidth = -1;
    bool m_follow = true;  // stay at the bottom while new lines arrive
    Scroller m_scroll;
};

// Copies text to the clipboard (CF_UNICODETEXT).
bool CopyToClipboard(HWND owner, const std::wstring& text);

}  // namespace WN::Ui
