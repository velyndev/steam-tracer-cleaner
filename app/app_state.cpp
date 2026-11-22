#include "app/app_state.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

#include "core/version.hpp"
#include "platform/paths.hpp"

namespace stc::app {
namespace {

bool portable_flag_present(const std::filesystem::path& exe_dir) {
    std::error_code ec;
    return std::filesystem::exists(exe_dir / "portable.flag", ec);
}

const char* theme_choice_name(ThemeChoice choice) {
    switch (choice) {
        case ThemeChoice::Light: return "light";
        case ThemeChoice::Dark: return "dark";
        case ThemeChoice::System: break;
    }
    return "system";
}

ThemeChoice theme_choice_from(const std::string& text) {
    if (text == "light") return ThemeChoice::Light;
    if (text == "dark") return ThemeChoice::Dark;
    return ThemeChoice::System;
}

}  // namespace

std::string accent_to_hex(std::uint32_t rgb) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(rgb & 0xFFFFFFU));
    return buf;
}

bool accent_from_hex(const std::string& text, std::uint32_t* rgb) {
    if (text.size() != 7 || text[0] != '#') {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4U) | digit;
    }
    *rgb = value;
    return true;
}

std::filesystem::path default_data_dir() {
    const auto exe_dir = stc::platform::exe_directory();
    if (portable_flag_present(exe_dir)) {
        return exe_dir / "data";
    }
    return stc::platform::local_appdata_dir() / "steam-tracer-cleaner";
}

void AppState::initialize() {
    auto exe_dir = stc::platform::exe_directory();
    portable_mode = portable_flag_present(exe_dir);

    if (!fixture_root.empty()) {
        portable_mode = false;
        config_dir = fixture_root / "app" / "config";
        data_dir = fixture_root / "app" / "data";
    } else if (portable_mode) {
        config_dir = exe_dir / "data";
        data_dir = default_data_dir();
    } else {
        config_dir = stc::platform::appdata_dir() / "steam-tracer-cleaner";
        data_dir = default_data_dir();
    }
    backups_dir = data_dir / "backups";
    configs_dir = data_dir / "configs";
    log_dir = data_dir / "logs";

    if (!read_only) {
        std::error_code ec;
        std::filesystem::create_directories(config_dir, ec);
        std::filesystem::create_directories(data_dir, ec);
        std::filesystem::create_directories(backups_dir, ec);
        std::filesystem::create_directories(configs_dir / "autoexec", ec);
        std::filesystem::create_directories(configs_dir / "video", ec);
    }

    load_settings();
    refresh_steam();
    refresh_config_library();

    profiles.assign(stc::core::built_in_profiles().begin(), stc::core::built_in_profiles().end());

    if (!profiles.empty()) {
        const auto& first = profiles.front();
        selected_target_ids.insert(first.target_ids.begin(), first.target_ids.end());
    }

    start_update_check();
}

void AppState::refresh_steam() {
    if (simulate_no_steam) {
        install = std::nullopt;
    } else if (!fixture_root.empty()) {
        // The synthetic install of --fixture, laid out like discover_install() reports a real one.
        stc::core::steam::InstallInfo info;
        info.install_path = fixture_root / "Steam";
        info.config_dir = info.install_path / "config";
        info.userdata_dir = info.install_path / "userdata";
        info.appcache_dir = info.install_path / "appcache";
        info.htmlcache_dir = fixture_root / "LocalSteam" / "htmlcache";
        info.local_vdf_path = fixture_root / "LocalSteam" / "local.vdf";
        install = std::move(info);
    } else {
        install = stc::core::steam::discover_install();
    }
    if (install) {
        libraries = stc::core::steam::discover_libraries(*install);
        accounts = stc::core::steam::enumerate_accounts(*install);
        spdlog::info("Steam found at {}, {} libraries, {} accounts",
                     install->install_path.string(), libraries.size(), accounts.size());
    } else {
        libraries.clear();
        accounts.clear();
        spdlog::warn(simulate_no_steam ? "Steam detection skipped (--no-steam)" : "Steam install not found");
    }
}

void AppState::refresh_config_library() {
    using CK = stc::core::config_library::ConfigKind;
    autoexec_library = stc::core::config_library::list_configs(configs_dir, CK::Autoexec);
    video_library = stc::core::config_library::list_configs(configs_dir, CK::Video);
}

void AppState::load_settings() {
    auto ignore = stc::core::load_ignore_list(config_dir / "ignore.json");
    if (ignore) {
        ignore_list = std::move(*ignore);
    } else {
        ignore_list = stc::core::default_ignore_list();
    }
    load_app_settings();
}

void AppState::save_settings() {
    if (read_only) {
        return;
    }
    stc::core::save_ignore_list(ignore_list, config_dir / "ignore.json");
    save_app_settings();
}

void AppState::ignore_list_changed() {
    ++ignore_revision;
    if (read_only) {
        return;
    }
    stc::core::save_ignore_list(ignore_list, config_dir / "ignore.json");
}

// One line per key in load_app_settings and in save_app_settings. A missing or malformed key keeps
// the default, so older settings.json files keep loading.
void AppState::load_app_settings() {
    std::ifstream f(config_dir / "settings.json");
    if (!f) {
        return;
    }
    try {
        nlohmann::json j;
        f >> j;
        check_updates_on_launch = j.value("check_updates_on_launch", check_updates_on_launch);
        version_check_skip_until = j.value("version_check_skip_until", version_check_skip_until);
        backup_by_default = j.value("backup_by_default", backup_by_default);
        confirm_full_wipe = j.value("confirm_full_wipe", confirm_full_wipe);
        backup_keep_count = j.value("backup_keep_count", backup_keep_count);
        theme_mode = theme_choice_from(j.value("theme_mode", std::string{theme_choice_name(theme_mode)}));
        if (!accent_from_hex(j.value("accent_color", accent_to_hex(accent_rgb)), &accent_rgb)) {
            accent_rgb = kDefaultAccentRgb;
        }
        ui_scale = j.value("ui_scale", ui_scale);
        increase_contrast = j.value("increase_contrast", increase_contrast);
    } catch (const std::exception& e) {
        spdlog::warn("settings.json parse failed: {}", e.what());
    }
    if (!std::isfinite(ui_scale)) {
        ui_scale = 1.0F;
    }
    ui_scale = std::clamp(ui_scale, kMinUiScale, kMaxUiScale);
}

void AppState::save_app_settings() {
    if (read_only) {
        return;
    }
    nlohmann::json j;
    j["check_updates_on_launch"] = check_updates_on_launch;
    j["version_check_skip_until"] = version_check_skip_until;
    j["backup_by_default"] = backup_by_default;
    j["confirm_full_wipe"] = confirm_full_wipe;
    j["backup_keep_count"] = backup_keep_count;
    j["theme_mode"] = theme_choice_name(theme_mode);
    j["accent_color"] = accent_to_hex(accent_rgb);
    j["ui_scale"] = ui_scale;
    j["increase_contrast"] = increase_contrast;

    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    std::ofstream f(config_dir / "settings.json", std::ios::trunc);
    if (!f) {
        spdlog::warn("settings.json: open for write failed");
        return;
    }
    f << j.dump(4);
}

void AppState::start_update_check() {
    if (!check_updates_on_launch || offline) {
        return;
    }
    using Result = std::optional<stc::core::update_check::Result>;
    tasks.submit<Result>(
        JobKind::UpdateCheck, "Checking for updates",
        [](JobContext& ctx) -> Result {
            if (ctx.stop_requested()) {
                return std::nullopt;
            }
            return stc::core::update_check::fetch_latest_release(stc::core::kRepoSlug, stc::core::kAppVersion);
        },
        [this](Result r) {
            if (r && r->newer_than_current) {
                update_result = std::move(r);
            }
        });
}

}  // namespace stc::app
