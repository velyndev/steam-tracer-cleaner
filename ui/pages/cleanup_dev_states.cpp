// CAPTURE-ONLY. Renders Cleanup page states that would otherwise need a destructive or slow
// action, from fabricated values, when the dev capture mode passes --state <name>
// (AppState::dev_state). Nothing here runs a cleanup, closes Steam or writes a file: it only sets
// page flags, the ticked targets and a made-up CleanResult. In normal runs dev_state is empty and
// apply_dev_state returns at once.
//
// States:
//   empty              nothing ticked
//   modified           Quick Clean with one target unticked (Modified pill, Reset to profile)
//   full-wipe          Full Wipe selected (warning alert)
//   game-reset         Game Reset without a game (summary asks for games)
//   game-reset-games   Game Reset with Counter-Strike 2 and Team Fortress 2 chosen
//   plan-loading       the plan is being measured (skeleton in the summary and the share column)
//   plan-error         the plan could not be built ("Try again")
//   nothing            the plan is empty ("Already clean")
//   category-open      the Caches and Sign-in categories opened
//   search             the search "cache" (matching categories open)
//   search-empty       a search without a match ("Clear search")
//   details            the details drawer open
//   confirm            the confirm dialog of a cleanup without backup
//   confirm-wipe       the confirm dialog of a Full Wipe with backup
//   running            a cleanup at 56 percent ("Cleaning N of M")
//   running-closing    a cleanup closing Steam
//   result-ok          a finished cleanup with backup
//   result-partial     a cleanup where three steps failed, failure messages open
//   result-error       a cleanup job that stopped with an error
//   scrolled           two categories open, the page scrolled to the end (the summary card stays
//                      at the top edge)

#include <algorithm>
#include <string>

#include "app/app_state.hpp"
#include "core/dry_run.hpp"
#include "ui/pages/cleanup_state.hpp"

namespace stc::ui::pages::cleanup {
namespace {

using stc::app::AppState;

void select_profile_where(AppState& state, bool (*match)(const stc::core::Profile&)) {
    for (std::size_t i = 0; i < state.profiles.size(); ++i) {
        if (match(state.profiles[i])) {
            state.selected_profile_index = static_cast<int>(i);
            apply_profile(state);
            return;
        }
    }
}

bool plan_ready(const AppState& state, PlanStatus status) {
    return status == PlanStatus::Ready && state.last_plan && !state.last_plan->steps.empty();
}

// A result made from the current plan, as execute() would report it.
stc::core::CleanResult fabricated_result(const stc::core::Plan& plan, int failures) {
    stc::core::CleanResult result;
    for (const auto& step : plan.steps) {
        const bool fail = failures > 0 && step.op.kind == stc::core::OpKind::RemoveFile;
        if (fail) {
            --failures;
            ++result.failed;
            result.failure_messages.push_back(L"DeleteFile failed: " + step.op.target);
        } else {
            ++result.succeeded;
            result.bytes_freed += step.op.size_bytes;
        }
    }
    return result;
}

}  // namespace

void apply_dev_state(AppState& state, PageState& ps, PlanStatus status) {
    const std::string& s = state.dev_state;
    if (s.empty()) {
        return;
    }

    // Per-frame overrides.
    ps.dev.plan_loading = s == "plan-loading";
    ps.dev.plan_error = s == "plan-error";
    ps.dev.nothing = s == "nothing";
    ps.dev.running = s == "running" || s == "running-closing";
    if (ps.dev.running) {
        ps.phase->store(static_cast<int>(s == "running" ? RunPhase::Cleaning : RunPhase::ClosingSteam));
        ps.dev.total = state.last_plan ? state.last_plan->steps.size() : 0;
        ps.dev.done = ps.dev.total * 56 / 100;
    }
    if (ps.dev.applied) {
        return;
    }

    // One-time setups. Those that need numbers wait until the fixture's plan is ready.
    if (s == "empty") {
        state.selected_target_ids.clear();
        ps.dev.applied = true;
    } else if (s == "modified") {
        if (!state.selected_target_ids.empty()) {
            state.selected_target_ids.erase(state.selected_target_ids.begin());
        }
        ps.dev.applied = true;
    } else if (s == "full-wipe") {
        select_profile_where(state, [](const stc::core::Profile& p) { return p.requires_confirmation; });
        ps.dev.applied = true;
    } else if (s == "game-reset" || s == "game-reset-games") {
        select_profile_where(state, [](const stc::core::Profile& p) { return p.name == L"Game Reset"; });
        state.selected_appids.clear();
        if (s == "game-reset-games") {
            state.selected_appids = {730, 440};
        }
        ps.appids_synced = false;
        ps.dev.applied = true;
    } else if (s == "category-open") {
        ps.category_open[0] = true;  // Caches
        ps.category_open[4] = true;  // Sign-in and account data
        ps.dev.applied = true;
    } else if (s == "search") {
        ps.search = "cache";
        ps.dev.applied = true;
    } else if (s == "search-empty") {
        ps.search = "minecraft";
        ps.dev.applied = true;
    } else if (s == "details") {
        if (plan_ready(state, status)) {
            ps.details_open = true;
            ps.dev.applied = true;
        }
    } else if (s == "confirm") {
        if (plan_ready(state, status)) {
            ps.backup_first = false;
            ps.confirm_open = true;
            ps.dev.applied = true;
        }
    } else if (s == "confirm-wipe") {
        if (!state.profiles.empty() && !is_full_wipe(state)) {
            select_profile_where(state, [](const stc::core::Profile& p) { return p.requires_confirmation; });
        } else if (plan_ready(state, status)) {
            ps.backup_first = true;
            ps.confirm_open = true;
            ps.dev.applied = true;
        }
    } else if (s == "result-ok" || s == "result-partial") {
        if (plan_ready(state, status)) {
            state.last_result = fabricated_result(*state.last_plan, s == "result-partial" ? 3 : 0);
            ps.last_backup_made = true;
            ps.last_planned_steps = state.last_plan->steps.size();
            ps.result_open = true;
            ps.failures_open = s == "result-partial";
            ps.dev.applied = true;
        }
    } else if (s == "result-error") {
        ps.run_error = "The backup folder could not be created, so nothing was removed.";
        ps.result_open = true;
        ps.dev.applied = true;
    } else if (s == "scrolled") {
        ps.category_open[0] = true;  // enough rows to scroll past the summary card
        ps.category_open[4] = true;
        ps.dev.scrolled = true;
        ps.dev.applied = true;
    } else {
        ps.dev.applied = true;  // running states, or a state of another page
    }
}

}  // namespace stc::ui::pages::cleanup
