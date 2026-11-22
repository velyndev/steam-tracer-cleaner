// The summary card of the Cleanup page: what will happen, in one glance, and the one primary
// action. Every plan and run state is drawn here (see draw_summary_card).

#include <imgui.h>
#include <imgui_internal.h>

#include <facet/facet.h>

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "app/app_state.hpp"
#include "core/dry_run.hpp"
#include "ui/common.hpp"
#include "ui/pages/cleanup_state.hpp"
#include "ui/pages/cleanup_view.hpp"

namespace stc::ui::pages::cleanup {
namespace {

using stc::app::AppState;
using stc::app::JobKind;

// The action buttons keep one width while their labels change ("Clean 9 items", "Clean 1204 items").
constexpr float kPrimaryUnits = 176.0F;
constexpr float kSecondaryUnits = 128.0F;
// Below this much room for the text (units), the buttons move under it.
constexpr float kMinTextUnits = 240.0F;

struct Action {
    const char* label = nullptr;
    facet::ButtonVariant variant = facet::ButtonVariant::Secondary;
    bool enabled = true;
    float width = kSecondaryUnits;
};

// The upper part of the card: a title (the plan size in LargeTitle, or a state in Title2), one or
// two plain lines under it, and the actions at the trailing edge (under the text when narrow).
// `extra` draws more content under the message (a skeleton, a disclosure). Returns the index of
// the pressed action or -1.
template <typename Extra>
int draw_hero(const char* title, facet::TextStyle title_style, facet::Tone title_tone, const char* message,
              std::span<const Action> actions, Extra&& extra) {
    const facet::Theme& t = facet::T();
    const float width = ImGui::GetContentRegionAvail().x;
    const float gap_px = facet::Px(t.spacing.sm);
    float actions_w = 0.0F;
    for (const auto& a : actions) {
        actions_w += facet::Px(a.width);
    }
    actions_w += gap_px * static_cast<float>(actions.empty() ? 0 : actions.size() - 1);
    const bool stacked = !actions.empty() && units(width - actions_w - facet::Px(t.spacing.xl)) < kMinTextUnits;
    const float text_w = stacked || actions.empty() ? width : width - actions_w - facet::Px(t.spacing.xl);

    const ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();
    if (title != nullptr) {
        facet::StyledText(title, {.style = title_style, .tone = title_tone, .wrap = true, .width = units(text_w)});
    }
    if (message != nullptr && message[0] != '\0') {
        gap(t.spacing.xs);
        facet::StyledText(message, {.secondary = true, .wrap = true, .width = units(text_w)});
    }
    extra(text_w);
    ImGui::EndGroup();
    const float text_h = ImGui::GetItemRectSize().y;

    int pressed = -1;
    if (actions.empty()) {
        return pressed;
    }
    const float button_h = facet::ControlHeight(facet::ControlSize::Large);
    float x = stacked ? top.x : top.x + width - actions_w;
    const float y = stacked ? top.y + text_h + facet::Px(t.spacing.md)
                            : top.y + std::max(0.0F, (std::min(text_h, facet::Px(64.0F)) - button_h) * 0.5F);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const Action& a = actions[i];
        reset_line_baseline();
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::BeginDisabled(!a.enabled);
        if (facet::Button(a.label, {.variant = a.variant, .size = facet::ControlSize::Large, .width = a.width}) &&
            a.enabled) {
            pressed = static_cast<int>(i);
        }
        ImGui::EndDisabled();
        x += facet::Px(a.width) + gap_px;
    }
    // Continue under whichever is lower: the text or the buttons.
    const float bottom = std::max(top.y + text_h, y + button_h);
    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
    ImGui::Dummy(ImVec2(0.0F, 0.0F));
    return pressed;
}

int draw_hero(const char* title, facet::TextStyle title_style, facet::Tone title_tone, const char* message,
              std::span<const Action> actions) {
    return draw_hero(title, title_style, title_tone, message, actions, [](float) {});
}

// Why the Clean button is off while another job runs, or null.
const char* blocking_job(const AppState& state) {
    if (state.tasks.is_running(JobKind::Restore)) {
        return "Wait for the restore to finish first.";
    }
    if (state.tasks.is_running(JobKind::BackupDelete)) {
        return "Wait for the backup to be deleted first.";
    }
    if (state.tasks.is_running(JobKind::ConfigInstall)) {
        return "Wait for the config install to finish first.";
    }
    return nullptr;
}

// The lower row: where the backup goes (or that there is none) and the "Back up first" switch at
// the trailing edge.
void draw_backup_row(AppState& state, PageState& ps) {
    const facet::Theme& t = facet::T();
    gap(t.spacing.md);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    facet::HairlineH(ImGui::GetWindowDrawList(), start.x, start.x + width, start.y, facet::ToU32(t.colors.separator));
    gap(t.spacing.md);

    const char* toggle_label = "Back up first";
    const float toggle_w = facet::MeasureText(facet::TextStyle::Body, toggle_label).x + facet::Px(t.spacing.sm) +
                           facet::Px(t.metrics.toggleWidth);
    const float row_h = facet::ControlHeight(facet::ControlSize::Inherit);
    const ImVec2 row = ImGui::GetCursorScreenPos();

    std::string note;
    if (ps.backup_first) {
        note = "A backup is saved in Backups first, so you can restore everything";
        note += state.backup_keep_count > 0 ? ". The newest " + std::to_string(state.backup_keep_count) + " are kept."
                                            : ".";
    } else {
        note = "No backup: what is removed cannot be restored.";
    }
    const float note_w = std::max(1.0F, width - toggle_w - facet::Px(t.spacing.lg));
    const float foot_h = facet::MeasureText(facet::TextStyle::Footnote, "Ag").y;
    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + (row_h - foot_h) * 0.5F));
    facet::TruncatedText(note.c_str(), {.style = facet::TextStyle::Footnote, .secondary = true, .width = units(note_w)});
    if (ps.backup_first) {
        const std::string where = "Backups are saved in " + stc::ui::path_to_utf8(state.backups_dir);
        ImGui::SetItemTooltip("%s", where.c_str());
    }

    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(row.x + width - toggle_w, row.y));
    facet::Toggle(toggle_label, &ps.backup_first);
}

std::string plain_line(const stc::core::Plan& plan) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s in %llu %s will be removed", items_text(plan.steps.size()).c_str(),
                  static_cast<unsigned long long>(plan.total_file_count), plan.total_file_count == 1 ? "file" : "files");
    return text;
}

void draw_running(const AppState& state, const PageState& ps) {
    const facet::Theme& t = facet::T();
    const RunProgress progress = run_progress(state, ps);
    char title[96];
    float fraction = facet::kIndeterminate;
    if (progress.phase == RunPhase::Cleaning && progress.total > 0) {
        std::snprintf(title, sizeof(title), "Cleaning %llu of %llu", static_cast<unsigned long long>(progress.done),
                      static_cast<unsigned long long>(progress.total));
        fraction = static_cast<float>(static_cast<double>(progress.done) / static_cast<double>(progress.total));
    } else if (progress.phase == RunPhase::Backup) {
        std::snprintf(title, sizeof(title), "Making the backup");
    } else {
        std::snprintf(title, sizeof(title), "Closing Steam");
    }
    const float width = ImGui::GetContentRegionAvail().x;
    facet::StyledText(title, {.style = facet::TextStyle::Title2});
    gap(t.spacing.md);
    facet::ProgressBar("##run-progress", fraction, {.size = facet::ControlSize::Large, .width = units(width)});
    gap(t.spacing.sm);
    facet::StyledText("Keep the app open until the cleanup finishes. Steam stays closed until you start it.",
                      {.style = facet::TextStyle::Footnote, .secondary = true, .wrap = true, .width = units(width)});
}

// The result of the last cleanup. Returns true while it is shown (until "Done").
bool draw_result(AppState& state, PageState& ps) {
    if (!ps.result_open) {
        ps.run_error.clear();
        return false;
    }
    const facet::Theme& t = facet::T();
    if (!ps.run_error.empty()) {
        const Action actions[] = {{.label = "Done", .variant = facet::ButtonVariant::Primary, .width = kSecondaryUnits}};
        if (draw_hero("The cleanup did not finish", facet::TextStyle::Title2, facet::Tone::Destructive,
                      ps.run_error.c_str(), actions) == 0) {
            ps.result_open = false;
            ps.run_error.clear();
        }
        return true;
    }
    if (!state.last_result) {
        ps.result_open = false;
        return false;
    }
    const auto& r = *state.last_result;
    const std::uint64_t total = static_cast<std::uint64_t>(r.succeeded) + static_cast<std::uint64_t>(r.failed);
    std::vector<Action> actions;
    if (ps.last_backup_made) {
        actions.push_back({.label = "Open Backups", .width = kSecondaryUnits});
    }
    actions.push_back({.label = "Done", .variant = facet::ButtonVariant::Primary, .width = kSecondaryUnits});
    const int done_index = static_cast<int>(actions.size()) - 1;

    int pressed = -1;
    if (r.failed == 0) {
        const std::string title = "Freed " + stc::ui::format_bytes(r.bytes_freed);
        const std::string message =
            "Removed " + items_text(static_cast<std::uint64_t>(r.succeeded)) + ". " +
            (ps.last_backup_made ? "A backup of everything removed is in Backups."
                                 : "No backup was made for this cleanup.");
        pressed = draw_hero(title.c_str(), facet::TextStyle::LargeTitle, facet::Tone::Success, message.c_str(), actions);
    } else {
        const std::string title = std::to_string(r.failed) + " of " + items_text(total) + " could not be removed";
        const std::string message = std::to_string(r.succeeded) + " removed, " + stc::ui::format_bytes(r.bytes_freed) +
                                    " freed. Close any program that uses them and clean again.";
        pressed = draw_hero(title.c_str(), facet::TextStyle::Title2, facet::Tone::Warning, message.c_str(), actions,
                            [&](float text_w) {
                                gap(t.spacing.sm);
                                char summary[32];
                                std::snprintf(summary, sizeof(summary), "%d", r.failed);
                                if (facet::BeginDisclosure("Failure messages", &ps.failures_open, {.summary = summary})) {
                                    std::vector<std::string> lines;
                                    lines.reserve(r.failure_messages.size());
                                    std::string all;
                                    for (const auto& m : r.failure_messages) {
                                        lines.push_back(stc::ui::to_utf8(m));
                                        all += lines.back();
                                        all += '\n';
                                    }
                                    std::vector<facet::LogEntry> entries;
                                    entries.reserve(lines.size());
                                    for (const auto& line : lines) {
                                        entries.push_back({.text = line, .level = facet::LogLevel::Error});
                                    }
                                    const float log_h = std::min(120.0F, 24.0F * static_cast<float>(entries.size()) + 12.0F);
                                    const float log_w = std::min(text_w, ImGui::GetContentRegionAvail().x);
                                    facet::LogView("##failures", entries, {.autoScroll = false,
                                                                           .timestamps = false,
                                                                           .width = units(log_w),
                                                                           .height = log_h});
                                    gap(t.spacing.xs);
                                    facet::CopyButton("Copy messages", all.c_str(), {.iconOnly = false});
                                    facet::EndDisclosure();
                                }
                            });
    }
    if (pressed == done_index) {
        ps.result_open = false;
        ps.failures_open = false;
    } else if (pressed == 0 && ps.last_backup_made) {
        state.current_page = stc::app::Page::Backups;
    }
    return true;
}

}  // namespace

void draw_summary_card(AppState& state, PageState& ps, PlanStatus status, float width_px) {
    const facet::Theme& t = facet::T();
    (void)width_px;  // the shared page card fills the column it is drawn in
    stc::ui::begin_page_card("summary-card");

    if (run_active(state, ps)) {
        draw_running(state, ps);
        stc::ui::end_page_card();
        return;
    }
    if (draw_result(state, ps)) {
        stc::ui::end_page_card();
        return;
    }

    const char* blocked = blocking_job(state);
    const bool has_plan = state.last_plan.has_value() && !ps.dev.plan_loading;
    const bool empty_plan = has_plan && (state.last_plan->steps.empty() || ps.dev.nothing);
    const std::uint64_t steps = has_plan && !ps.dev.nothing ? state.last_plan->steps.size() : 0;
    const std::string clean_label = (steps > 0 ? "Clean " + items_text(steps) : std::string("Clean")) + "###run-cleanup";
    Action actions[] = {
        {.label = "Review items###review", .width = kSecondaryUnits},
        {.label = clean_label.c_str(),
         .variant = ps.backup_first ? facet::ButtonVariant::Primary : facet::ButtonVariant::Destructive,
         .width = kPrimaryUnits},
    };
    int pressed = -1;

    if (status == PlanStatus::NothingTicked) {
        actions[0].enabled = actions[1].enabled = false;
        pressed = draw_hero("Nothing is selected", facet::TextStyle::Title2, facet::Tone::Neutral,
                            "Pick a profile above, or tick items under What gets removed.", actions);
    } else if (status == PlanStatus::NeedGames) {
        actions[0].enabled = actions[1].enabled = false;
        pressed = draw_hero("Choose games to reset", facet::TextStyle::Title2, facet::Tone::Neutral,
                            "Game Reset only removes the saves of the games you pick under Games to reset.", actions);
    } else if (status == PlanStatus::Failed) {
        const char* message = ps.dev.plan_error ? "Access to C:\\Program Files (x86)\\Steam\\userdata was denied."
                                                : ps.plan_error.c_str();
        const Action retry[] = {
            {.label = "Review items###review", .enabled = false, .width = kSecondaryUnits},
            {.label = "Try again", .variant = facet::ButtonVariant::Primary, .width = kPrimaryUnits},
        };
        if (draw_hero("Could not measure the cleanup", facet::TextStyle::Title2, facet::Tone::Destructive, message,
                      retry) == 1) {
            rebuild_plan_now(state, ps);
        }
    } else if (status == PlanStatus::Building && !has_plan) {
        actions[0].enabled = actions[1].enabled = false;
        pressed = draw_hero(nullptr, facet::TextStyle::LargeTitle, facet::Tone::Neutral, nullptr, actions,
                            [&](float text_w) {
                                facet::Skeleton({.shape = facet::SkeletonShape::Rect,
                                                 .width = std::min(160.0F, units(text_w)),
                                                 .height = 30.0F});
                                gap(t.spacing.sm);
                                facet::Spinner("Measuring the ticked items", {.size = facet::ControlSize::Small});
                            });
    } else if (empty_plan) {
        actions[0].enabled = actions[1].enabled = false;
        pressed = draw_hero("Already clean", facet::TextStyle::Title2, facet::Tone::Neutral,
                            "The ticked items are already gone, or everything they cover is excluded.", actions);
    } else if (has_plan) {
        // Ready, or Building with the previous plan still shown while the new one is measured.
        const auto& plan = *state.last_plan;
        const bool updating = status == PlanStatus::Building;
        actions[0].enabled = !updating;
        actions[1].enabled = !updating && blocked == nullptr;
        const std::string size = stc::ui::format_bytes(plan.total_bytes);
        std::string line = plain_line(plan);
        if (state.selected_target_ids.contains("steam.userdata") && !is_game_reset(state)) {
            // The one consequence that must not hide below the fold in a short window.
            const AccountCounts counts = account_counts(state);
            const int affected = counts.total - counts.excluded;
            line += ", including the game saves of " + std::to_string(affected) +
                    (affected == 1 ? " account" : " accounts");
        }
        line += blocked != nullptr ? ". " + std::string(blocked) : ".";
        pressed = draw_hero(size.c_str(), facet::TextStyle::LargeTitle, facet::Tone::Neutral, line.c_str(), actions,
                            [&](float) {
                                if (updating) {
                                    gap(t.spacing.xs);
                                    facet::Spinner("Updating for the new selection", {.size = facet::ControlSize::Small});
                                }
                            });
    }

    if (pressed == 0) {
        ps.details_open = true;
    } else if (pressed == 1 && plan_is_current(status) && has_plan && steps > 0 && blocked == nullptr) {
        request_run(state, ps);
    }

    draw_backup_row(state, ps);
    stc::ui::end_page_card();
}

}  // namespace stc::ui::pages::cleanup
