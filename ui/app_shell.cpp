#include "ui/app_shell.hpp"

#include <imgui.h>

#include <facet/facet.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/app_state.hpp"
#include "core/version.hpp"
#include "ui/appearance.hpp"
#include "ui/assets.hpp"
#include "ui/common.hpp"
#include "ui/pages/audit_page.hpp"
#include "ui/pages/backups_page.hpp"
#include "ui/pages/cleanup_page.hpp"
#include "ui/pages/configs_page.hpp"
#include "ui/pages/exclusions_page.hpp"
#include "ui/pages/settings_page.hpp"

namespace stc::ui {
namespace {

using stc::app::AppState;
using stc::app::Page;

constexpr const char* kAppTitle = "Steam Tracer Cleaner";
constexpr ImVec2 kMinWindowUnits{760.0F, 520.0F};
// The sidebar shows labels from this frame width up (units) and the icon rail below it. Facet's
// Auto mode switches at its Expanded width class (720), which at the 760 minimum would leave
// pages about 470 units; the rail below 900 keeps every page at 540 units or more.
constexpr float kExpandedSidebarFrom = 900.0F;
constexpr const char* kPaletteShortcutName = "stc.command-palette";

// One icon per page from Facet's built-in set, chosen for what the page is:
//   Cleanup     trash     the page deletes account residue
//   Backups     undo      a restore point undoes a cleanup
//   Exclusions  lock      excluded paths and accounts are locked against cleaning
//   Audit       eye       a read-only look at what is on the machine
//   Configs     file      the page manages config files
//   Settings    settings  the standard gear for settings
// The sidebar is one flat list in this order; the last entry (Settings) is pinned to the bottom.
struct PageEntry {
    Page page;
    const char* label;
    const char* icon;
};
constexpr std::array<PageEntry, stc::app::kPageCount> kPages{{
    {Page::Cleanup, "Cleanup", "trash"},
    {Page::Backups, "Backups", "undo"},
    {Page::Exclusions, "Exclusions", "lock"},
    {Page::Audit, "Audit", "eye"},
    {Page::Configs, "Configs", "file"},
    {Page::Settings, "Settings", "settings"},
}};

// The title bar activity indicator. A job shows only after it has run this long (seconds), so the
// quick jobs (listing backups, measuring the plan) never flash in the bar; the indicator has one
// fixed width (units), so a changing label or count never moves the items next to it.
constexpr double kActivityDelay = 0.4;
constexpr float kActivityWidth = 210.0F;

struct ShellState {
    bool palette_open = false;
    bool update_dialog_open = false;
    bool update_toast_shown = false;
    facet::ToastId update_toast;
    bool close_toast_shown = false;
    bool toast_corner_set = false;
    // The job the indicator follows and when the shell first saw it running (ImGui time).
    stc::app::JobId activity_job = 0;
    double activity_since = 0.0;
};

ShellState g_shell;
ShellDebug g_debug;

ShellRect last_item_rect() { return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}; }

bool update_available(const AppState& state) {
    return state.update_result && state.update_result->newer_than_current &&
           state.update_result->latest_tag != state.version_check_skip_until;
}

void draw_page(AppState& state) {
    switch (state.current_page) {
        case Page::Cleanup: pages::draw_cleanup_page(state); break;
        case Page::Backups: pages::draw_backups_page(state); break;
        case Page::Exclusions: pages::draw_exclusions_page(state); break;
        case Page::Audit: pages::draw_audit_page(state); break;
        case Page::Configs: pages::draw_configs_page(state); break;
        case Page::Settings: pages::draw_settings_page(state); break;
    }
}

// --- Title bar -------------------------------------------------------------------------------------

// The background job the title bar shows (see current_activity); an empty label shows nothing.
struct Activity {
    std::string label;
    std::string tooltip;
};
Activity current_activity(const AppState& state);
void draw_activity(const Activity& activity);

void draw_title_bar(AppState& state, HostWindow& host) {
    facet::TitleBarOptions options;
    options.icon = assets::app_icon_slot();
    options.windowButtons = true;
    options.maximized = host.maximized();
    facet::BeginTitleBar(kAppTitle, options);
    const Activity activity = current_activity(state);
    g_debug.activity = {};
    g_debug.activity_label.clear();
    if (!activity.label.empty()) {
        draw_activity(activity);
        ImGui::SameLine(0.0F, facet::Px(facet::T().spacing.md));
    }
    if (update_available(state)) {
        char label[96];
        std::snprintf(label, sizeof(label), "%s available###update-indicator", state.update_result->latest_tag.c_str());
        if (facet::Button(label, {.variant = facet::ButtonVariant::Plain, .icon = facet::Icon("download")})) {
            g_shell.update_dialog_open = true;
        }
        ImGui::SameLine();
    }
    if (facet::IconButton("Command palette (Ctrl+K)###palette-button", facet::Icon("search"))) {
        g_shell.palette_open = true;
    }
    g_debug.palette_button = last_item_rect();
    facet::EndTitleBar();
    const ImVec2 bar_min = ImGui::GetItemRectMin();
    const ImVec2 bar_max = ImGui::GetItemRectMax();

    // The full-color app icon over the place the empty slot icon reserved (spacing.md from the
    // leading edge, the Headline icon size, centered on the bar).
    facet::PushTextStyle(facet::TextStyle::Headline);
    const float icon = std::round(ImGui::GetFontSize());
    facet::PopTextStyle();
    const float x = bar_min.x + facet::Px(facet::T().spacing.md);
    const float y = std::round((bar_min.y + bar_max.y - icon) * 0.5F);
    assets::draw_app_icon(ImGui::GetWindowDrawList(), ImVec2(x, y), icon);

    switch (facet::TitleBarButtonPressed()) {
        case facet::WindowButton::Minimize: host.minimize(); break;
        case facet::WindowButton::Maximize: host.toggle_maximize(); break;
        case facet::WindowButton::Close: host.request_close(); break;
        case facet::WindowButton::None: break;
    }
    // A popup or another window over the bar takes the click there, not the drag region.
    const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool covered = ImGui::IsMouseHoveringRect(bar_min, bar_max, false) &&
                         !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    host.set_caption_blocked(popup_open || covered || g_shell.palette_open || g_shell.update_dialog_open);

    facet::HairlineH(ImGui::GetWindowDrawList(), bar_min.x, bar_max.x, bar_max.y - facet::HairlineThickness(),
                     facet::ToU32(facet::T().colors.separator));
}

// --- Sidebar ---------------------------------------------------------------------------------------

void sidebar_item(AppState& state, const PageEntry& entry) {
    if (facet::SidebarItem(entry.label, facet::Icon(entry.icon), {.selected = state.current_page == entry.page})) {
        state.current_page = entry.page;
    }
    g_debug.sidebar_items[static_cast<int>(entry.page)] = last_item_rect();
}

void draw_sidebar(AppState& state, float height_px) {
    const facet::Theme& t = facet::T();
    const bool expanded = facet::AvailableWidthUnits() >= kExpandedSidebarFrom;
    g_debug.sidebar_rail = !expanded;
    facet::SidebarOptions options;
    options.mode = expanded ? facet::SidebarMode::Expanded : facet::SidebarMode::Rail;
    options.height = std::max(t.list.rowHeight, height_px / facet::Scale());
    ImGui::BeginGroup();
    if (facet::BeginSidebar("##nav", options)) {
        for (std::size_t i = 0; i + 1 < kPages.size(); ++i) {
            sidebar_item(state, kPages[i]);
        }
        // Settings is pinned to the bottom of the column.
        const float row = std::round(facet::Px(t.list.rowHeight));
        const float room = ImGui::GetContentRegionAvail().y - row;
        if (room > 0.0F) {
            ImGui::Dummy(ImVec2(1.0F, room));
        }
        sidebar_item(state, kPages.back());
        facet::EndSidebar();
    }
    ImGui::EndGroup();
}

// --- Background activity ---------------------------------------------------------------------------

// "Cleaning 92 of 165" for a job that reports a total, the bare label otherwise.
std::string job_text(const stc::app::JobInfo& job) {
    std::string text = job.label;
    if (job.total > 0) {
        char progress[64];
        std::snprintf(progress, sizeof(progress), " %llu of %llu", static_cast<unsigned long long>(job.done),
                      static_cast<unsigned long long>(job.total));
        text += progress;
    }
    return text;
}

// What the indicator shows this frame: the job to follow (a must-finish job before the others)
// and every running job for the tooltip. Empty while idle or while the job is younger than
// kActivityDelay.
Activity current_activity(const AppState& state) {
    // Capture-only: the "shell-job" dev state shows a fabricated cleanup in progress, so a capture
    // can show the indicator without running a job. No job is started.
    if (state.dev_state == "shell-job") {
        return {"Cleaning 92 of 165", "Cleaning 92 of 165. The window stays open until it has finished."};
    }
    const std::vector<stc::app::JobInfo> jobs = state.tasks.running();
    if (jobs.empty()) {
        g_shell.activity_job = 0;
        return {};
    }
    const auto must = std::ranges::find_if(jobs, [](const stc::app::JobInfo& j) { return j.must_finish; });
    const stc::app::JobInfo& job = must != jobs.end() ? *must : jobs.front();
    const double now = ImGui::GetTime();
    // The delay runs once per busy stretch: a second job starting while one is shown is shown at once.
    if (g_shell.activity_job == 0) {
        g_shell.activity_since = now;
    }
    g_shell.activity_job = job.id;
    if (now - g_shell.activity_since < kActivityDelay) {
        return {};
    }
    Activity activity;
    activity.label = job_text(job);
    for (const stc::app::JobInfo& j : jobs) {
        if (!activity.tooltip.empty()) {
            activity.tooltip += "\n";
        }
        activity.tooltip += job_text(j);
        if (j.must_finish) {
            activity.tooltip += ". The window stays open until it has finished.";
        }
    }
    if (jobs.size() > 1) {
        char more[32];
        std::snprintf(more, sizeof(more), ", %zu more", jobs.size() - 1);
        activity.label += more;
    }
    return activity;
}

// A spinner and the job's label in one fixed-width group, centered on the bar's control line.
void draw_activity(const Activity& activity) {
    const facet::Theme& t = facet::T();
    const facet::TextStyle style = facet::ControlTextStyle(facet::ControlSize::Small);
    const float control_h = facet::ControlHeight(facet::ControlSize::Inherit);
    const float text_h = facet::MeasureText(style, "Ag").y;
    const float gap = facet::Px(t.spacing.xs);
    // The group always spans the fixed width (it starts at the cursor); spinner and label sit at its
    // trailing end, next to the items they come before.
    ImGui::BeginGroup();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float spinner_w = text_h;  // the Small spinner's diameter is the Small text height
    const float text_max = std::max(1.0F, facet::Px(kActivityWidth) - spinner_w - gap);
    const float label_w = facet::MeasureText(style, activity.label.c_str()).x;
    const float text_w = std::min(label_w, text_max);
    const float lead = std::max(0.0F, facet::Px(kActivityWidth) - spinner_w - gap - text_w);
    ImGui::SetCursorScreenPos(ImVec2(std::round(at.x + lead), std::round(at.y + (control_h - text_h) * 0.5F)));
    facet::Spinner("##activity-spinner", {.size = facet::ControlSize::Small});
    ImGui::SameLine(0.0F, gap);
    // A cut label shows its whole text as TruncatedText's own tooltip; an uncut one gets the
    // longer tooltip (every running job) here, so two tooltips never stack.
    const bool fits = label_w <= text_max;
    facet::TruncatedText(activity.label.c_str(),
                         {.style = style, .secondary = true, .width = std::ceil(text_w) / facet::Scale()});
    ImGui::EndGroup();
    g_debug.activity = last_item_rect();
    g_debug.activity_label = activity.label;
    if (fits && ImGui::IsItemHovered()) {
        facet::ItemTooltip("%s", activity.tooltip.c_str());
    }
}

// --- Command palette -------------------------------------------------------------------------------

enum class Command {
    GoCleanup,
    GoBackups,
    GoExclusions,
    GoAudit,
    GoConfigs,
    GoSettings,
    RedetectSteam,
    OpenBackups,
    OpenLogs,
    OpenRepository,
    ThemeSystem,
    ThemeLight,
    ThemeDark,
};

void set_theme(AppState& state, stc::app::ThemeChoice choice) {
    state.theme_mode = choice;
    state.save_app_settings();
}

void run_command(AppState& state, Command command) {
    switch (command) {
        case Command::GoCleanup: state.current_page = Page::Cleanup; break;
        case Command::GoBackups: state.current_page = Page::Backups; break;
        case Command::GoExclusions: state.current_page = Page::Exclusions; break;
        case Command::GoAudit: state.current_page = Page::Audit; break;
        case Command::GoConfigs: state.current_page = Page::Configs; break;
        case Command::GoSettings: state.current_page = Page::Settings; break;
        case Command::RedetectSteam:
            state.refresh_steam();
            facet::ShowToast(state.install ? "Steam found" : "Steam was not found",
                             {.tone = state.install ? facet::Tone::Success : facet::Tone::Warning});
            break;
        case Command::OpenBackups:
            if (!open_in_explorer(state.backups_dir)) {
                facet::ShowToast("The backups folder does not exist yet",
                                 {.tone = facet::Tone::Warning, .description = "It is created by the first cleanup with a backup."});
            }
            break;
        case Command::OpenLogs:
            if (!open_in_explorer(state.log_dir)) {
                facet::ShowToast("The log folder does not exist", {.tone = facet::Tone::Warning});
            }
            break;
        case Command::OpenRepository: open_url(kRepoUrl); break;
        case Command::ThemeSystem: set_theme(state, stc::app::ThemeChoice::System); break;
        case Command::ThemeLight: set_theme(state, stc::app::ThemeChoice::Light); break;
        case Command::ThemeDark: set_theme(state, stc::app::ThemeChoice::Dark); break;
    }
}

void draw_command_palette(AppState& state) {
    const facet::ShortcutId open_shortcut = facet::FindShortcut(kPaletteShortcutName);
    const char* current = "Current";
    const auto theme_detail = [&](stc::app::ThemeChoice c) { return state.theme_mode == c ? current : nullptr; };
    const facet::ChoiceItem commands[] = {
        {.label = "Go to Cleanup", .icon = facet::Icon("trash"), .group = "Go to"},
        {.label = "Go to Backups", .icon = facet::Icon("undo"), .group = "Go to"},
        {.label = "Go to Exclusions", .icon = facet::Icon("lock"), .group = "Go to"},
        {.label = "Go to Audit", .icon = facet::Icon("eye"), .group = "Go to"},
        {.label = "Go to Configs", .icon = facet::Icon("file"), .group = "Go to"},
        {.label = "Go to Settings", .icon = facet::Icon("settings"), .group = "Go to"},
        {.label = "Re-detect Steam", .icon = facet::Icon("refresh"), .group = "Steam"},
        {.label = "Open the backups folder", .icon = facet::Icon("folder"), .group = "Folders"},
        {.label = "Open the log folder", .icon = facet::Icon("folder"), .group = "Folders"},
        {.label = "Open the repository on GitHub", .icon = assets::github(), .group = "About"},
        {.label = "Use the system theme", .icon = facet::Icon("monitor"), .detail = theme_detail(stc::app::ThemeChoice::System), .group = "Appearance"},
        {.label = "Use the light theme", .icon = facet::Icon("sun"), .detail = theme_detail(stc::app::ThemeChoice::Light), .group = "Appearance"},
        {.label = "Use the dark theme", .icon = facet::Icon("moon"), .detail = theme_detail(stc::app::ThemeChoice::Dark), .group = "Appearance"},
    };
    int picked = -1;
    facet::CommandPaletteOptions options;
    options.placeholder = "Type a command";
    options.openShortcut = open_shortcut;
    if (facet::CommandPalette("##palette", &g_shell.palette_open, &picked, commands, options) && picked >= 0) {
        run_command(state, static_cast<Command>(picked));
    }
    g_debug.palette_open = g_shell.palette_open;
}

// --- Update flow -----------------------------------------------------------------------------------

void draw_update_flow(AppState& state) {
    if (!update_available(state)) {
        return;
    }
    const std::string& tag = state.update_result->latest_tag;
    if (!g_shell.update_toast_shown && !state.update_modal_dismissed_this_session) {
        const std::string text = "Steam Tracer Cleaner " + tag + " is available";
        const std::string description = std::string("You have v") + stc::core::kAppVersion + ".";
        g_shell.update_toast = facet::ShowToast(text.c_str(), {.tone = facet::Tone::Accent,
                                                               .icon = facet::Icon("download"),
                                                               .description = description.c_str(),
                                                               .actionLabel = "View update"});
        g_shell.update_toast_shown = true;
    }
    if (g_shell.update_toast.IsValid() && facet::ToastActionPressed(g_shell.update_toast)) {
        g_shell.update_dialog_open = true;
    }

    const bool was_open = g_shell.update_dialog_open;
    if (facet::BeginModal("Update available###update-dialog", &g_shell.update_dialog_open)) {
        char text[160];
        std::snprintf(text, sizeof(text), "Steam Tracer Cleaner %s is available. You have v%s.", tag.c_str(),
                      stc::core::kAppVersion);
        facet::StyledText(text, {.wrap = true});
        facet::StyledText("The releases page lists the changes and the download.",
                          {.secondary = true, .wrap = true});
        ImGui::Dummy(ImVec2(0.0F, facet::Px(facet::T().spacing.md)));
        bool done = false;
        if (facet::Button("Open releases page", {.variant = facet::ButtonVariant::Primary})) {
            open_url(kReleasesUrl);
            done = true;
        }
        ImGui::SameLine();
        if (facet::Button("Skip this version")) {
            state.version_check_skip_until = tag;
            state.save_app_settings();
            done = true;
        }
        ImGui::SameLine();
        if (facet::Button("Later", {.variant = facet::ButtonVariant::Plain})) {
            done = true;
        }
        if (done) {
            g_shell.update_dialog_open = false;
        }
        facet::EndModal();
    }
    // Any way of closing the dialog (a button, Escape, the close button) ends the prompt for this
    // session; Skip also hides the indicator for this version.
    if (was_open && !g_shell.update_dialog_open) {
        state.update_modal_dismissed_this_session = true;
        if (g_shell.update_toast.IsValid()) {
            facet::DismissToast(g_shell.update_toast);
        }
    }
}

void draw_close_notice(AppState& state) {
    if (!state.close_pending || g_shell.close_toast_shown) {
        return;
    }
    std::string waiting = "A running job";
    for (const auto& job : state.tasks.running()) {
        if (job.must_finish) {
            waiting = job.label;
            break;
        }
    }
    const std::string description = waiting + " must finish first. The window closes by itself afterwards.";
    facet::ShowToast("Closing when the work is done", {.tone = facet::Tone::Warning,
                                                       .description = description.c_str(),
                                                       .duration = -1.0F});
    g_shell.close_toast_shown = true;
}

}  // namespace

void register_shell_shortcuts() {
    if (facet::FindShortcut(kPaletteShortcutName).IsValid()) {
        return;
    }
    facet::ShortcutOptions options;
    options.label = "Open the command palette";
    options.group = "General";
    facet::RegisterShortcut(kPaletteShortcutName, ImGuiMod_Ctrl | ImGuiKey_K, options);
}

const ShellDebug& shell_debug() { return g_debug; }

bool draw_app(AppState& state, HostWindow& host) {
    state.tasks.poll();
    poll_toast_actions();
    sync_appearance(state);
    register_shell_shortcuts();
    set_dialog_owner(host.native_handle());
    if (!g_shell.toast_corner_set) {
        // Bottom right, the Windows notification corner, clear of the title bar buttons.
        facet::SetToastCorner(facet::ScreenCorner::BottomRight);
        g_shell.toast_corner_set = true;
    }

    facet::AppFrameOptions frame;
    frame.hostMaximized = host.maximized();
    frame.minSize = kMinWindowUnits;
    if (!facet::BeginAppFrame("##app", frame)) {
        return false;
    }
    draw_title_bar(state, host);

    const facet::Theme& t = facet::T();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // No status bar: the sidebar and the page run to the bottom edge, with the same inset there
    // as at the top.
    const float body_h = std::max(1.0F, avail.y);
    const float inset = facet::Px(t.spacing.sm);

    ImGui::SetCursorScreenPos(ImVec2(origin.x + inset, origin.y + inset));
    draw_sidebar(state, body_h - inset * 2.0F);
    const float nav_right = ImGui::GetItemRectMax().x;
    g_debug.sidebar_bottom = ImGui::GetItemRectMax().y;
    g_debug.window_bottom = origin.y + avail.y;

    ImGui::SetCursorScreenPos(ImVec2(nav_right, origin.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, facet::Px(ImVec2(t.spacing.xxl, t.spacing.xl)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
    const bool page_visible =
        ImGui::BeginChild("##page", ImVec2(origin.x + avail.x - nav_right, body_h), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // Popped at once so the page's own child windows start from the theme's padding.
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (page_visible) {
        ImGui::PushID(static_cast<int>(state.current_page));
        draw_page(state);
        ImGui::PopID();
    }
    ImGui::EndChild();
    facet::EndAppFrame();

    draw_command_palette(state);
    draw_update_flow(state);
    draw_close_notice(state);
    return true;
}

}  // namespace stc::ui
