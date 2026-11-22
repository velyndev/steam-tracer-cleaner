#pragma once

// Shared helpers for the shell and the pages.
//
// Non-visual: UTF-8 conversions, byte and timestamp formatting, opening URLs and folders, the
// native file dialogs, the project URLs.
// Visual: the page header every page starts with, and the "Steam was not found" state.
//
// Everything here runs on the UI thread.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include <facet/widgets/overlays.h>

namespace stc::app {
struct AppState;
}

namespace stc::ui {

inline constexpr const char* kRepoUrl = "https://github.com/velyndev/steam-tracer-cleaner";
inline constexpr const char* kReleasesUrl = "https://github.com/velyndev/steam-tracer-cleaner/releases";

// --- Text ----------------------------------------------------------------------------------------

std::string to_utf8(std::wstring_view s);
std::wstring from_utf8(std::string_view s);
std::string path_to_utf8(const std::filesystem::path& p);

// "512 B", "14 KiB", "3.2 MiB", "1.25 GiB".
std::string format_bytes(std::uint64_t bytes);

// A backup timestamp in its stored UTC form "20260508T153000Z" as a local date and time in the
// user's short date format, for example "08.05.2026 17:30". Returns the input unchanged (as
// UTF-8) when it is not in that form.
std::string format_backup_timestamp(std::wstring_view stamp);

// How long ago a stored backup timestamp was, in words and sentence case: "Just now",
// "5 minutes ago", "3 hours ago", "Yesterday", "6 days ago", "3 weeks ago", "4 months ago",
// "2 years ago". Empty when the stamp is not in the stored form.
std::string format_backup_age(std::wstring_view stamp);

// --- Shell actions -------------------------------------------------------------------------------

// Opens a URL in the default browser.
void open_url(std::string_view url);
// Opens a folder in Explorer, or for a file, its folder with the file selected. Returns false
// when the path does not exist.
bool open_in_explorer(const std::filesystem::path& path);

// --- Native dialogs ------------------------------------------------------------------------------

// The window that owns the dialogs. The shell sets it every frame from the host.
void set_dialog_owner(void* hwnd);

struct FileFilter {
    const wchar_t* name;     // "Config files"
    const wchar_t* pattern;  // "*.cfg;*.txt"
};

// Modal Windows file dialogs owned by the app window. They block the frame loop while open,
// which is what a modal dialog means here. Return nullopt when the user cancels.
std::optional<std::filesystem::path> open_file_dialog(const wchar_t* title, std::span<const FileFilter> filters);
std::optional<std::filesystem::path> save_file_dialog(const wchar_t* title, std::span<const FileFilter> filters,
                                                      const wchar_t* default_name,
                                                      const wchar_t* default_extension);
std::optional<std::filesystem::path> pick_folder(const wchar_t* title);

// --- Toasts with an action -----------------------------------------------------------------------

// Shows a toast whose action button runs `on_action`, whichever page is open when it is pressed.
// facet::ToastActionPressed is true for one frame only, so a page that polls its own toast misses
// a press made after the user moved to another page; the shell polls every routed toast instead.
// `options.actionLabel` must be set. `on_action` runs on the UI thread and may capture the
// AppState by reference, but nothing a page owns that could be gone by then.
facet::ToastId show_action_toast(const char* text, const facet::ToastOptions& options,
                                 std::function<void()> on_action);
// Runs the action of every routed toast that was pressed. The shell calls it once per frame.
void poll_toast_actions();

// --- Page building blocks ------------------------------------------------------------------------

// The header every page starts with: the title in Title1 (the main heading of the window) and a
// description in Body and the secondary label color under it, wrapped when it does not fit. Items submitted between begin_page_header and end_page_header
// are the trailing actions: they sit right-aligned on the title row, laid out with
// ImGui::SameLine between them (their width from the last frame is used to place them).
//
//   ui::begin_page_header("Backups", "Restore points made before each cleanup.");
//   if (facet::Button("Open folder")) ...
//   ui::end_page_header();
//
// end_page_header leaves the cursor under the description with spacing.xl of room.
void begin_page_header(const char* title, const char* description);
void end_page_header();
// A header without actions.
void page_header(const char* title, const char* description);

// --- Page layout: one column, no width cap -------------------------------------------------------
//
// The layout rule every page follows, so all pages share one set of edges, boxes and gaps:
//
//   - A page is one column that fills the whole content width. There is no width cap anywhere.
//     The page header, every card and every block start at the same left edge (the left edge of
//     the page title) and end at the same right edge (the right edge of the header's trailing
//     actions), at every window size and scale.
//   - Order: page_header / begin_page_header ... end_page_header, then fixed parts that must stay
//     in view (a summary line, a filter row), then begin_page_body ... end_page_body for the part
//     that scrolls, then fixed parts at the bottom if any (a footer with the primary action).
//   - Groups of content are page cards (begin_page_card / end_page_card): one surface, one corner
//     radius, one padding, the optional title in Headline and description in the secondary label
//     color at the top inside the card, one gap between that heading and the content. Cards are
//     never placed side by side with ad hoc widths; a page that needs two columns inside a card
//     lays them out inside that card.
//   - Between two blocks (cards, a summary line and a card, a card and a footer) the gap is
//     page_block_gap(): the same spacing token the header leaves under itself. Nothing else adds
//     vertical space between blocks.
//   - Sizes come from the tokens in ui/common.cpp (kPageBlockGap, kCardHeadingGap, kCardTitleGap,
//     all facet::T().spacing; the card padding is facet::BeginCard's spacing.lg), never from
//     literals in a page.
//
//   ui::page_header("Exclusions", "What a cleanup never touches.");
//   if (ui::begin_page_body("body")) {
//       ui::begin_page_card("paths", "Folders and files", "Kept by every cleanup.");
//       ...
//       ui::end_page_card();
//       ui::page_block_gap();
//       ui::begin_page_card("accounts", "Accounts");
//       ...
//       ui::end_page_card();
//       ui::end_page_body();
//   }

// The width of the page column in pixels: the full available width of the current content region.
// Fixed parts of a page outside the scrolling body use it to size themselves.
float page_column_width();

// --- Column scroll: the one scrolling region of the app ---
//
// Rule for every scrolling region the app lays out itself: its content edges do not depend on
// overflow, and its bar never covers content. A column scroll is a Facet scroll area whose content
// is exactly a column: it starts at the cursor, is `width` units wide (0: the available width) and
// `height` units tall (0: to the bottom of the region). The content's left edge is the column's
// left edge (Facet's own padding is compensated), its right edge (what full-width items and the
// available width end at) is the column's right edge in every frame, whether the content overflows
// or not, and the bar runs in a strip column_scroll_gutter() pixels wide right of the column. The
// caller leaves that strip free: the page's side padding (spacing.xxl) holds it beside the page
// body, a page card's own padding (spacing.lg, the same 16 units) holds it inside a card, and two
// columns side by side keep at least that much between them. Returns false while the area is out
// of sight; call end_column_scroll only after true. Ids inside are scoped under `id`. Nests up to
// four deep. Facet widgets that scroll themselves (VirtualList, DataTable, CodeView, the
// checklist) are not column scrolls; place them so their own inset and gutter fit (see the call
// sites).
[[nodiscard]] bool begin_column_scroll(const char* id, float width = 0.0F, float height = 0.0F);
void end_column_scroll();
// The strip right of a column scroll's column that holds its bar (pixels): Facet's
// scroll.barInset + barThicknessHover + barGap.
float column_scroll_gutter();

// The scrolling body of a page: a column scroll over the page column that fills the rest of the
// page height (from the cursor to the bottom of the page region), so the content's left edge equals
// the page title's left edge and its right edge the header's right edge, to the pixel, whether the
// page scrolls or not; the bar sits in the page's right padding. Returns false while the area is
// out of sight; call end_page_body only after true (it also closes column scrolls left open inside).
[[nodiscard]] bool begin_page_body(const char* id);
void end_page_body();

// THE card for a group of content on every page, as wide as the column. `title` (Headline) and
// `description` (one line, secondary label color, wraps when narrow) are optional; null or empty
// leaves them out. `id` keys the card and scopes the ids of its content. Always returns true (the
// content is always laid out); call end_page_card after every begin_page_card. Usable inside and
// outside the page body.
bool begin_page_card(const char* id, const char* title = nullptr, const char* description = nullptr);
void end_page_card();

// The one vertical gap between two blocks of a page (cards, a summary line, a footer). The page
// header leaves the same gap under itself.
void page_block_gap();

// Where the layout helpers put things in the last frames (pixels), for the selftest. Each group
// carries the frame number it was written in; compare it with ImGui::GetFrameCount() - 1.
struct PageLayoutDebug {
    int header_frame = -1;
    float header_left = 0.0F;
    float header_right = 0.0F;          // the column's right edge as the header sees it
    float header_actions_right = 0.0F;  // the right edge of the trailing actions; 0 without actions
    int card_frame = -1;                // the first page card of that frame
    float card_left = 0.0F;
    float card_right = 0.0F;
    int body_frame = -1;
    float body_content_top = 0.0F;      // the scrolled top of the body's content
    float body_scroll_y = 0.0F;
    float body_scroll_max = 0.0F;
    struct {
        ImVec2 min{0.0F, 0.0F};
        ImVec2 max{0.0F, 0.0F};
    } body_rect;                        // the scroll area's window
};
const PageLayoutDebug& page_layout_debug();

// One column scroll of the current frame (pixels), for the selftest: the column, the area, what
// Facet measured against the app's formulas, and the right extent of everything drawn inside.
struct ColumnScrollDebug {
    char id[48] = {};
    int frame = -1;
    float column_left = 0.0F;
    float column_right = 0.0F;
    float area_right = 0.0F;
    float content_max_x = 0.0F;  // the right edge of the widest item inside (0 until the area ended)
    float content_top = 0.0F;    // the scrolled top of the content
    bool overflows = false;
    float facet_inset = 0.0F;    // Facet's padding
    float facet_gutter = 0.0F;   // area right edge to Facet's own right edge: the gutter while it overflows
    float inset = 0.0F;          // the app's formulas
    float gutter = 0.0F;
    float scroll_y = 0.0F;
    float scroll_max = 0.0F;
    ImVec2 rect_min{0.0F, 0.0F};
    ImVec2 rect_max{0.0F, 0.0F};
};
// The column scrolls drawn in the frame that is being drawn, or the last one once it ended.
const std::vector<ColumnScrollDebug>& column_scroll_debug();

// The full-page state for "no Steam install was detected", with a working "Re-detect Steam"
// action (state.refresh_steam()). Pages that need Steam draw it instead of their content:
//
//   if (!state.install) { ui::steam_not_found_state(state); return; }
void steam_not_found_state(stc::app::AppState& state);

}  // namespace stc::ui
