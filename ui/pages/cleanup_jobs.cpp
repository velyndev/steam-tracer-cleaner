#include "ui/pages/cleanup_state.hpp"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <facet/facet.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/app_state.hpp"
#include "core/backup.hpp"
#include "core/cleaner.hpp"
#include "core/dry_run.hpp"
#include "core/steam_lifecycle.hpp"
#include "ui/common.hpp"

namespace stc::ui::pages::cleanup {
namespace {

using stc::app::AppState;
using stc::app::JobContext;
using stc::app::JobKind;

// Time between the last input change and the plan build, so ticking several boxes in a row
// starts one build instead of one per box.
constexpr double kDebounceSeconds = 0.25;

const stc::core::Profile* selected_profile(const AppState& state) {
    if (state.profiles.empty()) {
        return nullptr;
    }
    const int index = std::clamp(state.selected_profile_index, 0, static_cast<int>(state.profiles.size()) - 1);
    return &state.profiles[static_cast<std::size_t>(index)];
}

std::string plan_signature(const AppState& state, bool game_reset) {
    std::string sig = stc::ui::path_to_utf8(state.install->install_path);
    sig += '|';
    sig += std::to_string(state.accounts.size());
    sig += '|';
    sig += std::to_string(state.libraries.size());
    sig += '|';
    sig += std::to_string(state.ignore_revision);
    sig += '|';
    for (const auto& id : state.selected_target_ids) {
        sig += id;
        sig += ',';
    }
    if (game_reset) {
        sig += "|games:";
        for (const auto appid : state.selected_appids) {
            sig += std::to_string(appid);
            sig += ',';
        }
    }
    return sig;
}

void submit_plan(AppState& state, PageState& ps, bool game_reset) {
    const std::uint64_t generation = ++ps.generation;
    ps.submitted_sig = ps.wanted_sig;

    // Copies for the worker; it never touches AppState.
    auto install = *state.install;
    auto accounts = state.accounts;
    auto libraries = state.libraries;
    auto ignore = state.ignore_list;
    std::vector<std::string> ids(state.selected_target_ids.begin(), state.selected_target_ids.end());
    std::vector<std::uint32_t> appids;
    if (game_reset) {
        appids.assign(state.selected_appids.begin(), state.selected_appids.end());
    }
    const std::string sig = ps.wanted_sig;

    state.tasks.submit<stc::core::Plan>(
        JobKind::PlanBuild, "Measuring the cleanup plan",
        [install = std::move(install), accounts = std::move(accounts), libraries = std::move(libraries),
         ignore = std::move(ignore), ids = std::move(ids), appids = std::move(appids)](JobContext&) {
            const stc::core::ResolveContext ctx{install, accounts, libraries};
            stc::core::PlanOptions options;
            options.ignore = &ignore;
            options.only_appids = appids;
            return stc::core::build_plan_by_ids(ids, ctx, options);
        },
        [&state, &ps, generation, sig](stc::core::Plan plan) {
            if (generation != ps.generation || sig != ps.wanted_sig) {
                return;  // inputs changed while it ran; the current plan or a newer build stands
            }
            state.last_plan = std::move(plan);
            ps.applied_sig = sig;
            ps.failed_sig.clear();
            ps.plan_error.clear();
            ++ps.plan_version;
        },
        [&ps, generation, sig](const std::string& error) {
            if (generation != ps.generation || sig != ps.wanted_sig) {
                return;
            }
            ps.failed_sig = sig;
            ps.plan_error = error;
        });
}

struct CleanOutcome {
    stc::core::CleanResult result;
    bool backup_made = false;
    std::size_t pruned = 0;
};

}  // namespace

PageState& page_state() {
    static PageState state;
    return state;
}

KindFamily family_of(stc::core::OpKind kind) {
    using stc::core::OpKind;
    switch (kind) {
        case OpKind::RemoveFile: return KindFamily::Files;
        case OpKind::RemoveTree: return KindFamily::Folders;
        case OpKind::RemoveRegistryValue:
        case OpKind::RemoveRegistryKey:
        case OpKind::ClearRegistryValue:
        case OpKind::WriteRegistryString: return KindFamily::Registry;
        case OpKind::VdfRemoveChild:
        case OpKind::VdfSetValue: return KindFamily::Vdf;
        case OpKind::ClearBrowserSteamCookies: return KindFamily::Cookies;
    }
    return KindFamily::Files;
}

const char* family_label(KindFamily family) {
    switch (family) {
        case KindFamily::Files: return "Files";
        case KindFamily::Folders: return "Folders";
        case KindFamily::Registry: return "Registry";
        case KindFamily::Vdf: return "Settings file edits";
        case KindFamily::Cookies: return "Browser cookies";
    }
    return "";
}

const char* kind_badge(stc::core::OpKind kind) {
    using stc::core::OpKind;
    switch (kind) {
        case OpKind::RemoveFile: return "File";
        case OpKind::RemoveTree: return "Folder";
        case OpKind::RemoveRegistryValue: return "Registry value";
        case OpKind::RemoveRegistryKey: return "Registry key";
        case OpKind::ClearRegistryValue: return "Clear registry value";
        case OpKind::WriteRegistryString: return "Set registry value";
        case OpKind::VdfRemoveChild: return "Remove entry";
        case OpKind::VdfSetValue: return "Set entry";
        case OpKind::ClearBrowserSteamCookies: return "Cookies";
    }
    return "";
}

bool is_game_reset(const AppState& state) {
    const auto* profile = selected_profile(state);
    return profile != nullptr && profile->name == L"Game Reset";
}

bool is_full_wipe(const AppState& state) {
    const auto* profile = selected_profile(state);
    return profile != nullptr && profile->requires_confirmation;
}

void apply_profile(AppState& state) {
    const auto* profile = selected_profile(state);
    if (profile == nullptr) {
        return;
    }
    state.selected_target_ids.clear();
    state.selected_target_ids.insert(profile->target_ids.begin(), profile->target_ids.end());
}

bool profile_modified(const AppState& state) {
    const auto* profile = selected_profile(state);
    if (profile == nullptr) {
        return false;
    }
    if (profile->target_ids.size() != state.selected_target_ids.size()) {
        return true;
    }
    return std::any_of(profile->target_ids.begin(), profile->target_ids.end(),
                       [&](const std::string& id) { return !state.selected_target_ids.contains(id); });
}

PlanStatus update_plan(AppState& state, PageState& ps) {
    const bool game_reset = is_game_reset(state);
    const double now = ImGui::GetTime();
    std::string sig = plan_signature(state, game_reset);
    if (sig != ps.wanted_sig) {
        // The first signature builds at once; later changes wait for the debounce.
        ps.changed_at = ps.wanted_sig.empty() ? now - kDebounceSeconds : now;
        ps.wanted_sig = std::move(sig);
    }

    if (state.selected_target_ids.empty()) {
        return PlanStatus::NothingTicked;
    }
    if (game_reset && state.selected_appids.empty()) {
        return PlanStatus::NeedGames;
    }
    if (ps.applied_sig == ps.wanted_sig && state.last_plan) {
        return PlanStatus::Ready;
    }
    if (ps.failed_sig == ps.wanted_sig) {
        return PlanStatus::Failed;
    }
    // A cleanup changes the files on disk; no plan is built while it runs.
    if (ps.submitted_sig != ps.wanted_sig && now - ps.changed_at >= kDebounceSeconds &&
        !state.tasks.is_running(JobKind::Clean)) {
        submit_plan(state, ps, game_reset);
    }
    return PlanStatus::Building;
}

void rebuild_plan_now(AppState& state, PageState& ps) {
    state.last_plan.reset();
    ps.applied_sig.clear();
    ps.submitted_sig.clear();
    ps.failed_sig.clear();
    ps.plan_error.clear();
    ps.changed_at = ImGui::GetTime() - kDebounceSeconds;
    ++ps.plan_version;
}

bool run_active(const AppState& state, const PageState& ps) {
    return ps.dev.running || state.tasks.is_running(JobKind::Clean);
}

RunProgress run_progress(const AppState& state, const PageState& ps) {
    RunProgress progress;
    progress.phase = static_cast<RunPhase>(ps.phase->load(std::memory_order_relaxed));
    if (ps.dev.running) {
        progress.done = ps.dev.done;  // capture-only values (cleanup_dev_states.cpp)
        progress.total = ps.dev.total;
        return progress;
    }
    for (const auto& job : state.tasks.running()) {
        if (job.kind == JobKind::Clean) {
            progress.done = job.done;
            progress.total = job.total;
        }
    }
    return progress;
}

void start_cleanup(AppState& state, PageState& ps, bool with_backup) {
    // Safety net: the dev modes (capture, selftest) run read-only and must never clean.
    if (state.read_only || !state.dev_state.empty()) {
        facet::ShowToast("Cleaning is off in this read-only run", {.tone = facet::Tone::Warning});
        return;
    }
    if (!state.last_plan || state.tasks.is_running(JobKind::Clean)) {
        return;
    }

    auto plan = *state.last_plan;
    const auto backups_dir = state.backups_dir;
    const int keep = state.backup_keep_count;
    auto phase = ps.phase;
    phase->store(static_cast<int>(RunPhase::ClosingSteam), std::memory_order_relaxed);
    ps.last_planned_steps = plan.steps.size();
    ps.last_backup_made = false;
    ps.run_error.clear();
    state.last_result.reset();
    ps.result_open = false;
    ps.failures_open = false;
    spdlog::info("Cleanup started: {} steps, backup {}", plan.steps.size(), with_backup ? "on" : "off");

    state.tasks.submit<CleanOutcome>(
        JobKind::Clean, "Cleaning",
        [plan = std::move(plan), backups_dir, keep, with_backup, phase](JobContext& ctx) {
            ctx.set_total(plan.steps.size());

            // Steam rewrites loginusers.vdf and localconfig.vdf when it exits, so it goes first.
            const auto closed = stc::core::steam_lifecycle::close_steam_and_games();
            spdlog::info("Closed {} Steam and {} game processes", closed.steam_closed, closed.games_closed);

            std::optional<stc::core::backup::Session> session;
            if (with_backup) {
                phase->store(static_cast<int>(RunPhase::Backup), std::memory_order_relaxed);
                session = stc::core::backup::Session::create(backups_dir);
                if (!session) {
                    // Cleaning without the backup the user asked for would be irreversible.
                    throw std::runtime_error("The backup folder could not be created, so nothing was removed.");
                }
            }

            phase->store(static_cast<int>(RunPhase::Cleaning), std::memory_order_relaxed);
            stc::core::CleanOptions options;
            options.backup = session ? &*session : nullptr;
            options.on_progress = [&ctx](int done, int) { ctx.set_done(static_cast<std::uint64_t>(done)); };
            CleanOutcome outcome;
            // execute() finalizes the backup session (writes its manifest) after the last step.
            outcome.result = stc::core::execute(plan, options);
            spdlog::info("Cleaner finished: {} ok, {} failed, {} bytes freed", outcome.result.succeeded,
                         outcome.result.failed, outcome.result.bytes_freed);
            if (session) {
                outcome.backup_made = true;
                if (keep > 0) {
                    outcome.pruned = stc::core::backup::prune_backups(backups_dir, static_cast<std::size_t>(keep));
                }
            }
            return outcome;
        },
        [&state, &ps](CleanOutcome outcome) {
            ps.phase->store(static_cast<int>(RunPhase::Idle), std::memory_order_relaxed);
            ps.last_backup_made = outcome.backup_made;
            state.last_result = std::move(outcome.result);
            ps.result_open = true;
            rebuild_plan_now(state, ps);
            if (state.current_page != stc::app::Page::Cleanup) {
                const auto& r = *state.last_result;
                const std::string text = r.failed == 0 ? "Cleanup finished" : "Cleanup finished with errors";
                facet::ShowToast(text.c_str(), {.tone = r.failed == 0 ? facet::Tone::Success : facet::Tone::Warning});
            }
        },
        [&state, &ps](const std::string& error) {
            ps.phase->store(static_cast<int>(RunPhase::Idle), std::memory_order_relaxed);
            ps.run_error = error;
            ps.result_open = true;
            rebuild_plan_now(state, ps);
        },
        {.must_finish = true});
}

}  // namespace stc::ui::pages::cleanup
