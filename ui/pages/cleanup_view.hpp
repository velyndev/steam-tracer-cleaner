#pragma once

// Drawing helpers shared by the Cleanup page files, and the entry points of each part of the page.
// See cleanup_state.hpp for which file draws what.

#include <cstdint>
#include <string>

#include "core/targets.hpp"
#include "ui/pages/cleanup_state.hpp"

namespace stc::app {
struct AppState;
}

namespace stc::ui::pages::cleanup {

// --- Helpers ---------------------------------------------------------------------------------------

// Pixels to design units.
float units(float pixels);
// Makes the distance between the previous item and the next one exactly `design_units`.
void gap(float design_units);
// Clears the text baseline offset a list row leaves on its line, for items placed with
// SetCursorScreenPos on that line.
void reset_line_baseline();
// "1 item", "165 items".
std::string items_text(std::uint64_t n);
std::string lower(std::string s);

struct AccountCounts {
    int total = 0;
    int excluded = 0;
};
AccountCounts account_counts(const stc::app::AppState& state);

// Recomputes ps.shares from state.last_plan when the plan changed.
void update_shares(const stc::app::AppState& state, PageState& ps);
// True when the summary may show plan numbers: the plan matches the inputs (or a capture asks
// for a ready plan).
bool plan_is_current(PlanStatus status);

// Asks for the confirm dialog or starts the cleanup at once (see draw_confirm).
void request_run(stc::app::AppState& state, PageState& ps);

// --- Parts -----------------------------------------------------------------------------------------

// The summary card at the current cursor, `width_px` wide.
void draw_summary_card(stc::app::AppState& state, PageState& ps, PlanStatus status, float width_px);
// "What gets removed": heading, search field and the card of category rows.
void draw_targets_section(stc::app::AppState& state, PageState& ps, PlanStatus status, float width_px);
// The details drawer; call every frame (it shows while ps.details_open).
void draw_details_drawer(stc::app::AppState& state, PageState& ps);

}  // namespace stc::ui::pages::cleanup
