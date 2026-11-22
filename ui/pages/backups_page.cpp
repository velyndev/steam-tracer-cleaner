#include "ui/pages/backups_page.hpp"

#include <imgui.h>

#include <facet/facet.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "app/app_state.hpp"
#include "core/backup.hpp"
#include "platform/process.hpp"
#include "ui/common.hpp"

namespace stc::ui::pages {
namespace {

using stc::app::AppState;
using stc::app::JobKind;
using stc::core::backup::Entry;

// The page is a list of restore points, newest first, not a table: there are only ever a handful
// (the retention setting keeps ten by default), and each one is a thing the user acts on, so each
// row says when it was made and what it holds in words, with one labelled action (Restore) and the
// rest behind "More". Sorting columns would add nothing for five rows.

constexpr float kRestoreWidth = 104.0F;  // units; the Restore button, fixed so every row lines up
constexpr int kSkeletonRows = 3;

enum class Busy { None, Restoring, Deleting };

struct BackupsUi {
    std::vector<Entry> entries;  // newest first
    facet::DataState list_state = facet::DataState::Loading;
    std::string list_error;
    bool loaded_once = false;
    int last_drawn_frame = -100;

    // The backup an open dialog acts on (a copy, so a reload cannot invalidate it).
    Entry pending;
    bool restore_open = false;
    bool delete_open = false;
    bool steam_running = false;
    double steam_checked_at = -1.0;

    // The row whose "More" menu is open (by its folder, so a reload keeps it on the same backup).
    std::filesystem::path menu_dir;
    bool menu_open = false;

    // The backup a restore or delete job works on.
    Busy busy = Busy::None;
    std::filesystem::path busy_dir;

    bool dev_applied = false;
};

BackupsUi g_ui;

std::string plural(std::uint64_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// "19 items, 1.7 MiB"
std::string contents_text(const Entry& e) { return plural(e.op_count, "item", "items") + ", " + format_bytes(e.size_bytes); }

void sort_newest_first() {
    std::ranges::stable_sort(g_ui.entries, [](const Entry& a, const Entry& b) { return a.timestamp > b.timestamp; });
}

// Lists the backups on a worker; the directory size walk never runs on the UI thread.
void reload(AppState& state) {
    if (state.tasks.is_running(JobKind::BackupList)) {
        return;
    }
    if (!g_ui.loaded_once) {
        g_ui.list_state = facet::DataState::Loading;
    }
    const std::filesystem::path dir = state.backups_dir;
    state.tasks.submit<std::vector<Entry>>(
        JobKind::BackupList, "Listing backups",
        [dir](stc::app::JobContext&) { return stc::core::backup::list_backups(dir); },
        [](std::vector<Entry> entries) {
            g_ui.entries = std::move(entries);
            g_ui.list_state = facet::DataState::Ready;
            g_ui.list_error.clear();
            g_ui.loaded_once = true;
            sort_newest_first();
        },
        [](const std::string& error) {
            g_ui.list_state = facet::DataState::Error;
            g_ui.list_error = error;
            g_ui.loaded_once = true;
        });
}

void show_restore_result(AppState& state, bool ok, const std::string& when) {
    if (ok) {
        const std::string description = "Files and registry values from " + when + " are back in place.";
        facet::ShowToast("Backup restored", {.tone = facet::Tone::Success, .description = description.c_str()});
        return;
    }
    show_action_toast("Some files could not be restored. See the log.",
                      {.tone = facet::Tone::Destructive, .actionLabel = "Open log folder", .duration = 12.0F},
                      [log_dir = state.log_dir] { open_in_explorer(log_dir); });
}

void start_restore(AppState& state, const Entry& entry) {
    g_ui.busy = Busy::Restoring;
    g_ui.busy_dir = entry.dir;
    const std::string when = format_backup_timestamp(entry.timestamp);
    state.tasks.submit<bool>(
        JobKind::Restore, "Restoring a backup",
        [entry](stc::app::JobContext&) { return stc::core::backup::restore(entry); },
        [&state, when, dir = entry.dir](bool ok) {
            g_ui.busy = Busy::None;
            if (ok) {
                spdlog::info("restored backup {}", path_to_utf8(dir));
            } else {
                spdlog::warn("restore of backup {} finished with failures", path_to_utf8(dir));
            }
            show_restore_result(state, ok, when);
            reload(state);
        },
        [&state, when](const std::string&) {
            g_ui.busy = Busy::None;
            show_restore_result(state, false, when);
            reload(state);
        },
        {.must_finish = true});
}

void start_delete(AppState& state, const Entry& entry) {
    g_ui.busy = Busy::Deleting;
    g_ui.busy_dir = entry.dir;
    const std::string when = format_backup_timestamp(entry.timestamp);
    auto finish = [&state, when](bool ok) {
        g_ui.busy = Busy::None;
        if (ok) {
            const std::string text = "Deleted the backup from " + when;
            facet::ShowToast(text.c_str(), {.tone = facet::Tone::Success});
        } else {
            facet::ShowToast("The backup could not be deleted",
                             {.tone = facet::Tone::Destructive,
                              .description = "Some of its files are in use or protected. Nothing else changed."});
        }
        reload(state);
    };
    state.tasks.submit<bool>(
        JobKind::BackupDelete, "Deleting a backup",
        [entry](stc::app::JobContext&) { return stc::core::backup::remove_backup(entry); }, finish,
        [finish](const std::string&) { finish(false); }, {.must_finish = true});
}

void open_restore_dialog(const Entry& entry) {
    g_ui.pending = entry;
    g_ui.restore_open = true;
    g_ui.steam_checked_at = -1.0;
}

void open_delete_dialog(const Entry& entry) {
    g_ui.pending = entry;
    g_ui.delete_open = true;
}

// --- Capture-only ----------------------------------------------------------------------------------
// state.dev_state is set only by the dev capture mode (--state). These branches fabricate the states
// that would otherwise need a restore, a delete or a slow disk; nothing here runs a job that writes.
// Returns true when the list must not be loaded from disk for this state.
bool apply_dev_state(AppState& state) {
    const std::string& s = state.dev_state;
    if (s.empty()) {
        return false;
    }
    if (s == "loading") {
        g_ui.loaded_once = false;
        g_ui.list_state = facet::DataState::Loading;
        return true;
    }
    if (s == "error") {
        g_ui.loaded_once = true;
        g_ui.list_state = facet::DataState::Error;
        g_ui.list_error = "The backups folder could not be read.";
        return true;
    }
    if (s == "empty") {
        g_ui.loaded_once = true;
        g_ui.entries.clear();
        g_ui.list_state = facet::DataState::Ready;
        return true;
    }
    // The other states act on the newest real backup once the list is there.
    if (g_ui.dev_applied || g_ui.list_state != facet::DataState::Ready || g_ui.entries.empty()) {
        return false;
    }
    const Entry& first = g_ui.entries.front();
    g_ui.dev_applied = true;
    if (s == "confirm-restore" || s == "confirm-restore-steam") {
        open_restore_dialog(first);
    } else if (s == "confirm-delete") {
        open_delete_dialog(first);
    } else if (s == "running") {
        g_ui.busy = Busy::Restoring;
        g_ui.busy_dir = first.dir;
    } else if (s == "result-ok" || s == "result-failed") {
        show_restore_result(state, s == "result-ok", format_backup_timestamp(first.timestamp));
    } else if (s == "menu") {
        g_ui.menu_dir = first.dir;
        g_ui.menu_open = true;
    }
    return false;
}

// --- Drawing ---------------------------------------------------------------------------------------

void draw_summary(AppState& state) {
    const facet::Theme& t = facet::T();
    if (g_ui.list_state == facet::DataState::Loading && !g_ui.loaded_once) {
        facet::Skeleton({.width = 280.0F});
        return;
    }
    if (g_ui.list_state == facet::DataState::Error) {
        return;
    }
    std::uint64_t total = 0;
    for (const Entry& e : g_ui.entries) {
        total += e.size_bytes;
    }
    // With no backups the empty state says so; the line then only states the retention rule.
    std::string text;
    if (!g_ui.entries.empty()) {
        text = plural(g_ui.entries.size(), "backup", "backups") + ", " + format_bytes(total) + " in total. ";
    }
    text += state.backup_keep_count > 0 ? "Keeping the newest " + std::to_string(state.backup_keep_count) + "."
                                        : std::string("Keeping all backups.");
    constexpr const char* kLink = "Change in Settings";
    const float gap = facet::Px(t.spacing.sm);
    const float needed = facet::MeasureText(facet::TextStyle::Body, text.c_str()).x + gap +
                         facet::MeasureText(facet::TextStyle::Body, kLink).x;
    facet::StyledText(text.c_str(), {.secondary = true});
    if (needed <= ImGui::GetContentRegionAvail().x) {
        ImGui::SameLine(0.0F, gap);
    }
    if (facet::Link(kLink)) {
        state.current_page = stc::app::Page::Settings;
    }
}

// Pixel metrics of one row, from the text styles and the control height at the current scale.
// Rows sit on the page card's padding edges like the rows of the other pages' cards: the text
// starts at the card content's left edge, the "More" button ends at its right edge, and between
// two rows there is row_gap, a hairline across the content width, and row_gap again.
// `stacked` puts the contents ("19 items, 1.7 MiB") on a third line under the age instead of in its
// own column before the buttons, for a list too narrow to hold both side by side.
struct RowLayout {
    float row_gap = 0.0F;
    float line_gap = 0.0F;
    float title_h = 0.0F;
    float body_h = 0.0F;
    float control_h = 0.0F;
    float actions_w = 0.0F;
    float column_gap = 0.0F;
    bool stacked = false;
    float height = 0.0F;  // pixels; the row's content, without the gaps around the hairlines
};

RowLayout row_layout(float list_width) {
    const facet::Theme& t = facet::T();
    RowLayout l;
    l.row_gap = facet::Px(t.spacing.md);
    l.line_gap = facet::Px(t.spacing.xs) * 0.5F;
    l.title_h = facet::MeasureText(facet::TextStyle::Headline, "Ag").y;
    l.body_h = facet::MeasureText(facet::TextStyle::Body, "Ag").y;
    l.control_h = facet::ControlHeight(facet::ControlSize::Inherit);
    l.actions_w = facet::Px(kRestoreWidth) + facet::Px(t.spacing.sm) + l.control_h;
    l.column_gap = facet::Px(t.spacing.xl);

    // Side by side needs the widest date-and-pill line, the widest contents text and the buttons.
    float left = facet::MeasureText(facet::TextStyle::Headline, "00.00.0000 00:00").x + facet::Px(t.spacing.sm) +
                 facet::MeasureText(facet::TextStyle::Footnote, "Latest").x + facet::Px(t.spacing.xl);
    float contents = 0.0F;
    for (const Entry& e : g_ui.entries) {
        left = std::max(left, facet::MeasureText(facet::TextStyle::Headline, format_backup_timestamp(e.timestamp).c_str()).x);
        contents = std::max(contents, facet::MeasureText(facet::TextStyle::Body, contents_text(e).c_str()).x);
    }
    const float needed = left + l.column_gap + contents + l.column_gap + l.actions_w;
    l.stacked = needed > list_width;

    const float text_h = l.title_h + l.line_gap + l.body_h + (l.stacked ? l.line_gap + l.body_h : 0.0F);
    l.height = std::round(std::max(text_h, l.control_h));
    return l;
}

// Moves the cursor to the left edge at `y` with an empty item, so the card counts the space and the
// next item starts a fresh line there (no item spacing added).
void advance_to(float x, float y, float width) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
    ImGui::Dummy(ImVec2(width, 0.0F));
    ImGui::PopStyleVar();
}

// Between two rows: row_gap, a hairline across the whole content width, row_gap.
void draw_row_separator(float x, float width, const RowLayout& l) {
    const float y = ImGui::GetCursorScreenPos().y + l.row_gap;
    facet::HairlineH(ImGui::GetWindowDrawList(), x, x + width, y, facet::ToU32(facet::T().colors.separator));
    advance_to(x, y + facet::HairlineThickness() + l.row_gap, width);
}

void draw_menu_items(const Entry& entry, bool any_busy) {
    // Opening the folder changes nothing, so it stays available while a job runs.
    if (facet::MenuItem("Show in Explorer", {.icon = facet::Icon("folder")})) {
        open_in_explorer(entry.dir);
        g_ui.menu_open = false;
    }
    ImGui::BeginDisabled(any_busy);
    if (facet::MenuItem("Delete backup", {.icon = facet::Icon("trash"), .destructive = true})) {
        open_delete_dialog(entry);
        g_ui.menu_open = false;
    }
    ImGui::EndDisabled();
}

void draw_row(const Entry& entry, int row, bool latest, bool any_busy, const RowLayout& l) {
    const facet::Theme& t = facet::T();
    ImGui::PushID(row);
    const ImVec2 rmin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 rmax(rmin.x + width, rmin.y + l.height);
    // The row's area takes the pointer: a right click opens the same menu as "More", a double click
    // opens the backup in Explorer. It is not a Tab stop; from the keyboard the Restore and More
    // buttons are, and the Menu key or Shift+F10 on either opens the menu too. The buttons drawn
    // over it take their own clicks.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##row", ImVec2(width, l.height), ImGuiButtonFlags_MouseButtonLeft);
    ImGui::PopItemFlag();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        open_in_explorer(entry.dir);
    }
    if (facet::BeginContextMenu("row-menu")) {
        draw_menu_items(entry, any_busy);
        facet::EndContextMenu();
    }

    const float left = rmin.x;
    const float actions_x = rmax.x - l.actions_w;
    const float center_y = std::round((rmin.y + rmax.y) * 0.5F);
    const float text_h = l.title_h + l.line_gap + l.body_h + (l.stacked ? l.line_gap + l.body_h : 0.0F);
    const float title_y = std::round(center_y - text_h * 0.5F);
    const float text_room = std::max(1.0F, actions_x - l.column_gap - left);

    // Title line: the local date and time, and the "Latest" pill on the newest backup.
    const std::string when = format_backup_timestamp(entry.timestamp);
    const float pill_room = latest ? facet::MeasureText(facet::TextStyle::Footnote, "Latest").x + facet::Px(t.spacing.xl) : 0.0F;
    const float title_w = std::min(facet::MeasureText(facet::TextStyle::Headline, when.c_str()).x,
                                   std::max(1.0F, text_room - pill_room));
    ImGui::SetCursorScreenPos(ImVec2(left, title_y));
    facet::TruncatedText(when.c_str(), {.style = facet::TextStyle::Headline, .width = title_w / facet::Scale()});
    if (latest) {
        ImGui::SetCursorScreenPos(ImVec2(left + title_w + facet::Px(t.spacing.sm), title_y));
        facet::StatusPill("Latest###latest", {.size = facet::ControlSize::Small});
    }

    // Second line: the age in words. Stacked rows put the contents on a third line.
    const float age_y = title_y + l.title_h + l.line_gap;
    const std::string age = format_backup_age(entry.timestamp);
    const std::string contents = contents_text(entry);
    ImGui::SetCursorScreenPos(ImVec2(left, age_y));
    facet::TruncatedText(age.empty() ? "Date unknown" : age.c_str(),
                         {.secondary = true, .width = text_room / facet::Scale()});
    if (l.stacked) {
        ImGui::SetCursorScreenPos(ImVec2(left, age_y + l.body_h + l.line_gap));
        facet::TruncatedText(contents.c_str(), {.secondary = true, .width = text_room / facet::Scale()});
    } else {
        // Its own column, right-aligned against the buttons, on the row's center line.
        const float contents_w = facet::MeasureText(facet::TextStyle::Body, contents.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(actions_x - l.column_gap - contents_w, std::round(center_y - l.body_h * 0.5F)));
        facet::StyledText(contents.c_str(), {.secondary = true});
    }

    // Trailing actions, or the spinner while this backup is being restored or deleted.
    const float actions_y = std::round(center_y - l.control_h * 0.5F);
    const bool this_busy = g_ui.busy != Busy::None && g_ui.busy_dir == entry.dir;
    if (this_busy) {
        ImGui::SetCursorScreenPos(ImVec2(actions_x, std::round(center_y - l.body_h * 0.5F)));
        facet::Spinner(g_ui.busy == Busy::Restoring ? "Restoring###busy" : "Deleting###busy");
    } else {
        ImGui::SetCursorScreenPos(ImVec2(actions_x, actions_y));
        ImGui::BeginDisabled(any_busy);
        if (facet::Button("Restore###restore", {.icon = facet::Icon("undo"), .width = kRestoreWidth})) {
            open_restore_dialog(entry);
        }
        ImGui::EndDisabled();
        if (facet::BeginContextMenu("restore-keys")) {
            draw_menu_items(entry, any_busy);
            facet::EndContextMenu();
        }
        ImGui::SameLine(0.0F, facet::Px(t.spacing.sm));
        const bool menu_here = g_ui.menu_open && g_ui.menu_dir == entry.dir;
        if (facet::IconButton("More actions###more", facet::Icon("more-horizontal"), {.selected = menu_here})) {
            g_ui.menu_dir = entry.dir;
            g_ui.menu_open = !menu_here;
        }
        if (g_ui.menu_open && g_ui.menu_dir == entry.dir &&
            facet::BeginPopover("more-menu", &g_ui.menu_open,
                                {.placement = facet::PopoverPlacement::Below, .arrow = false,
                                 .width = facet::T().overlay.menuMinWidth})) {
            draw_menu_items(entry, any_busy);
            facet::EndPopover();
        }
    }

    advance_to(rmin.x, rmax.y, width);
    if (row + 1 < static_cast<int>(g_ui.entries.size())) {
        draw_row_separator(rmin.x, width, l);
    }
    ImGui::PopID();
}

// Three placeholder rows in the shape of the real ones, so the layout does not jump when the list
// arrives.
void draw_skeleton_rows(const RowLayout& l) {
    const float width = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < kSkeletonRows; ++i) {
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float title_y = std::round(top.y + (l.height - (l.title_h + l.line_gap + l.body_h)) * 0.5F);
        ImGui::SetCursorScreenPos(ImVec2(top.x, title_y));
        facet::Skeleton({.shape = facet::SkeletonShape::Rect, .width = 132.0F, .height = 12.0F});
        ImGui::SetCursorScreenPos(ImVec2(top.x, title_y + l.title_h + l.line_gap));
        facet::Skeleton({.shape = facet::SkeletonShape::Rect, .width = 84.0F, .height = 10.0F});
        ImGui::SetCursorScreenPos(
            ImVec2(top.x + width - l.actions_w, std::round(top.y + (l.height - l.control_h) * 0.5F)));
        facet::Skeleton({.shape = facet::SkeletonShape::Rect, .width = l.actions_w / facet::Scale()});
        advance_to(top.x, top.y + l.height, width);
        if (i + 1 < kSkeletonRows) {
            draw_row_separator(top.x, width, l);
        }
    }
}

// The restore points as rows of one page card, inside the page body, which scrolls as a whole when
// there are more than the page holds (like the other pages).
void draw_list(bool any_busy) {
    if (!begin_page_body("restore-points-body")) {
        return;
    }
    begin_page_card("restore-points");
    const RowLayout l = row_layout(ImGui::GetContentRegionAvail().x);
    if (g_ui.list_state == facet::DataState::Loading) {
        draw_skeleton_rows(l);
    } else {
        for (std::size_t i = 0; i < g_ui.entries.size(); ++i) {
            draw_row(g_ui.entries[i], static_cast<int>(i), i == 0, any_busy, l);
        }
    }
    // The card adds its bottom padding on top of the item spacing a last item leaves; the rows end
    // without it, so one empty item puts it back and the bottom padding equals the top.
    ImGui::Dummy(ImVec2(0.0F, 0.0F));
    end_page_card();
    end_page_body();
}

void draw_restore_dialog(AppState& state) {
    const facet::Theme& t = facet::T();
    if (!g_ui.restore_open) {
        return;
    }
    // Steam is checked when the dialog opens and every two seconds while it stays open.
    const double now = ImGui::GetTime();
    if (g_ui.steam_checked_at < 0.0 || now - g_ui.steam_checked_at > 2.0) {
        g_ui.steam_running = state.dev_state == "confirm-restore-steam" ||  // capture-only
                             (state.dev_state.empty() && stc::platform::proc::is_running(L"steam.exe"));
        g_ui.steam_checked_at = now;
    }
    if (!facet::BeginModal("Restore this backup?###restore-confirm", &g_ui.restore_open, {.width = 460.0F})) {
        return;
    }
    const Entry& e = g_ui.pending;
    const std::string when = format_backup_timestamp(e.timestamp);
    const std::string age = format_backup_age(e.timestamp);
    const std::string created = age.empty() ? when : when + " (" + age + ")";
    const std::string contents = contents_text(e);
    facet::BeginKeyValueList("restore-details", {.labelWidth = 96.0F});
    facet::KeyValue("Created", created.c_str());
    facet::KeyValue("Contents", contents.c_str());
    facet::EndKeyValueList();
    ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.sm)));
    facet::StyledText(
        "The files and settings saved in this backup are put back where they were. Files that are "
        "there now with the same names are replaced.",
        {.wrap = true});
    ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.sm)));
    if (g_ui.steam_running) {
        (void)facet::InlineAlert("Steam is running###steam-running", nullptr,
                                 "Close Steam before you restore. A running Steam can write over the restored files.",
                                 {.tone = facet::Tone::Warning});
    } else {
        facet::StyledText("Keep Steam closed until the restore has finished.", {.secondary = true, .wrap = true});
    }
    ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.lg)));

    bool confirmed = false;
    facet::BeginStack("restore-buttons", {.orientation = facet::Orientation::Horizontal, .width = facet::kFill});
    facet::StackSpacer();
    if (facet::Button("Cancel")) {
        g_ui.restore_open = false;
    }
    if (facet::Button("Restore", {.variant = facet::ButtonVariant::Primary})) {
        confirmed = true;
    }
    facet::EndStack();
    facet::EndModal();

    if (confirmed) {
        g_ui.restore_open = false;
        if (state.dev_state.empty()) {  // a capture never restores
            start_restore(state, g_ui.pending);
        }
    }
}

void draw_delete_dialog(AppState& state) {
    if (!g_ui.delete_open) {
        return;
    }
    const Entry& e = g_ui.pending;
    const std::string message = "The backup from " + format_backup_timestamp(e.timestamp) + " (" + contents_text(e) +
                                ") is deleted from disk. You cannot restore it afterwards.";
    const facet::DialogResult result =
        facet::ConfirmDialog("Delete this backup?###delete-confirm", &g_ui.delete_open, message.c_str(),
                             {.confirmLabel = "Delete backup", .destructive = true});
    if (result == facet::DialogResult::Confirmed && state.dev_state.empty()) {  // a capture never deletes
        start_delete(state, g_ui.pending);
    }
}

}  // namespace

void draw_backups_page(AppState& state) {
    const facet::Theme& t = facet::T();

    // A page that comes into view lists the backups again; so do restores, deletes and Refresh.
    const int frame = ImGui::GetFrameCount();
    const bool opened = frame - g_ui.last_drawn_frame != 1;
    g_ui.last_drawn_frame = frame;
    if (opened) {
        g_ui.dev_applied = false;  // capture-only: each capture pass shows its state again
        g_ui.menu_open = false;
    }
    const bool dev_fixed = apply_dev_state(state);
    if (opened && !dev_fixed) {
        reload(state);
    }

    const bool listing = state.tasks.is_running(JobKind::BackupList);
    const bool any_busy = g_ui.busy != Busy::None || state.tasks.is_running(JobKind::Restore) ||
                          state.tasks.is_running(JobKind::BackupDelete) || state.tasks.is_running(JobKind::Clean);

    begin_page_header("Backups",
                      "Every cleanup with \"Back up first\" on saves what it removes. Restore a backup to "
                      "put those files and settings back.");
    if (facet::Button("Open folder", {.icon = facet::Icon("folder")})) {
        if (!open_in_explorer(state.backups_dir)) {
            facet::ShowToast("No backups folder yet",
                             {.tone = facet::Tone::Warning,
                              .description = "It is created by the first cleanup that backs up first."});
        }
    }
    ImGui::SameLine(0.0F, facet::Px(t.spacing.sm));
    if (facet::IconButton("Refresh the list###refresh", facet::Icon("refresh"), {.loading = listing}) && !dev_fixed) {
        reload(state);
    }
    end_page_header();

    draw_summary(state);
    page_block_gap();

    const float rest = ImGui::GetContentRegionAvail().y / facet::Scale();
    if (g_ui.list_state == facet::DataState::Error) {
        const std::string message = g_ui.list_error + " Check that the folder can be read, then try again.";
        if (facet::EmptyState("The backups could not be listed###backups-error",
                              {.message = message.c_str(),
                               .icon = facet::Icon("error"),
                               .actionLabel = "Try again",
                               .tone = facet::Tone::Destructive,
                               .height = rest}) &&
            !dev_fixed) {
            reload(state);
        }
    } else if (g_ui.list_state == facet::DataState::Ready && g_ui.entries.empty()) {
        if (facet::EmptyState("No backups yet###backups-empty",
                              {.message = "A backup is made each time a cleanup runs with \"Back up first\" on. "
                                          "Restore points then show up here.",
                               .icon = facet::Icon("undo"),
                               .actionLabel = "Go to Cleanup",
                               .height = rest})) {
            state.current_page = stc::app::Page::Cleanup;
        }
    } else {
        draw_list(any_busy);
    }

    draw_restore_dialog(state);
    draw_delete_dialog(state);
}

}  // namespace stc::ui::pages
