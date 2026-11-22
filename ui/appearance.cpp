#include "ui/appearance.hpp"

#include <facet/facet.h>

#include <cstdint>

#include "app/app_state.hpp"

namespace stc::ui {
namespace {

struct Applied {
    bool valid = false;
    stc::app::ThemeChoice mode = stc::app::ThemeChoice::System;
    std::uint32_t accent = 0;
    float scale = 0.0F;
};

Applied g_applied;

facet::ThemeMode to_facet(stc::app::ThemeChoice choice) {
    switch (choice) {
        case stc::app::ThemeChoice::Light: return facet::ThemeMode::Light;
        case stc::app::ThemeChoice::Dark: return facet::ThemeMode::Dark;
        case stc::app::ThemeChoice::System: break;
    }
    return facet::ThemeMode::System;
}

ImVec4 accent_color(std::uint32_t rgb) {
    return ImVec4(static_cast<float>((rgb >> 16U) & 0xFFU) / 255.0F, static_cast<float>((rgb >> 8U) & 0xFFU) / 255.0F,
                  static_cast<float>(rgb & 0xFFU) / 255.0F, 1.0F);
}

}  // namespace

void sync_appearance(const stc::app::AppState& state, bool animate) {
    bool changed = false;
    // The accent first: SetThemeMode applies the accent of the last SetAccent to its preset.
    if (!g_applied.valid || g_applied.accent != state.accent_rgb) {
        facet::SetAccent(accent_color(state.accent_rgb));
        changed = true;
    }
    if (!g_applied.valid || g_applied.mode != state.theme_mode || changed) {
        facet::SetThemeMode(to_facet(state.theme_mode), animate && g_applied.valid);
        changed = true;
    }
    if (!g_applied.valid || g_applied.scale != state.ui_scale) {
        facet::SetUiScale(state.ui_scale);
        changed = true;
    }
    if (facet::IncreasedContrastEnabled() != state.increase_contrast) {
        facet::SetIncreasedContrast(state.increase_contrast);
        changed = true;
    }
    if (changed) {
        facet::ApplyImGuiStyle();
    }
    g_applied = {true, state.theme_mode, state.accent_rgb, state.ui_scale};
}

void reset_appearance() { g_applied = {}; }

}  // namespace stc::ui
