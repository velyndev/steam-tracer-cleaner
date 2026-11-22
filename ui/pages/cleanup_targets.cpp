// "What gets removed" on the Cleanup page: one expandable row per target category with a tri-state
// box, a plain explanation, how many of its targets are ticked and its share of the current plan;
// opened, the category's targets with their own boxes and sizes. A search field above filters the
// targets by name and description.

#include <imgui.h>
#include <imgui_internal.h>

#include <facet/facet.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "app/app_state.hpp"
#include "core/dry_run.hpp"
#include "core/targets.hpp"
#include "ui/common.hpp"
#include "ui/pages/cleanup_state.hpp"
#include "ui/pages/cleanup_view.hpp"

namespace stc::ui::pages::cleanup {
namespace {

using stc::app::AppState;
using stc::core::TargetCategory;

// From what Steam rebuilds on its own to what is personal, so the order reads as rising stakes.
constexpr std::array<TargetCategory, kCategoryCount> kCategoryOrder{
    TargetCategory::Cache,          TargetCategory::Log,
    TargetCategory::CrashDump,      TargetCategory::BrowserResidue,
    TargetCategory::AccountResidue, TargetCategory::ControllerResidue,
    TargetCategory::GameData,
};

// The share column: the size over a thin proportional bar.
constexpr float kShareUnits = 96.0F;
constexpr float kBarUnits = 4.0F;

const char* category_name(TargetCategory category) {
    switch (category) {
        case TargetCategory::Cache: return "Caches";
        case TargetCategory::Log: return "Logs";
        case TargetCategory::CrashDump: return "Crash reports";
        case TargetCategory::BrowserResidue: return "Web data and cookies";
        case TargetCategory::AccountResidue: return "Sign-in and account data";
        case TargetCategory::ControllerResidue: return "Controller layouts";
        case TargetCategory::GameData: return "Saves and personal files";
        case TargetCategory::Hwid: return "Hardware identifiers";
    }
    return "";
}

const char* category_explanation(TargetCategory category) {
    switch (category) {
        case TargetCategory::Cache: return "Temporary files Steam downloads or rebuilds when it needs them.";
        case TargetCategory::Log: return "Records of what Steam did. Steam writes new ones.";
        case TargetCategory::CrashDump: return "Files left behind when Steam or a game crashed. Nothing you use is lost.";
        case TargetCategory::BrowserResidue:
            return "Caches of Steam's built-in browser, which Steam rebuilds, and Steam cookies in your browsers.";
        case TargetCategory::AccountResidue:
            return "Remembered accounts, sign-in tokens and account settings. Removing them signs accounts out.";
        case TargetCategory::ControllerResidue:
            return "Your Steam Input layouts. Steam does not rebuild them; only Steam Cloud copies come back.";
        case TargetCategory::GameData:
            return "Local saves, screenshots and install records. Steam does not rebuild them.";
        case TargetCategory::Hwid: return "";
    }
    return "";
}

// Case-insensitive substring match on the target's name and description.
bool target_matches(const std::string& query_lower, const std::string& name, const std::string& description) {
    if (query_lower.empty()) {
        return true;
    }
    return lower(name).find(query_lower) != std::string::npos ||
           lower(description).find(query_lower) != std::string::npos;
}

// What a ticked target or category contributes to the current plan, as short text.
std::string share_text(std::uint64_t bytes, int steps, int ticked) {
    if (bytes > 0) {
        return stc::ui::format_bytes(bytes);
    }
    if (steps > 0) {
        return items_text(static_cast<std::uint64_t>(steps));
    }
    return ticked > 0 ? "Nothing found" : "";
}

// How far a category row reaches past the card content on each side, so its hover fill and
// focus ring have room; its content stays on the card content's edges.
float row_bleed() {
    return facet::Px(facet::T().spacing.sm);
}

// The keyboard focus ring around a custom row, as Facet draws it around its controls.
void focus_ring(ImVec2 min, ImVec2 max) {
    if (!ImGui::IsItemFocused() || !GImGui->NavCursorVisible) {
        return;
    }
    const facet::Theme& t = facet::T();
    facet::StrokeRect(ImGui::GetWindowDrawList(), min, max, facet::ToU32(t.colors.focusRing),
                      facet::Px(t.radii.control), std::max(1.0F, facet::PxSnapped(t.metrics.focusRingWidth)));
}

struct Row {
    const stc::core::Target* target;
    std::string name;
    std::string description;
};

// One category: the header row, and its targets when open. Returns the height it took.
void draw_category(AppState& state, PageState& ps, int slot, TargetCategory category, const std::vector<Row>& rows,
                   int ticked, int total, bool forced_open, bool plan_ready, float width) {
    const facet::Theme& t = facet::T();
    static std::array<float, kCategoryCount> s_row_h{};  // last frame's header height, for the hover fill
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = facet::Px(t.spacing.md);  // above and below the header text
    // The row starts row_bleed() left of the card content and ends that much right of it, so the
    // box sits on the content's left edge and the chevron ends on its right edge, as in the
    // card's title, search field and hairlines.
    const float inset = row_bleed();
    const float box = facet::Px(t.metrics.checkboxSize);
    const float icon = facet::MeasureText(facet::TextStyle::Body, "Ag").y;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool open = forced_open || ps.category_open[static_cast<std::size_t>(slot)];

    // Ticked state over the rows shown (all of them without a filter).
    int visible_ticked = 0;
    std::uint64_t bytes = 0;
    int steps = 0;
    for (const auto& row : rows) {
        if (state.selected_target_ids.contains(row.target->id)) {
            ++visible_ticked;
        }
    }
    for (const auto& target : stc::core::all_targets()) {
        if (target.category == category && state.selected_target_ids.contains(target.id)) {
            if (const auto it = ps.shares.find(target.id); it != ps.shares.end()) {
                bytes += it->second.bytes;
                steps += it->second.steps;
            }
        }
    }

    // Hover fill under the whole header (drawn first, from last frame's height).
    const float last_h = s_row_h[static_cast<std::size_t>(slot)];
    if (last_h > 0.0F && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + width, origin.y + last_h)) && !ImGui::IsAnyItemActive()) {
        facet::FillRect(dl, origin, ImVec2(origin.x + width, origin.y + last_h), facet::ToU32(t.colors.fill[3]),
                        facet::Px(t.radii.control));
    }

    // Columns from the trailing edge: chevron, share, count.
    const float right = origin.x + width - inset;
    const float chevron_x = right - icon;
    const float share_w = facet::Px(kShareUnits);
    const float share_x = chevron_x - facet::Px(t.spacing.md) - share_w;
    char count_text[32];
    std::snprintf(count_text, sizeof(count_text), "%d of %d", ticked, total);
    const float count_w = facet::MeasureText(facet::TextStyle::Footnote, "00 of 00").x;
    const float count_x = share_x - facet::Px(t.spacing.md) - count_w;
    const float text_x = origin.x + inset + box + facet::Px(t.spacing.md);
    const float text_w = std::max(1.0F, count_x - facet::Px(t.spacing.md) - text_x);

    // Text: the name and the explanation.
    const float top = origin.y + pad;
    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(text_x, top));
    ImGui::BeginGroup();
    facet::StyledText(category_name(category), {.style = facet::TextStyle::Headline});
    gap(2.0F);
    facet::TruncatedText(category_explanation(category), {.maxLines = 2,
                                                          .style = facet::TextStyle::Footnote,
                                                          .secondary = true,
                                                          .width = units(text_w)});
    ImGui::EndGroup();
    const float text_h = ImGui::GetItemRectSize().y;
    const float row_h = std::ceil(text_h + pad * 2.0F);
    const float mid = origin.y + row_h * 0.5F;

    // The tri-state box for the shown targets, centred on the row.
    facet::CheckState check = visible_ticked == 0                              ? facet::CheckState::Off
                              : visible_ticked == static_cast<int>(rows.size()) ? facet::CheckState::On
                                                                                 : facet::CheckState::Mixed;
    const float control_h = facet::ControlHeight(facet::ControlSize::Inherit);
    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + inset, mid - control_h * 0.5F));
    char box_id[64];
    std::snprintf(box_id, sizeof(box_id), "##tick-%s", category_name(category));
    if (facet::Checkbox(box_id, &check)) {
        for (const auto& row : rows) {
            if (check == facet::CheckState::On) {
                state.selected_target_ids.insert(row.target->id);
            } else {
                state.selected_target_ids.erase(row.target->id);
            }
        }
    }
    ImGui::SetItemTooltip(check == facet::CheckState::On ? "Untick every item in %s" : "Tick every item in %s",
                          category_name(category));

    // Count.
    const float foot_h = facet::MeasureText(facet::TextStyle::Footnote, "Ag").y;
    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(count_x, mid - foot_h * 0.5F));
    facet::StyledText(count_text, {.style = facet::TextStyle::Footnote,
                                   .secondary = true,
                                   .align = facet::Align::End,
                                   .width = units(count_w)});

    // Share of the plan: size over a thin bar in one quiet tone.
    if (ticked > 0) {
        const float body_h = facet::MeasureText(facet::TextStyle::Body, "Ag").y;
        const float bar_h = facet::Px(kBarUnits);
        const float block_h = body_h + facet::Px(t.spacing.xs) + bar_h;
        const float block_top = mid - block_h * 0.5F;
        if (plan_ready) {
            const std::string text = share_text(bytes, steps, ticked);
            reset_line_baseline();
            ImGui::SetCursorScreenPos(ImVec2(share_x, block_top));
            facet::StyledText(text.c_str(), {.secondary = bytes == 0,
                                             .align = facet::Align::End,
                                             .width = kShareUnits});
            const float bar_y = block_top + body_h + facet::Px(t.spacing.xs);
            const ImVec2 track_min(share_x, bar_y);
            const ImVec2 track_max(share_x + share_w, bar_y + bar_h);
            facet::FillRect(dl, track_min, track_max, facet::ToU32(t.colors.fill[2]), bar_h * 0.5F);
            const std::uint64_t total_bytes = state.last_plan ? state.last_plan->total_bytes : 0;
            if (bytes > 0 && total_bytes > 0) {
                const float fraction =
                    std::clamp(static_cast<float>(static_cast<double>(bytes) / static_cast<double>(total_bytes)), 0.0F, 1.0F);
                // At least a dot, so a share that rounds to nothing still shows.
                const float fill_w = std::max(bar_h, share_w * fraction);
                facet::FillRect(dl, track_min, ImVec2(share_x + fill_w, track_max.y),
                                facet::ToU32(t.colors.labelSecondary), bar_h * 0.5F);
            }
        } else {
            reset_line_baseline();
            ImGui::SetCursorScreenPos(ImVec2(share_x, mid - body_h * 0.5F));
            facet::Skeleton({.shape = facet::SkeletonShape::Rect, .width = kShareUnits, .height = 12.0F});
        }
    }

    // Chevron.
    facet::DrawIcon(dl, facet::Icon(open ? "chevron-down" : "chevron-right"), ImVec2(chevron_x, mid - icon * 0.5F), icon,
                    facet::ToU32(t.colors.labelSecondary));

    // The rest of the header opens and closes the category (click, Enter, Space).
    reset_line_baseline();
    const ImVec2 hit_min(text_x - facet::Px(t.spacing.xs), origin.y);
    ImGui::SetCursorScreenPos(hit_min);
    char row_id[64];
    std::snprintf(row_id, sizeof(row_id), "%s###row-%d", category_name(category), slot);
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton(row_id, ImVec2(std::max(1.0F, origin.x + width - hit_min.x), row_h)) && !forced_open) {
        ps.category_open[static_cast<std::size_t>(slot)] = !ps.category_open[static_cast<std::size_t>(slot)];
    }
    if (ImGui::IsItemHovered() && !forced_open) {
        facet::SetHandCursor();
    }
    focus_ring(ImVec2(origin.x, origin.y), ImVec2(origin.x + width, origin.y + row_h));
    s_row_h[static_cast<std::size_t>(slot)] = row_h;

    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + row_h));
    ImGui::Dummy(ImVec2(0.0F, 0.0F));
    if (!open) {
        return;
    }

    // The targets, under the category name.
    const float target_x = text_x;
    const float size_w = facet::Px(kShareUnits);
    const float size_x = right - size_w;
    const float desc_w = std::max(1.0F, size_x - facet::Px(t.spacing.md) - (target_x + box + facet::Px(t.spacing.sm)));
    for (const auto& row : rows) {
        ImGui::PushID(row.target->id.c_str());
        const ImVec2 line = ImGui::GetCursorScreenPos();
        bool on = state.selected_target_ids.contains(row.target->id);
        reset_line_baseline();
        ImGui::SetCursorScreenPos(ImVec2(target_x, line.y));
        ImGui::SetNextItemWidth(std::max(1.0F, size_x - facet::Px(t.spacing.md) - target_x));
        if (facet::Checkbox(row.name.c_str(), &on)) {
            if (on) {
                state.selected_target_ids.insert(row.target->id);
            } else {
                state.selected_target_ids.erase(row.target->id);
            }
        }
        const float label_mid = line.y + control_h * 0.5F;
        if (on && plan_ready) {
            const auto it = ps.shares.find(row.target->id);
            const std::uint64_t b = it != ps.shares.end() ? it->second.bytes : 0;
            const int s = it != ps.shares.end() ? it->second.steps : 0;
            const std::string text = share_text(b, s, 1);
            reset_line_baseline();
            ImGui::SetCursorScreenPos(ImVec2(size_x, label_mid - foot_h * 0.5F));
            facet::StyledText(text.c_str(), {.style = facet::TextStyle::Footnote,
                                             .secondary = true,
                                             .align = facet::Align::End,
                                             .width = kShareUnits});
        }
        if (!row.description.empty()) {
            const float label_bottom =
                line.y + control_h - (control_h - facet::MeasureText(facet::TextStyle::Body, "Ag").y) * 0.5F;
            reset_line_baseline();
            ImGui::SetCursorScreenPos(ImVec2(target_x + box + facet::Px(t.spacing.sm), label_bottom + facet::Px(2.0F)));
            facet::StyledText(row.description.c_str(), {.style = facet::TextStyle::Footnote,
                                                        .secondary = true,
                                                        .wrap = true,
                                                        .width = units(desc_w)});
        }
        gap(t.spacing.md);
        ImGui::PopID();
    }
    gap(t.spacing.xs);
}

}  // namespace

void draw_targets_section(AppState& state, PageState& ps, PlanStatus status, float width_px) {
    const facet::Theme& t = facet::T();
    const bool plan_ready = status == PlanStatus::Ready && !ps.dev.plan_loading;

    // Heading with the ticked count at the trailing edge.
    int ticked_all = 0;
    int total_all = 0;
    for (const auto& target : stc::core::all_targets()) {
        if (target.category == TargetCategory::Hwid) {
            continue;
        }
        ++total_all;
        if (state.selected_target_ids.contains(target.id)) {
            ++ticked_all;
        }
    }
    (void)width_px;  // the shared page card fills the column
    char description[96];
    std::snprintf(description, sizeof(description), "%d of %d items ticked. Open a group to change single items.",
                  ticked_all, total_all);
    stc::ui::begin_page_card("categories", "What gets removed", description);
    const float inner_w = ImGui::GetContentRegionAvail().x;
    facet::SearchField("##target-filter", &ps.search,
                       {.placeholder = "Search items by name or description", .width = units(inner_w)});
    gap(t.spacing.sm);
    const std::string query = lower(ps.search);
    // The card's own padding stays; the rows reach a little into it so their hover fill has room.
    const float bleed = row_bleed();
    const float row_x = ImGui::GetCursorScreenPos().x - bleed;
    const float row_w = inner_w + bleed * 2.0F;
    bool any_shown = false;
    bool first = true;
    for (int slot = 0; slot < kCategoryCount; ++slot) {
        const TargetCategory category = kCategoryOrder[static_cast<std::size_t>(slot)];
        std::vector<Row> rows;
        int total = 0;
        int ticked = 0;
        for (const auto& target : stc::core::all_targets()) {
            if (target.category != category) {
                continue;
            }
            ++total;
            if (state.selected_target_ids.contains(target.id)) {
                ++ticked;
            }
            Row row{&target, stc::ui::to_utf8(target.display_name), stc::ui::to_utf8(target.description)};
            if (target_matches(query, row.name, row.description)) {
                rows.push_back(std::move(row));
            }
        }
        if (rows.empty()) {
            continue;  // no target in this category, or none matches the search
        }
        any_shown = true;
        ImGui::PushID(slot);
        if (!first) {
            const float y = ImGui::GetCursorScreenPos().y;
            facet::HairlineH(ImGui::GetWindowDrawList(), row_x + bleed, row_x + row_w - bleed, y,
                             facet::ToU32(t.colors.separator));
        }
        first = false;
        reset_line_baseline();
        ImGui::SetCursorScreenPos(ImVec2(row_x, ImGui::GetCursorScreenPos().y));
        draw_category(state, ps, slot, category, rows, ticked, total, !query.empty(), plan_ready, row_w);
        ImGui::PopID();
    }
    if (!any_shown) {
        if (facet::EmptyState("No item matches the search###no-target",
                              {.message = "Items are matched by name and description.",
                               .icon = facet::Icon("search"),
                               .actionLabel = "Clear search",
                               .width = units(inner_w)})) {
            ps.search.clear();
        }
    }
    stc::ui::end_page_card();
}

}  // namespace stc::ui::pages::cleanup
