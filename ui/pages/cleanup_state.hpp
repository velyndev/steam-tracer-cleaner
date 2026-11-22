#pragma once

// State and non-visual parts of the Cleanup page, shared by its files:
//   cleanup_page.cpp        the page: layout, profile cards, Full Wipe and Game Reset blocks, confirm
//   cleanup_summary.cpp     the summary card (every plan and run state, the primary action)
//   cleanup_targets.cpp     "What gets removed": category rows, targets, search
//   cleanup_details.cpp     the details drawer with every step of the plan
//   cleanup_view.hpp        drawing helpers and the entry points of the files above
//   cleanup_jobs.cpp        the live plan job and the cleanup job
//   cleanup_dev_states.cpp  capture-only states (state.dev_state), never used in normal runs

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/cleaner.hpp"
#include "core/operation.hpp"

namespace stc::app {
struct AppState;
}

namespace stc::ui::pages::cleanup {

// Where the plan stands for the current inputs.
enum class PlanStatus {
    NothingTicked,  // no target ticked
    NeedGames,      // Game Reset without a chosen game (an empty AppID filter would mean every game)
    Building,       // a build for the current inputs is waiting or running
    Ready,          // state.last_plan matches the current inputs
    Failed,         // the build for the current inputs threw
};

// What the cleanup job is doing; written by the worker, read by the UI.
enum class RunPhase : int { Idle = 0, ClosingSteam = 1, Backup = 2, Cleaning = 3 };

// Kind families of the plan filter chips.
enum class KindFamily { Files, Folders, Registry, Vdf, Cookies };
inline constexpr int kFamilyCount = 5;

// The category rows of "What gets removed" (every TargetCategory except Hwid).
inline constexpr int kCategoryCount = 7;

KindFamily family_of(stc::core::OpKind kind);
const char* family_label(KindFamily family);
// The short neutral badge text of an operation kind ("File", "Reg value").
const char* kind_badge(stc::core::OpKind kind);

struct PageState {
    bool initialized = false;

    // Live plan. A signature string describes the inputs (targets, AppIDs, ignore revision,
    // install); the plan is current when the applied signature equals the wanted one.
    std::string wanted_sig;
    std::string submitted_sig;
    std::string applied_sig;
    std::string failed_sig;
    std::string plan_error;
    std::uint64_t generation = 0;  // bumped per submitted build; older results are dropped
    double changed_at = 0.0;       // ImGui time of the last input change (debounce)
    std::uint64_t plan_version = 0;  // bumped whenever state.last_plan is replaced

    // Plan view filter and the step indices it lets through.
    std::array<bool, kFamilyCount> family_on{true, true, true, true, true};
    std::vector<int> visible_steps;
    std::uint64_t visible_version = ~0ULL;
    unsigned visible_mask = ~0U;

    // What each target contributes to state.last_plan, rebuilt when plan_version changes.
    struct Share {
        std::uint64_t bytes = 0;
        int steps = 0;
    };
    std::unordered_map<std::string, Share> shares;
    std::uint64_t shares_version = ~0ULL;

    // "What gets removed": the filter and the open state of each category row.
    std::string search;
    std::array<bool, kCategoryCount> category_open{};
    std::vector<std::string> other_appids;  // Game Reset tags that are not known games
    std::string appid_error;
    bool appids_synced = false;

    // The details drawer (every step of the plan).
    bool details_open = false;

    // Summary card and run.
    bool backup_first = true;
    bool seen_backup_default = true;
    bool confirm_open = false;
    std::shared_ptr<std::atomic<int>> phase = std::make_shared<std::atomic<int>>(0);
    bool result_open = false;
    bool failures_open = false;
    bool last_backup_made = false;
    std::uint64_t last_planned_steps = 0;
    std::string run_error;  // the cleanup job threw (nothing or part of the plan ran)

    // Capture-only overrides (cleanup_dev_states.cpp). All false in normal runs.
    struct Dev {
        bool applied = false;
        bool plan_loading = false;
        bool plan_error = false;
        bool nothing = false;   // the plan is shown as empty ("Already clean")
        bool scrolled = false;  // the page body is scrolled to its end
        bool running = false;
        std::uint64_t done = 0;
        std::uint64_t total = 0;
    } dev;
};

PageState& page_state();

// Inputs and status --------------------------------------------------------------------------------

bool is_game_reset(const stc::app::AppState& state);
bool is_full_wipe(const stc::app::AppState& state);
// Sets the ticked targets to the selected profile's set.
void apply_profile(stc::app::AppState& state);
bool profile_modified(const stc::app::AppState& state);

// Updates the wanted signature, submits a plan build after the debounce and returns the status.
PlanStatus update_plan(stc::app::AppState& state, PageState& ps);
// Drops the current plan and builds again at once (after a cleanup, and for "Try again").
void rebuild_plan_now(stc::app::AppState& state, PageState& ps);

// Run -----------------------------------------------------------------------------------------------

// True while the cleanup job runs (or a capture shows the running state).
bool run_active(const stc::app::AppState& state, const PageState& ps);
// Progress of the running cleanup: phase, steps done and total.
struct RunProgress {
    RunPhase phase = RunPhase::Idle;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
};
RunProgress run_progress(const stc::app::AppState& state, const PageState& ps);
// Starts the cleanup job for state.last_plan. Refused (with a toast) in read-only dev modes.
void start_cleanup(stc::app::AppState& state, PageState& ps, bool with_backup);

// Capture-only --------------------------------------------------------------------------------------

// Applies state.dev_state to the page once; does nothing when it is empty. Called every frame
// after the plan status is known, so states that need a plan wait for one.
void apply_dev_state(stc::app::AppState& state, PageState& ps, PlanStatus status);

}  // namespace stc::ui::pages::cleanup
