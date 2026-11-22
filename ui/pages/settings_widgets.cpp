#include "ui/pages/settings_widgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <facet/facet.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ui/common.hpp"

namespace stc::ui::pages::settings_widgets {

void align_to_control(float height_px) {
    const float control = facet::ControlHeight(facet::ControlSize::Inherit);
    const float offset = std::round((control - height_px) * 0.5F);
    if (offset > 0.0F) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
    }
}

namespace {

struct RowContent {
    bool active = false;
    float slot = 0.0F;
    ImVector<float> stack;
    float item_width = 0.0F;
};

RowContent g_row;

}  // namespace

void begin_row_content() {
    IM_ASSERT(!g_row.active && "row content does not nest");
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    g_row.active = true;
    g_row.slot = ImGui::CalcItemWidth();
    // Workaround (see the header): hide the slot's pushed width so widgets use their natural one.
    g_row.stack = window->DC.ItemWidthStack;
    g_row.item_width = window->DC.ItemWidth;
    window->DC.ItemWidthStack.clear();
    window->DC.ItemWidth = window->DC.ItemWidthDefault;
}

void end_row_content() {
    if (!g_row.active) {
        return;
    }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    window->DC.ItemWidthStack = g_row.stack;
    window->DC.ItemWidth = g_row.item_width;
    g_row.active = false;
}

float slot_width() { return g_row.active ? g_row.slot : ImGui::GetContentRegionAvail().x; }

float fit_width(float wanted) {
    const float room = g_row.active ? g_row.slot : ImGui::CalcItemWidth();
    return std::min(wanted, room / facet::Scale());
}

void align_text_to_control() { align_to_control(InlineLine::text_height()); }

InlineLine::InlineLine() : top_(ImGui::GetCursorPosY()) {}

float InlineLine::text_height() { return facet::MeasureText(facet::TextStyle::Body, "Ag").y; }

void InlineLine::next(float item_height_px, float gap_px) {
    if (!first_) {
        ImGui::SameLine(0.0F, gap_px >= 0.0F ? gap_px : facet::Px(facet::T().spacing.md));
    }
    first_ = false;
    const float control = facet::ControlHeight(facet::ControlSize::Inherit);
    const float height = item_height_px > 0.0F ? item_height_px : control;
    ImGui::SetCursorPosY(top_ + std::max(0.0F, std::round((control - height) * 0.5F)));
}

void path_value(const char* id, const std::filesystem::path& path, bool copy) {
    ImGui::PushID(id);
    const facet::Theme& t = facet::T();
    const std::string text = path_to_utf8(path);
    const float button = facet::ControlHeight(facet::ControlSize::Inherit);
    const float gap = facet::Px(t.spacing.xs);
    const float buttons = (copy ? 2.0F : 1.0F) * (button + gap);
    const float text_w = std::max(facet::Px(40.0F), slot_width() - buttons);

    InlineLine line;
    line.next_text();
    facet::TruncatedText(text.c_str(),
                         {.mode = facet::TruncateMode::Middle, .focusable = true, .width = text_w / facet::Scale()});
    if (copy) {
        line.next(0.0F, gap);
        facet::CopyButton("Copy path###copy", text.c_str());
    }
    line.next(0.0F, gap);
    if (facet::IconButton("Show in Explorer###show", facet::Icon("folder"))) {
        if (!open_in_explorer(path)) {
            facet::ShowToast("This folder does not exist yet", {.tone = facet::Tone::Warning,
                                                                .description = text.c_str()});
        }
    }
    ImGui::PopID();
}

}  // namespace stc::ui::pages::settings_widgets
