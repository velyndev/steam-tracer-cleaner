#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app/task_runner.hpp"
#include "core/cleaner.hpp"
#include "core/config_library.hpp"
#include "core/dry_run.hpp"
#include "core/ignore_list.hpp"
#include "core/profile.hpp"
#include "core/steam_paths.hpp"
#include "core/update_check.hpp"

namespace stc::app {

// The pages of the sidebar, in sidebar order.
enum class Page { Cleanup, Backups, Exclusions, Audit, Configs, Settings };
inline constexpr int kPageCount = 6;

// The appearance setting; System follows the Windows app mode.
enum class ThemeChoice { System, Light, Dark };

inline constexpr std::uint32_t kDefaultAccentRgb = 0x4FB0FF;  // 0xRRGGBB
inline constexpr float kMinUiScale = 0.75F;
inline constexpr float kMaxUiScale = 2.0F;

struct AppState {
    std::filesystem::path config_dir;     // ignore.json, settings.json, ui.ini
    std::filesystem::path data_dir;       // logs/, backups/, configs/
    std::filesystem::path backups_dir;
    std::filesystem::path configs_dir;
    std::filesystem::path log_dir;
    bool portable_mode = false;

    // Set before initialize(). read_only: nothing is written to config_dir or data_dir (no
    // settings, ignore list or directories); every save path checks it. offline: no update check.
    // simulate_no_steam: refresh_steam() reports no install (capture of the empty states).
    bool read_only = false;
    bool offline = false;
    bool simulate_no_steam = false;
    // Empty in normal runs; set only by --state <name> in the dev capture mode. Pages may switch on
    // it to render a state that would otherwise need a destructive or slow action, for example
    // "running", "result-ok", "result-failed", "confirm", "error". Nothing else reads it.
    std::string dev_state;
    // Empty in normal runs; set only by --fixture <dir> in the dev modes. initialize() then takes
    // config_dir and data_dir from <dir>/app, and refresh_steam() the install from <dir>/Steam
    // (with htmlcache and local.vdf under <dir>/LocalSteam) instead of the registry and the
    // user's profile. See app/fixture.hpp.
    std::filesystem::path fixture_root;

    std::optional<stc::core::steam::InstallInfo> install;
    std::vector<stc::core::steam::AccountInfo> accounts;
    std::vector<std::filesystem::path> libraries;

    stc::core::IgnoreList ignore_list;
    // Bumped by ignore_list_changed(); pages that derive data from the ignore list (the cleanup
    // plan) compare it with the revision they last built from.
    std::uint64_t ignore_revision = 0;
    std::vector<stc::core::Profile> profiles;

    Page current_page = Page::Cleanup;

    // Cleanup page selection state
    int selected_profile_index = 0;
    std::set<std::string> selected_target_ids;
    std::set<std::uint32_t> selected_appids;        // Game Reset profile
    std::optional<stc::core::Plan> last_plan;
    std::optional<stc::core::CleanResult> last_result;

    // Configs page selection state
    std::filesystem::path autoexec_source;
    std::uint32_t autoexec_target_appid = 730;       // CS2 default
    std::string autoexec_dest_filename = "autoexec.cfg";
    std::filesystem::path video_config_source;
    std::set<std::uint32_t> video_config_targets;

    // Config library
    std::vector<stc::core::config_library::LibraryEntry> autoexec_library;
    std::vector<stc::core::config_library::LibraryEntry> video_library;
    int selected_autoexec_idx = -1;
    int selected_video_idx = -1;

    // Settings (settings.json)
    bool backup_by_default = true;
    bool confirm_full_wipe = true;
    int backup_keep_count = 10;       // auto-prune older backups; 0 means keep all
    bool check_updates_on_launch = true;
    std::string version_check_skip_until;
    ThemeChoice theme_mode = ThemeChoice::System;
    std::uint32_t accent_rgb = kDefaultAccentRgb;
    float ui_scale = 1.0F;
    bool increase_contrast = false;   // Facet's increased contrast variant (SetIncreasedContrast)

    // Update check result (written by the update job's completion on the UI thread).
    std::optional<stc::core::update_check::Result> update_result;
    bool update_modal_dismissed_this_session = false;

    // Set by the host when the window was asked to close while a must-finish job runs; the app
    // exits once that job is done.
    bool close_pending = false;

    void initialize();
    void refresh_steam();
    void refresh_config_library();
    void load_settings();
    void save_settings();
    void load_app_settings();
    void save_app_settings();
    void start_update_check();
    // Call after every edit of ignore_list: bumps ignore_revision and saves ignore.json.
    void ignore_list_changed();

    // Declared last so it is destroyed first: its shutdown joins the workers while every other
    // member still exists.
    TaskRunner tasks;
};

// The data directory of a normal run: "data" beside the exe when portable.flag is there,
// %LOCALAPPDATA%\steam-tracer-cleaner otherwise. The host starts the log in its logs/ folder
// before AppState exists, so the log is where AppState::log_dir says it is in both modes.
std::filesystem::path default_data_dir();

// "#RRGGBB" for 0xRRGGBB, and the reverse (false on malformed text).
std::string accent_to_hex(std::uint32_t rgb);
bool accent_from_hex(const std::string& text, std::uint32_t* rgb);

}  // namespace stc::app
