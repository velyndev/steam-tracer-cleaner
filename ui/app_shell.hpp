#pragma once

// The app shell: title bar (with the background activity indicator and the update indicator),
// sidebar, content region, command palette, update dialog and toasts, drawn with Facet over the
// whole borderless host window. There is no status bar: Steam details and the version live in
// Settings, a running job shows in the title bar.
//
// What a page may assume (the contract for ui/pages/<name>_page.cpp):
//
//   - It is one function, void draw_<name>_page(stc::app::AppState&) in stc::ui::pages, called
//     once per frame while its page is selected, inside the content region's child window.
//   - The child window is already padded (spacing.xxl left and right, spacing.xl top and bottom)
//     and has no scrollbar of its own. ImGui::GetContentRegionAvail() is the page's whole area.
//   - One column, no width cap: the page follows the layout rule in ui/common.hpp. It starts with
//     stc::ui::page_header() or begin_page_header()/end_page_header(), puts the part that scrolls
//     in stc::ui::begin_page_body()/end_page_body() (never a bare facet::BeginScrollArea, whose
//     inset would move the content off the column; a scrolling pane inside a card is a
//     stc::ui::begin_column_scroll for the same reason), groups content in
//     begin_page_card()/end_page_card() and separates blocks with page_block_gap(). Fixed parts
//     (a summary line, a footer with the primary action) sit outside the body. So every page has
//     the same left and right edge, the same card and the same gaps.
//   - The page works from 540 design units of available width upward (the window minimum of
//     760 x 520 units leaves about 650 next to the icon rail) and at UI scale 1.5. Long paths go
//     through facet::TruncatedText with TruncateMode::Middle.
//   - A page that needs Steam shows stc::ui::steam_not_found_state(state) when !state.install.
//   - Long work goes through state.tasks (app/task_runner.hpp), never on the UI thread. The shell
//     polls the runner before the page is drawn and shows a job that runs longer than a moment
//     in the title bar (spinner, label, "92 of 165" progress when the job reports a total).
//   - The content region runs to the bottom of the window; nothing of the shell sits under it.
//   - Navigation: assign state.current_page. Toasts: facet::ShowToast; the stack sits in the
//     bottom-right corner (set once here). Dialogs: facet::BeginModal / facet::ConfirmDialog,
//     called from the page function; they float above the shell.
//   - The page keeps its own UI state (selection, open dialogs) in AppState fields or in
//     function-local statics; the shell keeps none for it.

#include <imgui.h>

#include <string>

namespace stc::app {
struct AppState;
}

namespace stc::ui {

// What the shell needs from the host window. Requests are posted, so they run after the frame.
class HostWindow {
public:
    virtual ~HostWindow() = default;
    virtual void minimize() = 0;
    virtual void toggle_maximize() = 0;
    virtual void request_close() = 0;
    virtual bool maximized() const = 0;
    // The HWND, owner of native dialogs; null when there is none.
    virtual void* native_handle() const = 0;
    // While blocked, the host answers the title bar's caption and maximize button as client area,
    // so a click there reaches a popup or window that covers the bar.
    virtual void set_caption_blocked(bool blocked) = 0;
};

// Draws one frame of the app between facet::NewFrame and facet::EndFrame: polls the task runner,
// applies appearance changes, then draws everything. Returns false when nothing was drawn (the
// host window is minimized).
bool draw_app(stc::app::AppState& state, HostWindow& host);

// Rectangles and flags of the last frame, for the selftest (pixels, main viewport coordinates).
struct ShellRect {
    ImVec2 min{0.0F, 0.0F};
    ImVec2 max{0.0F, 0.0F};
    bool valid() const { return max.x > min.x && max.y > min.y; }
    ImVec2 center() const { return ImVec2((min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F); }
};
struct ShellDebug {
    ShellRect sidebar_items[6];  // in stc::app::Page order
    ShellRect palette_button;
    bool palette_open = false;
    bool sidebar_rail = false;
    // The title bar activity indicator; invalid and empty while it is hidden.
    ShellRect activity;
    std::string activity_label;
    // The bottom edge of the sidebar column and of the window (pixels), to check that nothing
    // takes the space under the content.
    float sidebar_bottom = 0.0F;
    float window_bottom = 0.0F;
};
const ShellDebug& shell_debug();

// Registers the shell's shortcuts (Ctrl+K). Idempotent; draw_app calls it.
void register_shell_shortcuts();

}  // namespace stc::ui
