// The details drawer of the Cleanup page: every step of the current plan with its path, kind and
// target, filter chips by kind, and a context menu per step (copy the path, show it in Explorer,
// exclude it from cleanups). This is where raw paths live; the page itself shows sizes and names.
//
// A drawer rather than a modal: it keeps the summary card visible beside it at wide sizes, takes
// the full window height that a long list needs, and closes with Escape or a click beside it.

#include <imgui.h>
#include <imgui_internal.h>

#include <facet/facet.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

#include "app/app_state.hpp"
#include "core/dry_run.hpp"
#include "core/targets.hpp"
#include "ui/common.hpp"
#include "ui/pages/cleanup_state.hpp"
#include "ui/pages/cleanup_view.hpp"

namespace stc::ui::pages::cleanup {
namespace {

using stc::app::AppState;
using stc::core::OpKind;

// Step rows have two lines (path, then kind and target), so every row has the same height.
constexpr float kStepRowUnits = 46.0F;
// The drawer's width, and the room it leaves beside it in a narrow window.
constexpr float kDrawerUnits = 640.0F;
constexpr float kDrawerMarginUnits = 48.0F;

void rebuild_visible_steps(const AppState& state, PageState& ps) {
    unsigned mask = 0;
    for (int i = 0; i < kFamilyCount; ++i) {
        if (ps.family_on[static_cast<std::size_t>(i)]) {
            mask |= 1U << i;
        }
    }
    if (ps.visible_version == ps.plan_version && ps.visible_mask == mask) {
        return;
    }
    ps.visible_version = ps.plan_version;
    ps.visible_mask = mask;
    ps.visible_steps.clear();
    if (!state.last_plan) {
        return;
    }
    const auto& steps = state.last_plan->steps;
    ps.visible_steps.reserve(steps.size());
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const int family = static_cast<int>(family_of(steps[i].op.kind));
        if ((mask & (1U << family)) != 0) {
            ps.visible_steps.push_back(static_cast<int>(i));
        }
    }
}

std::string step_detail(const stc::core::PlanStep& step) {
    std::string text;
    if (const auto* target = stc::core::find_target(step.target_id)) {
        text = stc::ui::to_utf8(target->display_name);
    }
    if (!step.op.value_name.empty()) {
        text += text.empty() ? "" : ", ";
        text += stc::ui::to_utf8(step.op.value_name);
        if (step.op.kind == OpKind::WriteRegistryString || step.op.kind == OpKind::VdfSetValue) {
            text += " = \"" + stc::ui::to_utf8(step.op.payload) + "\"";
        }
    }
    return text;
}

void draw_step_menu(AppState& state, const stc::core::PlanStep& step, bool running) {
    const std::string path = stc::ui::to_utf8(step.op.target);
    if (facet::MenuItem("Copy path", {.icon = facet::Icon("copy")})) {
        ImGui::SetClipboardText(path.c_str());
        facet::ShowToast("Path copied", {.tone = facet::Tone::Success});
    }
    const bool file_op = step.op.kind == OpKind::RemoveFile || step.op.kind == OpKind::RemoveTree;
    if (file_op) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(step.op.target), ec)) {
            if (facet::MenuItem("Show in Explorer", {.icon = facet::Icon("folder")})) {
                if (!stc::ui::open_in_explorer(std::filesystem::path(step.op.target))) {
                    facet::ShowToast("The path no longer exists", {.tone = facet::Tone::Warning});
                }
            }
        }
    }
    // Only kinds the ignore list can filter get an exclude entry (see core/dry_run.cpp).
    const bool registry_op = step.op.kind == OpKind::RemoveRegistryKey || step.op.kind == OpKind::RemoveRegistryValue ||
                             step.op.kind == OpKind::ClearRegistryValue;
    if (file_op || registry_op) {
        ImGui::Separator();
        ImGui::BeginDisabled(running);
        const char* label = file_op ? "Exclude this path from cleanups" : "Exclude this registry value from cleanups";
        if (facet::MenuItem(label, {.icon = facet::Icon("lock")})) {
            if (file_op) {
                state.ignore_list.preserved_paths.push_back(step.op.target);
            } else if (step.op.kind == OpKind::RemoveRegistryKey || step.op.value_name.empty()) {
                state.ignore_list.preserved_registry_values.push_back(step.op.target);
            } else {
                state.ignore_list.preserved_registry_values.push_back(step.op.target + L"::" + step.op.value_name);
            }
            state.ignore_list_changed();
            facet::ShowToast("Excluded from cleanups", {.tone = facet::Tone::Success, .description = path.c_str()});
        }
        ImGui::EndDisabled();
    }
}

void draw_step_row(AppState& state, const stc::core::PlanStep& step, bool running) {
    const facet::Theme& t = facet::T();
    const ImVec2 row_min = ImGui::GetItemRectMin();
    const ImVec2 row_max = ImGui::GetItemRectMax();
    const bool focused = ImGui::IsItemFocused();
    const bool hovered = ImGui::IsItemHovered();
    const std::string path = stc::ui::to_utf8(step.op.target);
    const std::string detail = step_detail(step);

    if (focused && !hovered) {
        // Keyboard focus on the row shows the full path, as hovering the cut path text does.
        facet::ItemTooltip("%s", path.c_str());
    }
    if (facet::BeginContextMenu("##step-menu")) {
        draw_step_menu(state, step, running);
        facet::EndContextMenu();
    }

    const float pad = facet::Px(t.spacing.sm);
    const float body_h = facet::MeasureText(facet::TextStyle::Body, "Ag").y;
    const float badge_h = facet::ControlHeight(facet::ControlSize::Small);
    const float inner = body_h + facet::Px(t.spacing.xs) + badge_h;
    const float top = row_min.y + std::max(0.0F, (row_max.y - row_min.y - inner) * 0.5F);
    const float left = row_min.x + pad;
    const float right = row_max.x - pad;

    std::string size_text;
    if ((step.op.kind == OpKind::RemoveFile || step.op.kind == OpKind::RemoveTree) && step.op.size_bytes > 0) {
        size_text = stc::ui::format_bytes(step.op.size_bytes);
    }
    const float size_w = size_text.empty() ? 0.0F : facet::MeasureText(facet::TextStyle::Body, size_text.c_str()).x;
    const float path_w = std::max(1.0F, right - left - (size_w > 0.0F ? size_w + facet::Px(t.spacing.md) : 0.0F));

    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(left, top));
    facet::TruncatedText(path.c_str(), {.mode = facet::TruncateMode::Middle, .width = units(path_w)});
    if (!size_text.empty()) {
        reset_line_baseline();
        ImGui::SetCursorScreenPos(ImVec2(right - size_w, top));
        facet::StyledText(size_text.c_str(), {.secondary = true});
    }

    const float line2 = top + body_h + facet::Px(t.spacing.xs);
    reset_line_baseline();
    ImGui::SetCursorScreenPos(ImVec2(left, line2));
    facet::Badge(kind_badge(step.op.kind));
    const float badge_right = ImGui::GetItemRectMax().x;
    const float badge_rect_h = ImGui::GetItemRectSize().y;
    if (!detail.empty()) {
        const float foot_h = facet::MeasureText(facet::TextStyle::Footnote, "Ag").y;
        const float x = badge_right + facet::Px(t.spacing.sm);
        reset_line_baseline();
        ImGui::SetCursorScreenPos(ImVec2(x, line2 + (badge_rect_h - foot_h) * 0.5F));
        facet::TruncatedText(detail.c_str(), {.style = facet::TextStyle::Footnote,
                                              .secondary = true,
                                              .width = units(std::max(1.0F, right - x))});
    }
}

}  // namespace

void draw_details_drawer(AppState& state, PageState& ps) {
    // Nothing to list without a plan: the drawer closes (a cleanup or a new selection replaced it).
    if (ps.details_open && (!state.last_plan || state.last_plan->steps.empty() || ps.dev.nothing)) {
        ps.details_open = false;
    }
    const facet::Theme& t = facet::T();
    const std::uint64_t count = state.last_plan ? state.last_plan->steps.size() : 0;
    const std::string title = items_text(count) + "###plan-details";
    const float display_w = units(ImGui::GetMainViewport()->Size.x);
    const float extent = std::max(320.0F, std::min(kDrawerUnits, display_w - kDrawerMarginUnits));
    if (!facet::BeginDrawer(title.c_str(), &ps.details_open, {.extent = extent})) {
        return;
    }
    const auto& plan = *state.last_plan;
    const bool running = run_active(state, ps);
    const float width = ImGui::GetContentRegionAvail().x;

    char line[160];
    std::snprintf(line, sizeof(line), "%s in %llu files. Right-click an item to copy its path, show it in Explorer "
                                      "or exclude it from cleanups.",
                  stc::ui::format_bytes(plan.total_bytes).c_str(),
                  static_cast<unsigned long long>(plan.total_file_count));
    facet::StyledText(line, {.secondary = true, .wrap = true, .width = units(width)});
    gap(t.spacing.md);

    // Filter chips, one per kind family present in the plan.
    std::array<int, kFamilyCount> counts{};
    for (const auto& step : plan.steps) {
        ++counts[static_cast<std::size_t>(family_of(step.op.kind))];
    }
    std::array<facet::ChoiceItem, kFamilyCount> chips{};
    std::array<std::array<char, 16>, kFamilyCount> details{};
    std::array<bool, kFamilyCount> chip_on{};
    std::array<int, kFamilyCount> chip_family{};
    int chip_count = 0;
    for (int f = 0; f < kFamilyCount; ++f) {
        if (counts[static_cast<std::size_t>(f)] == 0) {
            continue;
        }
        auto& d = details[static_cast<std::size_t>(chip_count)];
        std::snprintf(d.data(), d.size(), "%d", counts[static_cast<std::size_t>(f)]);
        chips[static_cast<std::size_t>(chip_count)] = {.label = family_label(static_cast<KindFamily>(f)),
                                                       .detail = d.data()};
        chip_on[static_cast<std::size_t>(chip_count)] = ps.family_on[static_cast<std::size_t>(f)];
        chip_family[static_cast<std::size_t>(chip_count)] = f;
        ++chip_count;
    }
    const auto n = static_cast<std::size_t>(chip_count);
    if (facet::ChipGroup("##kinds", std::span<bool>(chip_on.data(), n),
                         std::span<const facet::ChoiceItem>(chips.data(), n), {.width = units(width)})) {
        for (std::size_t i = 0; i < n; ++i) {
            ps.family_on[static_cast<std::size_t>(chip_family[i])] = chip_on[i];
        }
    }
    gap(t.spacing.md);

    rebuild_visible_steps(state, ps);
    const float list_h = std::max(facet::Px(kStepRowUnits * 2.0F), ImGui::GetContentRegionAvail().y);
    if (facet::BeginVirtualList("##steps", static_cast<int>(ps.visible_steps.size()), nullptr,
                                {.rowHeight = kStepRowUnits,
                                 .emptyText = "No items of the chosen kinds. Turn a filter chip back on.",
                                 .height = units(list_h)})) {
        facet::RowRange rows;
        while (facet::NextVisibleRows(&rows)) {
            for (int row = rows.first; row < rows.last; ++row) {
                const int index = ps.visible_steps[static_cast<std::size_t>(row)];
                ImGui::PushID(index);
                facet::VirtualListRow(row);
                draw_step_row(state, plan.steps[static_cast<std::size_t>(index)], running);
                ImGui::PopID();
            }
        }
        facet::EndVirtualList();
    }
    facet::EndDrawer();
}

}  // namespace stc::ui::pages::cleanup
