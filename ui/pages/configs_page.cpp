#include "ui/pages/configs_page.hpp"

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <facet/facet.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "app/app_state.hpp"
#include "core/appid_map.hpp"
#include "core/autoexec.hpp"
#include "core/config_library.hpp"
#include "core/steam_lifecycle.hpp"
#include "ui/common.hpp"
#include "ui/pages/settings_widgets.hpp"

// The Configs page: a library of config files per kind (autoexec, CS2 video settings), each tab a
// list and detail view. The list holds the library; the detail previews the selected file and
// installs it, on a background job, into a game (autoexec) or into accounts (video settings).
//
// Both tabs run through the same functions, parameterized by a KindSpec. Page state that AppState
// does not hold (the tab, list selection storage, previews, results, open dialogs) lives in g_page.

namespace stc::ui::pages {
namespace {

namespace fs = std::filesystem;
using stc::app::AppState;
using stc::app::JobKind;
using stc::core::config_library::ConfigKind;
using stc::core::config_library::LibraryEntry;
using stc::core::steam::AccountInfo;

constexpr std::uintmax_t kPreviewCapBytes = 64 * 1024;
constexpr std::uint32_t kCs2AppId = 730;
// From this many accounts the account list gets a filter field.
constexpr std::size_t kSearchableAccountsFrom = 7;

// --- Kinds ---------------------------------------------------------------------------------------

struct KindSpec {
    ConfigKind kind;
    int tab;
    const char* id;              // id prefix
    const wchar_t* filter_name;  // open dialog filter
    const wchar_t* filter_pattern;
    const wchar_t* dialog_title;
    const char* empty_title;
    const char* empty_message;
};

constexpr KindSpec kAutoexecSpec{
    ConfigKind::Autoexec,
    0,
    "autoexec",
    L"Config files (*.cfg)",
    L"*.cfg",
    L"Import a config file",
    "No autoexec configs yet",
    "Import a .cfg file to keep it here. You can then install it into any supported Source game, "
    "for example as the autoexec.cfg that runs at every launch.",
};

constexpr KindSpec kVideoSpec{
    ConfigKind::Video,
    1,
    "video",
    L"Video settings (*.txt)",
    L"*.txt",
    L"Import a CS2 video settings file",
    "No video settings files yet",
    "Import a cs2_video.txt to keep it here. You can then install it into the CS2 settings of "
    "one or more Steam accounts on this PC.",
};

std::vector<LibraryEntry>& library_of(AppState& state, const KindSpec& spec) {
    return spec.kind == ConfigKind::Autoexec ? state.autoexec_library : state.video_library;
}
int& selected_of(AppState& state, const KindSpec& spec) {
    return spec.kind == ConfigKind::Autoexec ? state.selected_autoexec_idx : state.selected_video_idx;
}
fs::path& source_of(AppState& state, const KindSpec& spec) {
    return spec.kind == ConfigKind::Autoexec ? state.autoexec_source : state.video_config_source;
}

// --- Capture-only states -------------------------------------------------------------------------

// CAPTURE ONLY. AppState::dev_state is empty in normal runs; the --capture dev mode sets it to
// render states that otherwise need a destructive or slow action. Names: an optional "video"
// prefix ("video" alone, or "video-<state>") picks the second tab, then one of:
//   running            an install is running (busy button, disabled inputs)
//   result-ok          the success toast (and for video the per-account result)
//   result-failed      autoexec: the inline error; video: a per-account result with one failure
//   confirm            the delete dialog for the selected entry
//   preview-error      the preview cannot read the file
//   preview-truncated  the preview shows the truncation note (a 256 byte cap)
//   empty              the library is empty
//   none-selected      nothing is selected
//   no-accounts        video: no accounts were found
//   many-accounts      video: fourteen synthetic accounts, so the list gets its filter field
// Nothing here writes to disk or the registry: values are fabricated in memory.
struct DevView {
    bool active = false;
    int tab = -1;
    bool running = false;
    bool result_ok = false;
    bool result_failed = false;
    bool confirm = false;
    bool preview_error = false;
    bool preview_truncated = false;
    bool empty = false;
    bool none_selected = false;
    bool no_accounts = false;
    bool many_accounts = false;
};

DevView parse_dev_state(const std::string& name) {
    DevView v;
    if (name.empty()) {
        return v;
    }
    v.active = true;
    std::string rest = name;
    v.tab = 0;
    if (rest == "video") {
        v.tab = 1;
        rest.clear();
    } else if (rest.rfind("video-", 0) == 0) {
        v.tab = 1;
        rest = rest.substr(6);
    }
    v.running = rest == "running";
    v.result_ok = rest == "result-ok";
    v.result_failed = rest == "result-failed";
    v.confirm = rest == "confirm";
    v.preview_error = rest == "preview-error";
    v.preview_truncated = rest == "preview-truncated";
    v.empty = rest == "empty";
    v.none_selected = rest == "none-selected";
    v.no_accounts = rest == "no-accounts";
    v.many_accounts = rest == "many-accounts";
    return v;
}

// CAPTURE ONLY: synthetic accounts for the many-accounts state.
const std::vector<AccountInfo>& dev_many_accounts() {
    static const std::vector<AccountInfo> accounts = [] {
        std::vector<AccountInfo> out;
        for (std::uint32_t i = 1; i <= 14; ++i) {
            AccountInfo a;
            a.account_id = 900000U + i;
            a.steamid64 = stc::core::steam::account_id_to_steamid64(a.account_id);
            wchar_t name[32];
            std::swprintf(name, 32, L"Test account %02u", i);
            a.persona_name = name;
            std::swprintf(name, 32, L"test_login_%02u", i);
            a.account_name = name;
            out.push_back(std::move(a));
        }
        return out;
    }();
    return accounts;
}

// --- File metadata and preview -------------------------------------------------------------------

struct FileMeta {
    bool ok = false;
    std::uint64_t size = 0;
    std::string modified;  // local short date and time
    std::string line;      // "2 KiB, 12.05.2026 17:30"
};

std::string format_file_time(fs::file_time_type t) {
    // MSVC's file_clock counts 100 ns ticks since 1601-01-01 UTC, which is a FILETIME.
    const auto ticks = static_cast<std::uint64_t>(t.time_since_epoch().count());
    FILETIME ft{static_cast<DWORD>(ticks & 0xFFFFFFFFULL), static_cast<DWORD>(ticks >> 32U)};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&ft, &utc)) {
        return {};
    }
    wchar_t stamp[32];
    std::swprintf(stamp, 32, L"%04u%02u%02uT%02u%02u%02uZ", static_cast<unsigned>(utc.wYear),
                  static_cast<unsigned>(utc.wMonth), static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
                  static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond));
    return stc::ui::format_backup_timestamp(stamp);
}

FileMeta read_meta(const fs::path& path) {
    FileMeta m;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return m;
    }
    m.ok = true;
    m.size = static_cast<std::uint64_t>(size);
    const auto time = fs::last_write_time(path, ec);
    if (!ec) {
        m.modified = format_file_time(time);
    }
    m.line = stc::ui::format_bytes(m.size);
    if (!m.modified.empty()) {
        m.line += ", " + m.modified;
    }
    return m;
}

struct Preview {
    fs::path path;
    std::uintmax_t cap = 0;
    bool loaded = false;
    std::string text;
    std::string error;  // empty when the text was read
    std::uint64_t size = 0;
    bool truncated = false;
};

void load_preview(Preview& p, const fs::path& path, std::uintmax_t cap) {
    p = Preview{};
    p.path = path;
    p.cap = cap;
    p.loaded = true;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        p.error = ec.message();
        return;
    }
    p.size = static_cast<std::uint64_t>(size);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        p.error = "The file could not be opened.";
        return;
    }
    const std::uintmax_t want = std::min<std::uintmax_t>(size, cap);
    std::string raw(static_cast<std::size_t>(want), '\0');
    f.read(raw.data(), static_cast<std::streamsize>(want));
    raw.resize(static_cast<std::size_t>(f.gcount()));
    p.truncated = size > cap;
    // UTF-8 BOM, carriage returns and NULs would show as glyph boxes; drop them.
    std::size_t start = raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    p.text.reserve(raw.size());
    for (std::size_t i = start; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\r') {
            continue;
        }
        p.text.push_back(c == '\0' ? ' ' : c);
    }
}

// Sizes and dates of the library entries, read when the list of paths changes, not per frame.
struct LibraryCache {
    std::vector<fs::path> paths;
    std::vector<FileMeta> meta;

    void sync(std::span<const LibraryEntry> entries) {
        bool same = paths.size() == entries.size();
        for (std::size_t i = 0; same && i < entries.size(); ++i) {
            same = paths[i] == entries[i].path;
        }
        if (same) {
            return;
        }
        paths.clear();
        meta.clear();
        for (const auto& e : entries) {
            paths.push_back(e.path);
            meta.push_back(read_meta(e.path));
        }
    }
    void invalidate() { paths.clear(); }
};

// --- Page state ----------------------------------------------------------------------------------

struct AccountResult {
    std::string name;   // persona (login)
    bool ok = false;
    std::string detail; // the error, or the target path
    fs::path target;
};

struct TabState {
    LibraryCache cache;
    Preview preview;
    ImGuiSelectionBasicStorage selection;
    bool auto_selected = false;

    bool confirm_open = false;
    fs::path delete_path;
    std::string delete_name;
    std::string delete_size;

    // The last install of this tab and the library file it was for.
    fs::path result_for;
    std::string install_error;  // autoexec failure, or a failed job (both tabs)
    bool install_error_open = false;
    std::vector<AccountResult> video_results;
    bool video_results_open = false;
    bool scroll_to_result = false;  // a new result scrolls the detail to it
    int scroll_frames = 0;

    float footer_h = 0.0F;      // pixels, measured in the last frame
    bool preview_open = false;  // the folded preview of a short pane
};

struct PageState {
    int tab = 0;
    std::array<TabState, 2> tabs;
    std::vector<std::string> game_labels;
    std::vector<fs::path> installed_for;  // libraries the installed flags were computed for
    std::vector<bool> game_installed;
    bool dev_toast_shown = false;
};

PageState g_page;

// The autoexec success toast. Its "Show in Explorer" action goes through the shell's toast router,
// so it still works when the user has moved to another page.
void show_installed_toast(const std::string& text, const std::string& description, const fs::path& target) {
    facet::ToastOptions options;
    options.tone = facet::Tone::Success;
    options.description = description.empty() ? nullptr : description.c_str();
    options.actionLabel = "Show in Explorer";
    stc::ui::show_action_toast(text.c_str(), options, [target] {
        if (!stc::ui::open_in_explorer(target)) {
            facet::ShowToast("The installed file is gone",
                             {.tone = facet::Tone::Warning, .description = "It was moved or deleted after the install."});
        }
    });
}

// --- Games ---------------------------------------------------------------------------------------

std::string game_name(std::uint32_t appid) {
    const auto* g = stc::core::appid::find_by_appid(appid);
    return g != nullptr ? stc::ui::to_utf8(g->display_name) : std::string("the game");
}

// Which known games have an install folder in one of the libraries, as install_autoexec looks
// for it. Recomputed only when the library list changes.
void sync_installed_games(const AppState& state) {
    const auto games = stc::core::appid::all_games();
    if (g_page.game_labels.size() != games.size()) {
        g_page.game_labels.clear();
        for (const auto& g : games) {
            g_page.game_labels.push_back(stc::ui::to_utf8(g.display_name));
        }
    }
    if (g_page.installed_for == state.libraries && g_page.game_installed.size() == games.size()) {
        return;
    }
    g_page.installed_for = state.libraries;
    g_page.game_installed.assign(games.size(), false);
    for (std::size_t i = 0; i < games.size(); ++i) {
        for (const auto& lib : state.libraries) {
            std::error_code ec;
            if (fs::is_directory(lib / "common" / std::wstring{games[i].install_folder}, ec)) {
                g_page.game_installed[i] = true;
                break;
            }
        }
    }
}

const char* filename_error(const std::string& name) {
    if (name.find_first_not_of(" \t") == std::string::npos) {
        return "Enter a file name.";
    }
    if (name.find_first_of("/\\<>:\"|?*") != std::string::npos) {
        return "A file name cannot contain / \\ < > : \" | ? *";
    }
    return nullptr;
}

std::string libraries_phrase(std::size_t n) {
    return n == 1 ? std::string("your Steam library") : "your " + std::to_string(n) + " Steam libraries";
}

std::string account_label(const AccountInfo& a) {
    std::string persona = stc::ui::to_utf8(a.persona_name);
    std::string login = stc::ui::to_utf8(a.account_name);
    if (persona.empty()) {
        persona = login.empty() ? std::to_string(a.account_id) : login;
    }
    return persona;
}

std::string account_detail(const AccountInfo& a) {
    std::string login = stc::ui::to_utf8(a.account_name);
    std::string id = stc::ui::to_utf8(a.steamid64);
    if (id.empty()) {
        id = stc::ui::to_utf8(stc::core::steam::account_id_to_steamid64(a.account_id));
    }
    return login.empty() ? id : login + ", " + id;
}

// --- Actions -------------------------------------------------------------------------------------

void select_entry(AppState& state, const KindSpec& spec, std::span<const LibraryEntry> entries, int index) {
    TabState& tab = g_page.tabs[static_cast<std::size_t>(spec.tab)];
    int& selected = selected_of(state, spec);
    fs::path& source = source_of(state, spec);
    if (index >= 0 && index < static_cast<int>(entries.size())) {
        selected = index;
        source = entries[static_cast<std::size_t>(index)].path;
    } else {
        selected = -1;
        source.clear();
    }
    tab.selection.Clear();
    if (selected >= 0) {
        tab.selection.SetItemSelected(static_cast<ImGuiID>(selected), true);
    }
}

void import_file(AppState& state, const KindSpec& spec) {
    const stc::ui::FileFilter filters[] = {{spec.filter_name, spec.filter_pattern}, {L"All files", L"*.*"}};
    const auto picked = stc::ui::open_file_dialog(spec.dialog_title, filters);
    if (!picked) {
        return;
    }
    const std::string picked_name = stc::ui::path_to_utf8(picked->filename());
    if (state.read_only) {
        facet::ShowToast("Nothing was imported", {.tone = facet::Tone::Warning,
                                                  .description = "The app runs in read-only mode."});
        return;
    }
    const fs::path stored = stc::core::config_library::import_config(state.configs_dir, spec.kind, *picked);
    state.refresh_config_library();
    g_page.tabs[static_cast<std::size_t>(spec.tab)].cache.invalidate();
    if (stored.empty()) {
        facet::ShowToast(("Could not import " + picked_name).c_str(),
                         {.tone = facet::Tone::Destructive,
                          .description = "The file could not be copied into the library. The log has the reason."});
        return;
    }
    auto& entries = library_of(state, spec);
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (entries[static_cast<std::size_t>(i)].path == stored) {
            select_entry(state, spec, entries, i);
            break;
        }
    }
    const std::string stored_name = stc::ui::path_to_utf8(stored.filename());
    if (stored_name == picked_name) {
        facet::ShowToast(("Imported " + stored_name).c_str(), {.tone = facet::Tone::Success});
    } else {
        const std::string description = "The library already had a " + picked_name + ".";
        facet::ShowToast(("Imported as " + stored_name).c_str(),
                         {.tone = facet::Tone::Success, .description = description.c_str()});
    }
}

void delete_entry(AppState& state, const KindSpec& spec) {
    TabState& tab = g_page.tabs[static_cast<std::size_t>(spec.tab)];
    if (state.read_only) {
        facet::ShowToast("Nothing was deleted", {.tone = facet::Tone::Warning,
                                                 .description = "The app runs in read-only mode."});
        return;
    }
    const int old_selected = selected_of(state, spec);
    const bool removed = stc::core::config_library::remove_config(tab.delete_path);
    state.refresh_config_library();
    tab.cache.invalidate();
    if (!removed) {
        facet::ShowToast(("Could not delete " + tab.delete_name).c_str(),
                         {.tone = facet::Tone::Destructive, .description = "The file is in use or already gone."});
        return;
    }
    // Keep the selection on the same file, or on the neighbour of a deleted selection.
    auto& entries = library_of(state, spec);
    int next = -1;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (entries[static_cast<std::size_t>(i)].path == source_of(state, spec)) {
            next = i;
        }
    }
    if (next < 0 && !entries.empty() && old_selected >= 0) {
        next = std::min(old_selected, static_cast<int>(entries.size()) - 1);
    }
    select_entry(state, spec, entries, next);
    facet::ShowToast(("Deleted " + tab.delete_name).c_str(), {.tone = facet::Tone::Success});
}

struct AutoexecOutcome {
    stc::core::autoexec::InstallResult result;
    int closed = 0;
};

void start_autoexec_install(AppState& state, const fs::path& source) {
    TabState& tab = g_page.tabs[0];
    tab.result_for = source;
    tab.install_error.clear();
    tab.install_error_open = false;
    if (state.read_only) {
        facet::ShowToast("Nothing was installed", {.tone = facet::Tone::Warning,
                                                   .description = "The app runs in read-only mode."});
        return;
    }
    const auto libraries = state.libraries;
    const std::uint32_t appid = state.autoexec_target_appid;
    const std::wstring dest = stc::ui::from_utf8(state.autoexec_dest_filename);
    const std::string dest_name = state.autoexec_dest_filename;
    const std::string game = game_name(appid);
    const std::size_t library_count = libraries.size();
    state.tasks.submit<AutoexecOutcome>(
        JobKind::ConfigInstall, "Installing " + dest_name + " to " + game,
        [libraries, appid, source, dest](stc::app::JobContext&) {
            AutoexecOutcome out;
            out.closed = stc::core::steam_lifecycle::close_games_for_appid(appid);
            out.result = stc::core::autoexec::install_autoexec(libraries, appid, source, dest);
            return out;
        },
        [dest_name, game, library_count](AutoexecOutcome out) {
            TabState& t = g_page.tabs[0];
            spdlog::info("Autoexec install: ok={} target={}", out.result.ok, stc::ui::path_to_utf8(out.result.target_path));
            if (out.result.ok) {
                std::string description;
                if (out.closed > 0) {
                    description = game + " was closed first.";
                }
                if (!out.result.backup_path.empty()) {
                    if (!description.empty()) {
                        description += " ";
                    }
                    description += "The previous file was kept as " +
                                   stc::ui::path_to_utf8(out.result.backup_path.filename()) + ".";
                }
                show_installed_toast("Installed " + dest_name + " to " + game, description, out.result.target_path);
                return;
            }
            const std::string reason = stc::ui::to_utf8(out.result.error);
            if (out.result.error == L"Game install folder not found in any Steam library") {
                t.install_error = "No installed copy of " + game + " was found in " + libraries_phrase(library_count) + ".";
            } else if (reason.empty()) {
                t.install_error = "The file could not be copied.";
            } else {
                t.install_error = reason + ".";
            }
            t.install_error_open = true;
            t.scroll_to_result = true;
        },
        [](const std::string& error) {
            TabState& t = g_page.tabs[0];
            t.install_error = "The install stopped: " + error;
            t.install_error_open = true;
            t.scroll_to_result = true;
        },
        {.must_finish = true});
}

struct VideoTarget {
    std::uint32_t account_id = 0;
    std::string name;
};

void start_video_install(AppState& state, const fs::path& source, std::span<const AccountInfo> accounts) {
    TabState& tab = g_page.tabs[1];
    tab.result_for = source;
    tab.install_error.clear();
    tab.install_error_open = false;
    tab.video_results.clear();
    tab.video_results_open = false;
    if (state.read_only || !state.install) {
        facet::ShowToast("Nothing was installed", {.tone = facet::Tone::Warning,
                                                   .description = "The app runs in read-only mode."});
        return;
    }
    std::vector<VideoTarget> targets;
    for (const auto& a : accounts) {
        if (state.video_config_targets.contains(a.account_id)) {
            targets.push_back({a.account_id, account_label(a)});
        }
    }
    const auto install = *state.install;
    const auto all_accounts = state.accounts;
    const std::size_t count = targets.size();
    state.tasks.submit<std::vector<AccountResult>>(
        JobKind::ConfigInstall, "Installing video settings to " + std::to_string(count) + (count == 1 ? " account" : " accounts"),
        [install, all_accounts, targets, source](stc::app::JobContext&) {
            std::vector<AccountResult> rows;
            stc::core::steam_lifecycle::close_games_for_appid(kCs2AppId);
            std::error_code ec;
            const bool source_exists = fs::exists(source, ec);
            // One account per call, so every result row belongs to a known account.
            for (const auto& t : targets) {
                AccountResult row;
                row.name = t.name;
                auto r = stc::core::autoexec::install_video_config(install, all_accounts, {t.account_id}, kCs2AppId, source);
                if (r.per_account.empty()) {
                    row.detail = source_exists ? "The account is no longer in Steam's userdata."
                                               : "The library file is missing.";
                } else {
                    const auto& one = r.per_account.front();
                    row.ok = one.ok;
                    row.target = one.target_path;
                    row.detail = one.ok ? stc::ui::path_to_utf8(one.target_path) : stc::ui::to_utf8(one.error);
                }
                rows.push_back(std::move(row));
            }
            return rows;
        },
        [count](std::vector<AccountResult> rows) {
            TabState& t = g_page.tabs[1];
            const auto ok = static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [](const auto& r) { return r.ok; }));
            spdlog::info("Video settings install: {} of {} succeeded", ok, rows.size());
            t.video_results = std::move(rows);
            t.video_results_open = true;
            t.scroll_to_result = true;
            if (ok == count && count > 0) {
                const std::string text =
                    "Installed cs2_video.txt to " + std::to_string(count) + (count == 1 ? " account" : " accounts");
                facet::ShowToast(text.c_str(), {.tone = facet::Tone::Success});
            }
        },
        [](const std::string& error) {
            TabState& t = g_page.tabs[1];
            t.install_error = "The install stopped: " + error;
            t.install_error_open = true;
            t.scroll_to_result = true;
        },
        {.must_finish = true});
}

// --- Drawing helpers -----------------------------------------------------------------------------

float units(float px) { return px / facet::Scale(); }

void gap(float u) { ImGui::Dummy(ImVec2(0.0F, facet::Px(u))); }

// After SetCursorScreenPos onto a line that a taller item started (a list row centers its text),
// text would keep that line's baseline offset and sit lower than placed.
void reset_line_baseline() { ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset = 0.0F; }

// The list pane: header row, then one two-line row per entry with a delete button.
void draw_list_pane(AppState& state, const KindSpec& spec, std::span<const LibraryEntry> entries, bool busy,
                    const DevView& dev) {
    const facet::Theme& t = facet::T();
    TabState& tab = g_page.tabs[static_cast<std::size_t>(spec.tab)];
    int& selected = selected_of(state, spec);

    // Header: "Saved configs" with the count, and the import button at the end.
    {
        char count[16];
        std::snprintf(count, sizeof(count), "%d", static_cast<int>(entries.size()));
        const float button_w = facet::MeasureText(facet::TextStyle::Body, "Import file").x +
                               facet::Px(t.spacing.lg * 2.0F + 16.0F + t.spacing.xs);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float control_h = facet::ControlHeight(facet::ControlSize::Inherit);
        const float text_h = facet::MeasureText(facet::TextStyle::Headline, "Ag").y;
        const float title_room = std::max(1.0F, width - button_w - facet::Px(t.spacing.sm));
        const float title_w = facet::MeasureText(facet::TextStyle::Headline, "Saved configs").x;
        const float count_w = facet::MeasureText(facet::TextStyle::Body, count).x;
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (control_h - text_h) * 0.5F));
        reset_line_baseline();
        facet::TruncatedText("Saved configs", {.style = facet::TextStyle::Headline, .width = units(title_room)});
        // The count follows the title when both fit.
        if (title_w + facet::Px(t.spacing.sm) + count_w <= title_room) {
            const float body_h = facet::MeasureText(facet::TextStyle::Body, "Ag").y;
            ImGui::SetCursorScreenPos(ImVec2(start.x + title_w + facet::Px(t.spacing.sm),
                                             start.y + (control_h - body_h) * 0.5F));
            reset_line_baseline();
            facet::StyledText(count, {.secondary = true});
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - button_w, start.y));
        ImGui::BeginDisabled(busy);
        if (facet::Button("Import file", {.icon = facet::Icon("plus"), .width = units(button_w)})) {
            import_file(state, spec);
        }
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + control_h));
        gap(t.spacing.sm);
    }

    tab.cache.sync(entries);
    // Single selection over the multi-select list: keep only the newest entry.
    tab.selection.Clear();
    if (selected >= 0) {
        tab.selection.SetItemSelected(static_cast<ImGuiID>(selected), true);
    }
    const ImGuiSelectionBasicStorage before = tab.selection;

    const float row_h = std::round(t.spacing.sm * 2.0F + units(facet::MeasureText(facet::TextStyle::Body, "Ag").y) +
                                   units(facet::MeasureText(facet::TextStyle::Footnote, "Ag").y) + 2.0F);
    int ask_delete = -1;
    int clicked = -1;
    int activated = -1;
    // The list pane is a column scroll, which does the scrolling; the virtual list inside is as tall
    // as its rows, so it never scrolls itself and never takes a gutter. It insets its rows like
    // every Facet scroll area (the padding the column scroll compensates for itself); placed that
    // inset to the left and two insets wider, its rows start where "Saved configs" starts and end
    // where the Import button ends.
    const float list_inset = facet::HairlineThickness() + std::max(1.0F, facet::PxSnapped(t.metrics.focusRingWidth));
    const float rows_px = facet::Px(row_h) * static_cast<float>(entries.size()) + list_inset * 2.0F + 1.0F;
    const float list_avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x - list_inset, ImGui::GetCursorScreenPos().y));
    ImGui::SetNextItemWidth(list_avail + list_inset * 2.0F);
    const bool list_open = facet::BeginVirtualList("##library", static_cast<int>(entries.size()), &tab.selection,
                                                   {.rowHeight = row_h, .height = units(rows_px)});
    if (list_open) {
        facet::RowRange rows;
        while (facet::NextVisibleRows(&rows)) {
            for (int i = rows.first; i < rows.last; ++i) {
                const auto& entry = entries[static_cast<std::size_t>(i)];
                const FileMeta* meta =
                    static_cast<std::size_t>(i) < tab.cache.meta.size() ? &tab.cache.meta[static_cast<std::size_t>(i)] : nullptr;
                ImGui::PushID(i);
                ImGui::SetNextItemAllowOverlap();
                if (facet::VirtualListRow(i)) {
                    activated = i;
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    clicked = i;
                }
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                const float content_x = ImGui::GetCursorScreenPos().x;
                const float pad = content_x - rmin.x;
                const float button = facet::ControlHeight(facet::ControlSize::Small);
                const float text_w = std::max(1.0F, rmax.x - content_x - pad - button - facet::Px(t.spacing.xs));
                const float l1 = facet::MeasureText(facet::TextStyle::Body, "Ag").y;
                const float l2 = facet::MeasureText(facet::TextStyle::Footnote, "Ag").y;
                const float top = rmin.y + std::round((rmax.y - rmin.y - l1 - l2 - facet::Px(2.0F)) * 0.5F);

                ImGui::SetCursorScreenPos(ImVec2(content_x, top));
                reset_line_baseline();
                facet::TruncatedText(entry.filename.c_str(), {.mode = facet::TruncateMode::Middle, .width = units(text_w)});
                ImGui::SetCursorScreenPos(ImVec2(content_x, top + l1 + facet::Px(2.0F)));
                reset_line_baseline();
                const char* line = meta != nullptr && meta->ok ? meta->line.c_str() : "Cannot read the file";
                facet::TruncatedText(line, {.style = facet::TextStyle::Footnote, .secondary = true, .width = units(text_w)});

                ImGui::SetCursorScreenPos(ImVec2(rmax.x - pad - button + facet::Px(t.spacing.xs),
                                                 rmin.y + std::round((rmax.y - rmin.y - button) * 0.5F)));
                char del[300];
                std::snprintf(del, sizeof(del), "Delete %s###delete", entry.filename.c_str());
                ImGui::BeginDisabled(busy);
                if (facet::IconButton(del, facet::Icon("trash"), {.size = facet::ControlSize::Small})) {
                    ask_delete = i;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        facet::EndVirtualList();
    }
    // The Delete key asks to delete the selected entry while the list pane has the focus.
    if (!busy && selected >= 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        ask_delete = selected;
    }

    // Reduce the list's selection to one entry: a new one when the list added one, none when it
    // cleared the selection (Escape).
    if (tab.selection.Size != before.Size || (selected >= 0 && !tab.selection.Contains(static_cast<ImGuiID>(selected)))) {
        int next = -1;
        void* it = nullptr;
        ImGuiID id = 0;
        while (tab.selection.GetNextSelectedItem(&it, &id)) {
            if (static_cast<int>(id) != selected || next < 0) {
                next = static_cast<int>(id);
            }
        }
        if (next != selected) {
            select_entry(state, spec, entries, next);
        }
    }
    // Enter or a double click on a row only selects it: the detail is always beside the list.
    if (clicked >= 0 && clicked != selected) {
        select_entry(state, spec, entries, clicked);
    } else if (activated >= 0 && activated != selected) {
        select_entry(state, spec, entries, activated);
    }

    if (ask_delete >= 0 && !dev.active) {
        const auto& e = entries[static_cast<std::size_t>(ask_delete)];
        tab.delete_path = e.path;
        tab.delete_name = e.filename;
        const FileMeta* meta = static_cast<std::size_t>(ask_delete) < tab.cache.meta.size()
                                   ? &tab.cache.meta[static_cast<std::size_t>(ask_delete)]
                                   : nullptr;
        tab.delete_size = meta != nullptr && meta->ok ? stc::ui::format_bytes(meta->size) : std::string();
        tab.confirm_open = true;
    }
}

// The file preview at the top of the detail pane.
// `fold`: a short pane, where the preview folds away so the install fields are in view without
// scrolling, and the size and date move into the fold's summary.
void draw_preview(const LibraryEntry& entry, const FileMeta* meta, TabState& tab, const DevView& dev, float pane_h,
                  bool fold) {
    const facet::Theme& t = facet::T();
    const std::uintmax_t cap = dev.preview_truncated ? 256 : kPreviewCapBytes;
    if (!tab.preview.loaded || tab.preview.path != entry.path || tab.preview.cap != cap) {
        load_preview(tab.preview, entry.path, cap);
    }
    const Preview& p = tab.preview;
    const std::string error = dev.preview_error ? std::string("Access is denied.") : p.error;

    // Title row: file name and a button that shows the library file in Explorer.
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float button = facet::ControlHeight(facet::ControlSize::Inherit);
    const float title_h = facet::MeasureText(facet::TextStyle::Title3, "Ag").y;
    const float row_h = std::max(button, title_h);
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (row_h - title_h) * 0.5F));
    facet::TruncatedText(entry.filename.c_str(), {.mode = facet::TruncateMode::Middle,
                                                  .style = facet::TextStyle::Title3,
                                                  .focusable = true,
                                                  .width = units(std::max(1.0F, width - button - facet::Px(t.spacing.sm)))});
    ImGui::SetCursorScreenPos(ImVec2(start.x + width - button, start.y + (row_h - button) * 0.5F));
    if (facet::IconButton("Show the library file in Explorer###reveal", facet::Icon("folder"))) {
        if (!stc::ui::open_in_explorer(entry.path)) {
            facet::ShowToast("The library file is gone", {.tone = facet::Tone::Warning});
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + row_h));
    const bool has_text = error.empty() && !p.text.empty();
    if (meta != nullptr && meta->ok && !(fold && has_text)) {
        facet::StyledText(meta->line.c_str(), {.style = facet::TextStyle::Footnote, .secondary = true});
    }
    gap(fold ? t.spacing.xs : t.spacing.md);

    if (!error.empty()) {
        const std::string message = "The preview needs the file's text. " + error;
        facet::EmptyState("Cannot read this file###preview-error",
                          {.message = message.c_str(), .icon = facet::Icon("error"), .tone = facet::Tone::Destructive,
                           .compact = true});
        return;
    }
    if (p.text.empty()) {
        facet::EmptyState("This file is empty###preview-empty", {.icon = facet::Icon("file"), .compact = true});
        return;
    }
    const auto line_count = std::count(p.text.begin(), p.text.end(), '\n') + (p.text.back() == '\n' ? 0 : 1);
    std::string summary = std::to_string(line_count) + (line_count == 1 ? " line" : " lines");
    if (meta != nullptr && meta->ok) {
        summary += ", " + stc::ui::format_bytes(meta->size);
    }
    if (fold && !facet::BeginDisclosure("Preview###preview-fold", &tab.preview_open, {.summary = summary.c_str()})) {
        return;
    }
    // A third of the pane at most, so the install fields stay in view below it.
    const float max_h = std::clamp(units(pane_h) * 0.36F, 96.0F, 280.0F);
    const float fit_h = static_cast<float>(line_count) * units(facet::MeasureText(facet::TextStyle::Footnote, "Ag").y) +
                        t.spacing.lg * 2.0F;
    facet::CodeView("##preview", p.text, {.language = facet::CodeLanguage::Cpp, .height = std::min(max_h, fit_h)});
    if (p.truncated) {
        gap(t.spacing.xs);
        char note[128];
        std::snprintf(note, sizeof(note), "Showing the first %s of %s. Install copies the whole file.",
                      stc::ui::format_bytes(p.cap).c_str(), stc::ui::format_bytes(p.size).c_str());
        facet::StyledText(note, {.style = facet::TextStyle::Footnote, .secondary = true, .wrap = true});
    }
    if (fold) {
        facet::EndDisclosure();
    }
}

void draw_install_error(TabState& tab) {
    if (tab.install_error.empty()) {
        return;
    }
    gap(facet::T().spacing.md);
    if (facet::InlineAlert("Install failed###install-error", &tab.install_error_open, tab.install_error.c_str(),
                           {.tone = facet::Tone::Destructive}) == facet::InlineAlertAction::Dismissed) {
        tab.install_error.clear();
    }
}

// What the footer's primary button shows and whether it can run.
struct InstallAction {
    std::string label;
    bool enabled = false;
    std::string info;  // the line under the button: what happens, or why it cannot run
};

// The autoexec install fields.
InstallAction draw_autoexec_fields(AppState& state, bool busy, bool can_read) {
    sync_installed_games(state);
    const auto games = stc::core::appid::all_games();

    std::vector<facet::ChoiceItem> items;
    items.reserve(games.size());
    int index = -1;
    for (std::size_t i = 0; i < games.size(); ++i) {
        items.push_back({.label = g_page.game_labels[i].c_str(),
                         .detail = g_page.game_installed[i] ? nullptr : "Not installed"});
        if (games[i].appid == state.autoexec_target_appid) {
            index = static_cast<int>(i);
        }
    }
    const bool game_found = index >= 0 && g_page.game_installed[static_cast<std::size_t>(index)];
    const std::string game = game_name(state.autoexec_target_appid);
    const std::string not_found = "Not found in " + libraries_phrase(state.libraries.size()) + ".";

    ImGui::BeginDisabled(busy);
    facet::BeginSettingsRow("Game");
    if (facet::Dropdown("##value", &index, items,
                        {.searchable = items.size() > 8, .error = game_found ? nullptr : not_found.c_str()})) {
        state.autoexec_target_appid = games[static_cast<std::size_t>(index)].appid;
    }
    facet::EndSettingsRow();

    const char* name_error = filename_error(state.autoexec_dest_filename);
    facet::BeginSettingsRow("File name", {.description = "In the game's cfg folder"});
    facet::TextField("##value", &state.autoexec_dest_filename,
                     {.placeholder = "autoexec.cfg",
                      .maxLength = 120,
                      .error = name_error,
                      .helpText = "Use autoexec.cfg to run it at every launch, or another name and exec it from the console."});
    facet::EndSettingsRow();
    ImGui::EndDisabled();

    InstallAction action;
    action.enabled = can_read && game_found && name_error == nullptr && !state.libraries.empty();
    action.label = "Install to " + game + "###install";
    if (busy) {
        action.info = "Installing. " + game + " is closed first if it is running, which can take a few seconds.";
    } else if (!can_read) {
        action.info = "The file cannot be read, so it cannot be installed.";
    } else if (!game_found) {
        action.info = "Install " + game + " with Steam first, or pick another game.";
    } else if (name_error != nullptr) {
        action.info = "Fix the file name to install.";
    } else {
        // The effect in plain words; the backup's exact name is in the toast after the install.
        const std::string& name = state.autoexec_dest_filename;
        action.info = game + " is closed first if it is running. An existing " + name +
                      " is kept as a backup copy next to it.";
    }
    return action;
}

// The accounts list of the video form: checkboxes for a few accounts, a filtered checklist for
// many.
void draw_accounts(AppState& state, std::span<const AccountInfo> accounts) {
    const facet::Theme& t = facet::T();
    auto& targets = state.video_config_targets;
    // The same label column as the autoexec form, so both tabs line up.
    facet::SettingsRowScope row("Accounts");
    if (accounts.empty()) {
        facet::EmptyState("No accounts found in Steam's userdata###no-accounts",
                          {.message = "Sign in to Steam once with the account, then re-detect Steam in Settings.",
                           .icon = facet::Icon("user"),
                           .compact = true});
        return;
    }
    const std::size_t n = accounts.size();
    auto checked = std::make_unique<bool[]>(n);
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        checked[i] = targets.contains(accounts[i].account_id);
        count += checked[i] ? 1 : 0;
    }
    bool changed = false;
    if (n >= kSearchableAccountsFrom) {
        std::vector<std::string> labels;
        std::vector<std::string> details;
        std::vector<facet::ChoiceItem> items;
        labels.reserve(n);
        details.reserve(n);
        for (const auto& a : accounts) {
            labels.push_back(account_label(a));
            details.push_back(account_detail(a));
        }
        for (std::size_t i = 0; i < n; ++i) {
            items.push_back({.label = labels[i].c_str(), .detail = details[i].c_str()});
        }
        changed = facet::SearchableChecklist("##value", std::span<bool>(checked.get(), n), items,
                                             {.placeholder = "Filter accounts", .visibleRows = 6});
    } else {
        ImGui::BeginGroup();
        facet::CheckState all = count == 0 ? facet::CheckState::Off
                                           : (count == n ? facet::CheckState::On : facet::CheckState::Mixed);
        if (facet::Checkbox("Select all###select-all", &all)) {
            const bool value = all == facet::CheckState::On;
            for (std::size_t i = 0; i < n; ++i) {
                checked[i] = value;
            }
            changed = true;
        }
        // Each account: the persona name on the box, login name and SteamID64 under the name.
        const float text_x = facet::Px(t.metrics.checkboxSize) + std::round(facet::Px(t.spacing.sm));
        for (std::size_t i = 0; i < n; ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string label = account_label(accounts[i]) + "###account";
            if (facet::Checkbox(label.c_str(), &checked[i])) {
                changed = true;
            }
            const std::string detail = account_detail(accounts[i]);
            const float x = ImGui::GetItemRectMin().x + text_x;
            ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetItemRectMax().y));
            facet::TruncatedText(detail.c_str(),
                                 {.style = facet::TextStyle::Footnote,
                                  .secondary = true,
                                  .width = units(std::max(1.0F, ImGui::GetContentRegionAvail().x))});
            // The next account follows spacing.sm below, not a whole item spacing.
            if (i + 1 < n) {
                ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x - text_x,
                                                 ImGui::GetItemRectMax().y + facet::Px(t.spacing.sm)));
            }
            ImGui::PopID();
        }
        ImGui::EndGroup();
    }
    if (changed) {
        for (std::size_t i = 0; i < n; ++i) {
            if (checked[i]) {
                targets.insert(accounts[i].account_id);
            } else {
                targets.erase(accounts[i].account_id);
            }
        }
    }
}

// The per-account result of the last video install.
void draw_video_results(TabState& tab) {
    if (tab.video_results.empty()) {
        return;
    }
    const facet::Theme& t = facet::T();
    const auto total = tab.video_results.size();
    const auto ok = static_cast<std::size_t>(
        std::count_if(tab.video_results.begin(), tab.video_results.end(), [](const auto& r) { return r.ok; }));
    char title[96];
    facet::Tone tone = facet::Tone::Success;
    const char* message = "Each account starts CS2 with these video settings.";
    if (ok == total) {
        std::snprintf(title, sizeof(title), "Installed to %d %s###video-result", static_cast<int>(total),
                      total == 1 ? "account" : "accounts");
    } else if (ok == 0) {
        std::snprintf(title, sizeof(title), "Install failed for %s###video-result",
                      total == 1 ? "the account" : "every account");
        tone = facet::Tone::Destructive;
        message = "Nothing was changed. The reason is listed per account below.";
    } else {
        std::snprintf(title, sizeof(title), "Installed to %d of %d accounts###video-result", static_cast<int>(ok),
                      static_cast<int>(total));
        tone = facet::Tone::Warning;
        message = "The failed accounts keep their old settings. The reason is listed below.";
    }
    gap(t.spacing.md);
    if (facet::InlineAlert(title, &tab.video_results_open, message, {.tone = tone}) ==
        facet::InlineAlertAction::Dismissed) {
        tab.video_results.clear();
        return;
    }
    gap(t.spacing.sm);
    for (std::size_t i = 0; i < total; ++i) {
        const AccountResult& r = tab.video_results[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string label = r.name + (r.ok ? ": installed" : ": failed") + "###status";
        facet::StatusDot(label.c_str(), {.tone = r.ok ? facet::Tone::Success : facet::Tone::Destructive});
        ImGui::Indent(facet::Px(t.spacing.lg));
        facet::TruncatedText(r.detail.c_str(), {.mode = r.ok ? facet::TruncateMode::Middle : facet::TruncateMode::End,
                                                .style = facet::TextStyle::Footnote,
                                                .secondary = true});
        ImGui::Unindent(facet::Px(t.spacing.lg));
        gap(t.spacing.xs);
        ImGui::PopID();
    }
}

// The video install fields: the accounts.
InstallAction draw_video_fields(AppState& state, bool busy, bool can_read, std::span<const AccountInfo> accounts) {
    ImGui::BeginDisabled(busy);
    draw_accounts(state, accounts);
    ImGui::EndDisabled();

    std::size_t count = 0;
    for (const auto& a : accounts) {
        count += state.video_config_targets.contains(a.account_id) ? 1 : 0;
    }
    InstallAction action;
    action.enabled = can_read && count > 0 && state.install.has_value();
    action.label = count == 0 ? std::string("Install###install")
                              : "Install to " + std::to_string(count) + (count == 1 ? " account" : " accounts") +
                                    "###install";
    action.info = "Counter-Strike 2 is closed first if it is running. Each account's current video settings are "
                  "kept as a backup copy.";
    if (busy) {
        action.info = "Installing. Counter-Strike 2 is closed first if it is running, which can take a few seconds.";
    } else if (!can_read) {
        action.info = "The file cannot be read, so it cannot be installed.";
    } else if (accounts.empty()) {
        action.info = "There is no account to install to.";
    } else if (count == 0) {
        action.info = "Tick at least one account to install to.";
    }
    return action;
}

// CAPTURE ONLY: fabricated results for the result states. Nothing is installed.
void apply_dev_results(const KindSpec& spec, TabState& tab, const LibraryEntry& entry,
                       std::span<const AccountInfo> accounts, const DevView& dev) {
    if (!dev.result_ok && !dev.result_failed) {
        return;
    }
    tab.result_for = entry.path;
    if (spec.kind == ConfigKind::Autoexec) {
        if (dev.result_failed) {
            tab.install_error = "No installed copy of Counter-Strike 2 was found in your Steam library.";
            tab.install_error_open = true;
            tab.scroll_to_result = true;
        } else if (!g_page.dev_toast_shown) {
            g_page.dev_toast_shown = true;
            facet::ShowToast("Installed autoexec.cfg to Counter-Strike 2",
                             {.tone = facet::Tone::Success,
                              .description = "The previous file was kept as autoexec.cfg.bak.20261122T101500Z.",
                              .actionLabel = "Show in Explorer"});
        }
        return;
    }
    if (tab.video_results.empty()) {
        for (std::size_t i = 0; i < accounts.size() && i < 3; ++i) {
            AccountResult r;
            r.name = account_label(accounts[i]);
            r.ok = !(dev.result_failed && i == 1);
            r.detail = r.ok ? "C:\\Steam\\userdata\\" + std::to_string(accounts[i].account_id) + "\\730\\local\\cfg\\cs2_video.txt"
                            : std::string("copy_file failed: Access is denied.");
            tab.video_results.push_back(std::move(r));
        }
        tab.video_results_open = true;
        tab.scroll_to_result = true;
        if (dev.result_ok && !g_page.dev_toast_shown) {
            g_page.dev_toast_shown = true;
            const std::string text = "Installed cs2_video.txt to " + std::to_string(tab.video_results.size()) + " accounts";
            facet::ShowToast(text.c_str(), {.tone = facet::Tone::Success});
        }
    }
}

// One tab: an empty library state, or the list and detail view.
void draw_tab(AppState& state, const KindSpec& spec, const DevView& dev) {
    const facet::Theme& t = facet::T();
    TabState& tab = g_page.tabs[static_cast<std::size_t>(spec.tab)];
    auto& library = library_of(state, spec);
    const std::span<const LibraryEntry> entries =
        dev.empty ? std::span<const LibraryEntry>() : std::span<const LibraryEntry>(library);
    std::span<const AccountInfo> accounts(state.accounts);
    if (dev.no_accounts) {
        accounts = {};
    } else if (dev.many_accounts) {
        accounts = dev_many_accounts();
    }
    const bool busy = dev.running || state.tasks.is_running(JobKind::ConfigInstall);

    int& selected = selected_of(state, spec);
    fs::path& source = source_of(state, spec);
    // Keep the index on the source path when the library list changed under it.
    if (!source.empty() &&
        (selected < 0 || selected >= static_cast<int>(entries.size()) ||
         entries[static_cast<std::size_t>(selected)].path != source)) {
        int found = -1;
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            if (entries[static_cast<std::size_t>(i)].path == source) {
                found = i;
            }
        }
        selected = found;
        if (found < 0) {
            source.clear();
        }
    } else if (source.empty() && selected >= 0) {
        selected = -1;
    }
    // The first visit selects the first entry, so the detail has something to show, and ticks the
    // account Steam signed in last, the likely target of a video settings install.
    if (!tab.auto_selected) {
        tab.auto_selected = true;
        if (selected < 0 && !entries.empty()) {
            select_entry(state, spec, entries, 0);
        }
        if (spec.kind == ConfigKind::Video && state.video_config_targets.empty()) {
            for (const auto& a : state.accounts) {
                if (a.most_recent) {
                    state.video_config_targets.insert(a.account_id);
                }
            }
        }
    }
    if (dev.none_selected) {
        selected = -1;
        source.clear();
    }

    if (entries.empty()) {
        const float h = units(ImGui::GetContentRegionAvail().y);
        ImGui::BeginDisabled(busy);
        const std::string title = std::string(spec.empty_title) + "###empty";
        if (facet::EmptyState(title.c_str(),
                              {.message = spec.empty_message,
                               .icon = facet::Icon("file"),
                               .actionLabel = "Import file",
                               .height = std::max(0.0F, h)})) {
            import_file(state, spec);
        }
        ImGui::EndDisabled();
        return;
    }

    const float avail_units = facet::AvailableWidthUnits();
    const float list_w = avail_units >= 720.0F ? 288.0F : 248.0F;
    // The library and its detail are one page card spanning the column down to the bottom of the
    // page, like the cards on every other page. Inside the card's padding: the list column (a
    // column scroll) from the card content's left edge, the strip that holds the list's bar, a
    // divider, spacing.lg, and the detail column to the card content's right edge, whose scrolling
    // part is a column scroll too; its bar runs in the card's right padding. Both columns stop one
    // card padding above the card's bottom edge. The page has at least 540 units here (the window
    // minimum), so the two columns always sit side by side.
    const float column_left = ImGui::GetCursorScreenPos().x;
    begin_page_card(spec.id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float card_padding = origin.x - column_left;
    const float content_right = origin.x + ImGui::GetContentRegionAvail().x;
    const float panes_px = std::max(facet::Px(96.0F), ImGui::GetContentRegionAvail().y - card_padding);
    const float list_px = facet::Px(list_w);
    const float divider_x = std::round(origin.x + list_px + stc::ui::column_scroll_gutter());
    const float detail_left = divider_x + facet::HairlineThickness() + facet::Px(t.spacing.lg);
    const float detail_px = std::max(1.0F, content_right - detail_left);

    if (stc::ui::begin_column_scroll("##list", list_w, units(panes_px))) {
        draw_list_pane(state, spec, entries, busy, dev);
        stc::ui::end_column_scroll();
    }
    facet::HairlineV(ImGui::GetWindowDrawList(), divider_x, origin.y, origin.y + panes_px,
                     facet::ToU32(t.colors.separator));

    ImGui::SetCursorScreenPos(ImVec2(detail_left, origin.y));
    ImGui::Indent(detail_left - origin.x);
    {
        const float pane_h = panes_px;
        if (selected < 0 || selected >= static_cast<int>(entries.size())) {
            facet::EmptyState("Select a config to preview and install it###none",
                              {.message = "The list on the left holds your saved files.",
                               .icon = facet::Icon("list"),
                               .height = std::max(0.0F, units(pane_h))});
        } else {
            const auto& entry = entries[static_cast<std::size_t>(selected)];
            // A result belongs to the file it was made for.
            if (tab.result_for != entry.path) {
                tab.install_error.clear();
                tab.video_results.clear();
            }
            if (dev.active) {
                apply_dev_results(spec, tab, entry, accounts, dev);
            }
            const FileMeta* meta = static_cast<std::size_t>(selected) < tab.cache.meta.size()
                                       ? &tab.cache.meta[static_cast<std::size_t>(selected)]
                                       : nullptr;
            const float content_w = detail_px;
            // The preview, the fields and the results scroll; the footer with the primary action
            // stays at the bottom of the pane, so it is in view at every window height.
            const float footer_h = tab.footer_h > 0.0F ? tab.footer_h : facet::Px(96.0F);
            const float scroll_h = std::max(facet::Px(96.0F), pane_h - footer_h);
            InstallAction action;
            const ImVec2 scroll_at = ImGui::GetCursorScreenPos();
            // A column scroll exactly as wide as the footer below it: its content ends on the card
            // content's right edge whether it scrolls or not, and its bar runs in the card padding.
            if (stc::ui::begin_column_scroll("##detail-scroll", units(content_w), units(scroll_h))) {
                const bool fold = units(pane_h) < 440.0F;
                draw_preview(entry, meta, tab, dev, pane_h, fold);
                const bool can_read = tab.preview.error.empty() && !dev.preview_error;
                if (fold) {
                    gap(t.spacing.sm);
                } else {
                    gap(t.spacing.md);
                    facet::Separator();
                    gap(t.spacing.sm);
                    facet::StyledText("Install", {.style = facet::TextStyle::Headline});
                    gap(t.spacing.xs);
                }
                action = spec.kind == ConfigKind::Autoexec ? draw_autoexec_fields(state, busy, can_read)
                                                           : draw_video_fields(state, busy, can_read, accounts);
                draw_install_error(tab);
                draw_video_results(tab);
                gap(t.spacing.md);
                // A new result scrolls into view. The scroll range grows with the result only in
                // the frames after it first shows, so the scroll follows for a few frames.
                if (tab.scroll_to_result) {
                    tab.scroll_frames = 3;
                    tab.scroll_to_result = false;
                }
                if (tab.scroll_frames > 0) {
                    ImGui::SetScrollY(ImGui::GetScrollMaxY());
                    --tab.scroll_frames;
                }
                stc::ui::end_column_scroll();
            }
            ImGui::SetCursorScreenPos(ImVec2(scroll_at.x, scroll_at.y + scroll_h));

            // Footer: the one primary action of the page and what it will do.
            const float footer_top = ImGui::GetCursorScreenPos().y;
            facet::Separator();
            gap(t.spacing.sm);
            ImGui::BeginDisabled(!action.enabled || busy);
            const bool pressed = facet::Button(action.label.c_str(), {.variant = facet::ButtonVariant::Primary,
                                                                      .icon = facet::Icon("download"),
                                                                      .loading = busy});
            ImGui::EndDisabled();
            if (pressed && action.enabled && !busy && !dev.active) {
                if (spec.kind == ConfigKind::Autoexec) {
                    start_autoexec_install(state, entry.path);
                } else {
                    start_video_install(state, entry.path, accounts);
                }
            }
            facet::StyledText(action.info.c_str(), {.style = facet::TextStyle::Footnote,
                                                    .secondary = true,
                                                    .wrap = true,
                                                    .width = units(content_w)});
            tab.footer_h = ImGui::GetCursorScreenPos().y - footer_top + facet::Px(t.spacing.xs);
        }
    }
    ImGui::Unindent(detail_left - origin.x);
    // The card ends one padding under the columns: the item spacing under this empty item plus the
    // card's filler make up that padding.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + panes_px));
    ImGui::Dummy(ImVec2(content_right - origin.x, 0.0F));
    end_page_card();

    // The delete dialog, with what will happen in concrete terms.
    if (dev.confirm && selected >= 0 && !tab.confirm_open) {
        const auto& e = entries[static_cast<std::size_t>(selected)];
        tab.delete_path = e.path;
        tab.delete_name = e.filename;
        const FileMeta* meta = static_cast<std::size_t>(selected) < tab.cache.meta.size()
                                   ? &tab.cache.meta[static_cast<std::size_t>(selected)]
                                   : nullptr;
        tab.delete_size = meta != nullptr && meta->ok ? stc::ui::format_bytes(meta->size) : std::string();
        tab.confirm_open = true;
    }
    if (tab.confirm_open) {
        const std::string title = "Delete " + tab.delete_name + "?###confirm-delete";
        std::string message = "Removes " + tab.delete_name;
        if (!tab.delete_size.empty()) {
            message += " (" + tab.delete_size + ")";
        }
        message += " from the library. Copies you already installed into games or accounts stay where they are.";
        const facet::DialogResult result = facet::ConfirmDialog(
            title.c_str(), &tab.confirm_open, message.c_str(), {.confirmLabel = "Delete", .destructive = true});
        if (result == facet::DialogResult::Confirmed && !dev.active) {
            delete_entry(state, spec);
        }
    }
}

}  // namespace

void draw_configs_page(stc::app::AppState& state) {
    stc::ui::page_header("Configs", "Save your config files here, then install them into a game or a Steam account.");
    if (!state.install) {
        stc::ui::steam_not_found_state(state);
        return;
    }
    const DevView dev = parse_dev_state(state.dev_state);
    if (dev.tab >= 0) {
        g_page.tab = dev.tab;
    }
    static const char* const kTabs[] = {"Autoexec", "CS2 video settings"};
    facet::TabBar("##configs-tabs", &g_page.tab, kTabs);
    stc::ui::page_block_gap();
    if (g_page.tab == 0) {
        draw_tab(state, kAutoexecSpec, dev);
    } else {
        draw_tab(state, kVideoSpec, dev);
    }
}

}  // namespace stc::ui::pages
