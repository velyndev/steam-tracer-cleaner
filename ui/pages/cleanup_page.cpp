#include "ui/pages/cleanup_page.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <facet/facet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <span>
#include <string>
#include <vector>

#include "app/app_state.hpp"
#include "core/appid_map.hpp"
#include "core/dry_run.hpp"
#include "core/targets.hpp"
#include "ui/common.hpp"
#include "ui/pages/cleanup_state.hpp"
#include "ui/pages/cleanup_view.hpp"

// --- Shared helpers (cleanup_view.hpp) -------------------------------------------------------------

namespace stc::ui::pages::cleanup {

float units(float pixels) {
    return pixels / facet::Scale();
}

// The cursor already sits ItemSpacing.y under the previous item, and a Dummy adds that once more.
// It moves the cursor back up to the item's bottom edge first (never down, which Dear ImGui
// reports as growing the window without an item) and always ends with an item.
void gap(float design_units) {
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float height = facet::Px(design_units) - spacing;  // the Dummy adds `spacing` after itself
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - spacing + std::min(0.0F, height));
    ImGui::Dummy(ImVec2(0.0F, std::max(0.0F, height)));
}

void reset_line_baseline() {
    ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0.0F;
}

std::string items_text(std::uint64_t n) {
    return std::to_string(n) + (n == 1 ? " item" : " items");
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

AccountCounts account_counts(const stc::app::AppState& state) {
    AccountCounts counts;
    counts.total = static_cast<int>(state.accounts.size());
    for (const auto& account : state.accounts) {
        if (state.ignore_list.preserves_account(account.steamid64)) {
            ++counts.excluded;
        }
    }
    return counts;
}

void update_shares(const stc::app::AppState& state, PageState& ps) {
    if (ps.shares_version == ps.plan_version) {
        return;
    }
    ps.shares_version = ps.plan_version;
    ps.shares.clear();
    if (!state.last_plan) {
        return;
    }
    for (const auto& step : state.last_plan->steps) {
        auto& share = ps.shares[step.target_id];
        share.bytes += step.op.size_bytes;
        ++share.steps;
    }
}

bool plan_is_current(PlanStatus status) {
    return status == PlanStatus::Ready;
}

void request_run(stc::app::AppState& state, PageState& ps) {
    // A dialog when the cleanup cannot be undone, or for Full Wipe when Settings asks for one.
    const bool needs_confirm = !ps.backup_first || (is_full_wipe(state) && state.confirm_full_wipe);
    if (needs_confirm) {
        ps.confirm_open = true;
    } else {
        start_cleanup(state, ps, ps.backup_first);
    }
}

}  // namespace stc::ui::pages::cleanup

namespace stc::ui::pages {
namespace {

using stc::app::AppState;
using namespace stc::ui::pages::cleanup;

// Profile cards sit four in a row from this content width (units), two by two below it.
constexpr float kFourCardWidth = 800.0F;

// One plain sentence about what a profile means for the person, derived from its targets.
std::string profile_consequence(const stc::core::Profile& profile) {
    const auto has = [&](const char* id) {
        return std::find(profile.target_ids.begin(), profile.target_ids.end(), id) != profile.target_ids.end();
    };
    const bool signs_out = has("steam.config_vdf") || has("steam.local_vdf") || has("steam.loginusers") ||
                           has("steam.reg.autologin");
    const bool saves = has("steam.userdata");
    const bool game_only = profile.name == L"Game Reset";
    if (saves && game_only) {
        return "Removes saves of the games you pick. You stay signed in.";
    }
    if (signs_out && saves) {
        return "Signs accounts out and removes their saves.";
    }
    if (signs_out) {
        return "Signs accounts out. Game saves stay.";
    }
    if (saves) {
        return "Removes local game saves. You stay signed in.";
    }
    return "You stay signed in. Game saves stay.";
}

void draw_profile_section(AppState& state, float width_px) {
    if (state.profiles.empty()) {
        return;
    }
    const facet::Theme& t = facet::T();
    const ImVec2 row = ImGui::GetCursorScreenPos();
    facet::StyledText("Profile", {.style = facet::TextStyle::Headline});
    const ImVec2 after = ImGui::GetCursorScreenPos();
    if (profile_modified(state)) {
        // The pill says the ticked set left the profile; the link puts it back. Both sit on the
        // heading row, next to the cards they refer to.
        const float title_w = facet::MeasureText(facet::TextStyle::Headline, "Profile").x;
        const float title_h = facet::MeasureText(facet::TextStyle::Headline, "Ag").y;
        const float pill_h = facet::ControlHeight(facet::ControlSize::Small);
        reset_line_baseline();
        ImGui::SetCursorScreenPos(
            ImVec2(row.x + title_w + facet::Px(t.spacing.sm), row.y + (title_h - pill_h) * 0.5F));
        facet::StatusPill("Modified", {.size = facet::ControlSize::Small});
        const char* reset_label = "Reset to profile";
        const float link_w = facet::MeasureText(facet::TextStyle::Body, reset_label).x;
        const float body_h = facet::MeasureText(facet::TextStyle::Body, "Ag").y;
        reset_line_baseline();
        ImGui::SetCursorScreenPos(ImVec2(row.x + width_px - link_w, row.y + (title_h - body_h) * 0.5F));
        if (facet::Link(reset_label)) {
            apply_profile(state);
        }
        ImGui::SetCursorScreenPos(after);
    }
    gap(t.spacing.sm);

    std::vector<std::string> names;
    std::vector<std::string> texts;
    names.reserve(state.profiles.size());
    texts.reserve(state.profiles.size());
    for (const auto& profile : state.profiles) {
        names.push_back(stc::ui::to_utf8(profile.name));
        texts.push_back(stc::ui::to_utf8(profile.description) + "\n\n" + profile_consequence(profile));
    }
    std::vector<facet::OptionCard> cards;
    cards.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        cards.push_back({.label = names[i].c_str(), .description = texts[i].c_str()});
    }
    const int columns = units(width_px) >= kFourCardWidth ? 4 : 2;
    int index = std::clamp(state.selected_profile_index, 0, static_cast<int>(state.profiles.size()) - 1);
    if (facet::OptionCardGroup("##profile", &index, cards, {.columns = columns, .width = units(width_px)})) {
        state.selected_profile_index = index;
        apply_profile(state);
    }
}

void draw_full_wipe_alert(AppState& state, float width_px) {
    const AccountCounts counts = account_counts(state);
    std::string message = "Game saves and settings, controller layouts, screenshots and non-Steam games in the "
                          "library are removed";
    if (counts.total > 0) {
        const int affected = counts.total - counts.excluded;
        message += " for " + std::to_string(affected) + " of " + std::to_string(counts.total) +
                   (counts.total == 1 ? " account." : " accounts.");
        if (counts.excluded > 0) {
            message += " " + std::to_string(counts.excluded) +
                       (counts.excluded == 1 ? " excluded account is kept." : " excluded accounts are kept.");
        } else {
            message += " No account is excluded yet.";
        }
    } else {
        message += " for every account that is not excluded.";
    }
    const auto action = facet::InlineAlert("Full Wipe removes personal data###full-wipe", nullptr, message.c_str(),
                                           {.tone = facet::Tone::Warning,
                                            .actionLabel = "Edit exclusions",
                                            .width = units(width_px)});
    if (action == facet::InlineAlertAction::Action) {
        state.current_page = stc::app::Page::Exclusions;
    }
}

bool parse_appid(const std::string& text, std::uint32_t* out) {
    if (text.empty() || text.size() > 10) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value == 0 || value > 0xFFFFFFFFULL) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

void draw_games_section(AppState& state, PageState& ps) {
    const facet::Theme& t = facet::T();
    const auto games = stc::core::appid::all_games();
    if (!ps.appids_synced) {
        // Tags hold the chosen AppIDs that are not in the known list.
        ps.other_appids.clear();
        for (const auto appid : state.selected_appids) {
            if (stc::core::appid::find_by_appid(appid) == nullptr) {
                ps.other_appids.push_back(std::to_string(appid));
            }
        }
        ps.appids_synced = true;
    }

    stc::ui::begin_page_card("games", "Games to reset",
                             "Only the saves and settings of these games are removed, for every account that is "
                             "not excluded.");
    const float inner_w = ImGui::GetContentRegionAvail().x;

    std::vector<std::string> names;
    std::vector<std::string> details;
    names.reserve(games.size());
    details.reserve(games.size());
    for (const auto& game : games) {
        names.push_back(stc::ui::to_utf8(game.display_name));
        details.push_back(std::to_string(game.appid));
    }
    std::vector<facet::ChoiceItem> items;
    items.reserve(games.size());
    std::array<bool, 64> checked{};
    const std::size_t count = std::min(games.size(), checked.size());
    for (std::size_t i = 0; i < count; ++i) {
        items.push_back({.label = names[i].c_str(), .detail = details[i].c_str()});
        checked[i] = state.selected_appids.contains(games[i].appid);
    }

    bool changed = false;
    if (facet::MultiDropdown("Known games", std::span<bool>(checked.data(), count),
                             std::span<const facet::ChoiceItem>(items.data(), count),
                             {.placeholder = "None chosen", .searchable = true, .width = units(inner_w)})) {
        changed = true;
    }
    gap(t.spacing.sm);
    if (facet::TagInput("Other AppIDs", &ps.other_appids,
                        {.placeholder = "Type an AppID, then Enter",
                         .width = units(inner_w),
                         .error = ps.appid_error.empty() ? nullptr : ps.appid_error.c_str(),
                         .helpText = "The number in the game's store address, for example 730."})) {
        changed = true;
        ps.appid_error.clear();
        std::vector<std::string> kept;
        for (const auto& tag : ps.other_appids) {
            std::uint32_t appid = 0;
            if (!parse_appid(tag, &appid)) {
                ps.appid_error = "\"" + tag + "\" is not an AppID. Use digits only.";
                continue;
            }
            // A known game moves into the dropdown instead of staying a tag.
            if (const auto* game = stc::core::appid::find_by_appid(appid)) {
                const auto it = std::find_if(games.begin(), games.end(),
                                             [&](const auto& g) { return g.appid == game->appid; });
                if (it != games.end()) {
                    checked[static_cast<std::size_t>(it - games.begin())] = true;
                }
                continue;
            }
            kept.push_back(std::to_string(appid));
        }
        ps.other_appids = std::move(kept);
    }
    stc::ui::end_page_card();

    if (changed) {
        state.selected_appids.clear();
        for (std::size_t i = 0; i < count; ++i) {
            if (checked[i]) {
                state.selected_appids.insert(games[i].appid);
            }
        }
        for (const auto& tag : ps.other_appids) {
            std::uint32_t appid = 0;
            if (parse_appid(tag, &appid)) {
                state.selected_appids.insert(appid);
            }
        }
    }
}

void draw_confirm(AppState& state, PageState& ps) {
    if (!ps.confirm_open || !state.last_plan) {
        ps.confirm_open = false;
        return;
    }
    const auto& plan = *state.last_plan;
    const std::string count = items_text(plan.steps.size());
    std::string message = count + " (" + stc::ui::format_bytes(plan.total_bytes) + ") will be removed";
    if (is_full_wipe(state) && state.selected_target_ids.contains("steam.userdata")) {
        const AccountCounts counts = account_counts(state);
        const int affected = counts.total - counts.excluded;
        message += ", including the game saves of " + std::to_string(affected) +
                   (affected == 1 ? " account" : " accounts");
    }
    message += ". Steam and any running games are closed first. ";
    message += ps.backup_first ? "A backup is made, so you can restore everything from Backups."
                               : "No backup is made, so this cannot be undone.";
    const std::string title = (ps.backup_first ? std::string("Run Full Wipe?") : std::string("Clean without a backup?")) +
                              "###confirm-clean";
    const std::string confirm = "Clean " + count;
    const auto result = facet::ConfirmDialog(title.c_str(), &ps.confirm_open, message.c_str(),
                                             {.confirmLabel = confirm.c_str(),
                                              .cancelLabel = "Keep everything",
                                              .destructive = true});
    if (result == facet::DialogResult::Confirmed) {
        start_cleanup(state, ps, ps.backup_first);
    }
}

// The summary card in the page flow at the cursor, kept inside the visible part of the scroll area:
// where it fits it sits between the profiles and the categories; scrolled past, it stays at the top
// edge; below the fold (a short window, an open Game Reset block) it waits at the bottom edge. It is
// a child window, so it draws over the rows that scroll under it and takes their pointer input.
void draw_pinned_summary(AppState& state, PageState& ps, PlanStatus status, float width_px) {
    static float s_height = 0.0F;  // the card's height last frame (px), to place it before drawing
    const facet::Theme& t = facet::T();
    const ImVec2 natural = ImGui::GetCursorScreenPos();
    const ImRect view = ImGui::GetCurrentWindow()->InnerRect;
    float y = natural.y;
    if (s_height > 0.0F) {
        y = std::clamp(natural.y, view.Min.y, std::max(view.Min.y, view.Max.y - s_height));
    }
    y = std::round(y);
    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(natural.x, y));
    const bool child = ImGui::BeginChild("##summary", ImVec2(width_px, 0.0F),
                                         ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    if (child) {
        // The page background behind the card hides the rows that scroll under it; the band above
        // and below the card is that background too.
        // While the card floats over rows, the band on the side the rows come from fades in, so
        // the rows read as passing under the card instead of being cut off.
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImVec2 min = window->Pos;
        const ImVec2 max(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
        const float band_px = std::min(facet::Px(t.spacing.xl), window->Size.y * 0.5F);
        const ImU32 solid = facet::ToU32(t.colors.bg);
        const ImU32 clear = facet::ToU32(ImVec4(t.colors.bg.x, t.colors.bg.y, t.colors.bg.z, 0.0F));
        const bool over_top = y < natural.y - 0.5F;     // pinned to the bottom edge, rows above
        const bool over_bottom = y > natural.y + 0.5F;  // pinned to the top edge, rows below
        const float solid_top = over_top ? min.y + band_px : min.y;
        const float solid_bottom = over_bottom ? max.y - band_px : max.y;
        if (over_top) {
            facet::FillGradientRect(window->DrawList, min, ImVec2(max.x, solid_top), clear, solid);
        }
        facet::FillRect(window->DrawList, ImVec2(min.x, solid_top), ImVec2(max.x, solid_bottom), solid);
        if (over_bottom) {
            facet::FillGradientRect(window->DrawList, ImVec2(min.x, solid_bottom), max, solid, clear);
        }
        // The bands are the page's block gap, so the gaps around the card match every other block.
        stc::ui::page_block_gap();
        draw_summary_card(state, ps, status, width_px);
        stc::ui::page_block_gap();
    }
    ImGui::EndChild();
    s_height = ImGui::GetItemRectSize().y;
    // The flow continues under the card's place in the page, wherever the card is drawn.
    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(natural.x, natural.y + s_height));
}

}  // namespace

void draw_cleanup_page(stc::app::AppState& state) {
    stc::ui::page_header("Cleanup", "Pick how much to remove, see what it frees, then clean.");
    if (!state.install) {
        stc::ui::steam_not_found_state(state);
        return;
    }

    PageState& ps = page_state();
    const facet::Theme& t = facet::T();
    if (!ps.initialized) {
        ps.backup_first = state.backup_by_default;
        ps.seen_backup_default = state.backup_by_default;
        ps.initialized = true;
    }
    if (ps.seen_backup_default != state.backup_by_default) {
        // The Settings default changed: the per-run choice follows it.
        ps.seen_backup_default = state.backup_by_default;
        ps.backup_first = state.backup_by_default;
    }
    if (!state.profiles.empty()) {
        state.selected_profile_index =
            std::clamp(state.selected_profile_index, 0, static_cast<int>(state.profiles.size()) - 1);
    }

    PlanStatus status = update_plan(state, ps);
    apply_dev_state(state, ps, status);  // capture-only; no effect when state.dev_state is empty
    if (ps.dev.plan_loading) {
        status = PlanStatus::Building;  // capture-only, as above
    } else if (ps.dev.plan_error) {
        status = PlanStatus::Failed;
    }
    update_shares(state, ps);
    if (ps.dev.nothing) {
        ps.shares.clear();  // capture-only: the "Already clean" state shows no shares
        ps.shares_version = ~0ULL;
    }
    const bool running = run_active(state, ps);

    if (stc::ui::begin_page_body("cleanup-body")) {
        const float width = std::max(1.0F, ImGui::GetContentRegionAvail().x);

        ImGui::BeginDisabled(running);
        draw_profile_section(state, width);
        if (is_full_wipe(state)) {
            stc::ui::page_block_gap();
            draw_full_wipe_alert(state, width);
        }
        if (is_game_reset(state)) {
            stc::ui::page_block_gap();
            draw_games_section(state, ps);
        }
        ImGui::EndDisabled();

        // The summary card brings its own block gaps (see draw_pinned_summary).
        draw_pinned_summary(state, ps, status, width);

        ImGui::BeginDisabled(running);
        draw_targets_section(state, ps, status, width);
        ImGui::EndDisabled();
        // Room under the last card; an item, so the scroll range includes it.
        ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.xl)));
        if (ps.dev.scrolled) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());  // capture-only (cleanup_dev_states.cpp)
        }
        stc::ui::end_page_body();
    }

    draw_details_drawer(state, ps);
    draw_confirm(state, ps);
}

}  // namespace stc::ui::pages
