#pragma once

// Applies the appearance settings of AppState (theme mode, accent, UI scale) to Facet.
//
// The shell calls sync_appearance() at the start of every frame, before any window. It compares
// the settings with what it applied last and applies only what changed, so a page that edits
// state.theme_mode, state.accent_rgb or state.ui_scale needs no other call: the next frame shows
// the change (and the page saves settings.json itself with state.save_app_settings()).

namespace stc::app {
struct AppState;
}

namespace stc::ui {

// `animate` false switches at once (start-up, capture).
void sync_appearance(const stc::app::AppState& state, bool animate = true);

// Forgets what was applied, so the next sync applies everything again (after facet::Init on a
// new device, or between capture runs).
void reset_appearance();

}  // namespace stc::ui
