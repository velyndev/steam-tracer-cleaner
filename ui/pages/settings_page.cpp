#include "ui/pages/settings_page.hpp"

#include <imgui.h>

#include <facet/facet.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.hpp"
#include "core/scheduled_clean.hpp"
#include "core/update_check.hpp"
#include "core/version.hpp"
#include "platform/paths.hpp"
#include "ui/common.hpp"
#include "ui/pages/settings_widgets.hpp"

namespace stc::ui::pages {
namespace {

using stc::app::AppState;
using stc::app::JobContext;
using stc::app::JobKind;
using stc::app::ThemeChoice;

// The settings column stops at settings_widgets::kFormMaxWidth: rows wider than that put the
// controls far from their labels, and a reading column is easier to scan.
constexpr float kScaleControlWidth = 300.0F;
constexpr float kThemeControlWidth = 300.0F;
constexpr float kKeepCountWidth = 136.0F;
constexpr int kKeepCountMax = 999;
constexpr const char* kSearchName = "stc.settings";

// --- Row table ---------------------------------------------------------------------------------
//
// Every row the search can find. The card of a section is drawn while at least one of its rows
// matches; row_matches uses the same rule as Facet's settings search (PassesFilter on the label,
// the description and the keywords one at a time), so the two never disagree.

struct RowSpec {
    const char* label;
    const char* description;
    const char* keywords;
};

enum Section { kAppearance, kCleanup, kAutomation, kUpdates, kSteam, kStorage, kAbout, kSectionCount };

constexpr const char* kSectionTitles[kSectionCount] = {"Appearance", "Cleanup",  "Automation", "Updates",
                                                       "Steam",      "Storage", "About"};
// Names a capture can ask for with --state section-<name>.
constexpr const char* kSectionIds[kSectionCount] = {"appearance", "cleanup", "automation", "updates",
                                                    "steam",      "storage", "about"};

// Appearance
constexpr RowSpec kThemeRow{"Theme", "System follows the Windows app mode.", "dark light mode color scheme"};
constexpr RowSpec kAccentRow{"Accent color", "Marks the selection, the main action and keyboard focus.",
                             "colour highlight blue"};
constexpr RowSpec kScaleRow{"Interface scale", "Size of text and controls.", "zoom size dpi bigger smaller"};
constexpr RowSpec kContrastRow{"Increase contrast", "Makes dividers and control outlines easier to see.",
                               "accessibility borders high contrast"};
constexpr RowSpec kAppearanceRows[] = {kThemeRow, kAccentRow, kScaleRow, kContrastRow};

// Cleanup
constexpr RowSpec kBackupRow{"Back up before cleaning",
                             "The Cleanup page starts with its backup switch on, so a cleanup can be undone.",
                             "restore point safety"};
constexpr RowSpec kConfirmRow{"Confirm before a Full Wipe",
                              "Asks first, because a Full Wipe deletes the saved data of every account that is "
                              "not kept.",
                              "dialog warning profile userdata"};
constexpr RowSpec kKeepRow{"Backups to keep",
                           "When a cleanup makes a new backup, the oldest backups beyond this number are deleted.",
                           "prune limit count storage"};
constexpr RowSpec kCleanupRows[] = {kBackupRow, kConfirmRow, kKeepRow};

// Automation
constexpr RowSpec kLogonRow{"Run Quick Clean when I sign in",
                            "Each time you sign in to Windows, Quick Clean runs without opening a window and makes "
                            "a backup first. Uses a Windows Task Scheduler task.",
                            "schedule task logon startup automatic"};
constexpr RowSpec kAutomationRows[] = {kLogonRow};

// Updates
constexpr RowSpec kStartupCheckRow{"Check for updates at startup",
                                   "Tells you when a newer version is out. Sends one request to GitHub when the app "
                                   "starts, nothing else.",
                                   "network release version api.github.com"};
constexpr RowSpec kVersionRow{"Installed version", "Compared with the latest release on GitHub.",
                              "update check now release"};
constexpr RowSpec kSkipRow{"Skipped version", "The update notice stays hidden for this release.",
                           "update ignore notice"};
constexpr RowSpec kUpdateRows[] = {kStartupCheckRow, kVersionRow, kSkipRow};

// Steam. The status row comes first: without the status bar this card is the one place that
// says whether Steam was found, and its Re-detect button is what to press when it was not.
constexpr RowSpec kStatusRow{"Status", "Re-detect after you move Steam or add a library folder.",
                             "re-detect detection refresh found missing steam"};
constexpr RowSpec kInstallRow{"Install folder", nullptr, "steam path directory"};
constexpr RowSpec kUserdataRow{"Account data folder", nullptr, "steam userdata path directory accounts"};
constexpr RowSpec kLibrariesRow{"Library folders", nullptr, "steam games drives"};
constexpr RowSpec kAccountsRow{"Accounts", nullptr, "steam users logins"};
constexpr RowSpec kSteamRows[] = {kStatusRow, kInstallRow, kUserdataRow, kLibrariesRow, kAccountsRow};

// Storage
constexpr RowSpec kPortableRow{"Portable mode",
                               "Keeps settings and backups next to the app. To turn it on, put a file named "
                               "portable.flag next to the exe and restart.",
                               "usb stick flag"};
constexpr RowSpec kConfigDirRow{"Settings folder", "Your settings, exclusions and window layout.",
                                "config path directory settings.json ignore.json"};
constexpr RowSpec kDataDirRow{"Data folder", "Backups and saved configs.", "path directory backups library"};
constexpr RowSpec kLogDirRow{"Log folder", nullptr, "logs path directory"};
constexpr RowSpec kStorageRows[] = {kPortableRow, kConfigDirRow, kDataDirRow, kLogDirRow};

// About
constexpr RowSpec kAppVersionRow{"Version", nullptr, "about release build"};
constexpr RowSpec kRepoRow{"Source code", nullptr, "github repository about"};
constexpr RowSpec kReleasesRow{"Releases", nullptr, "download changelog github about"};
constexpr RowSpec kAboutRows[] = {kAppVersionRow, kRepoRow, kReleasesRow};

bool text_matches(const std::string& query, const char* text) {
    return text != nullptr && text[0] != '\0' && facet::PassesFilter(query, {}, text);
}

bool row_matches(const std::string& query, const RowSpec& row) {
    return query.empty() || text_matches(query, row.label) || text_matches(query, row.description) ||
           text_matches(query, row.keywords);
}

bool any_row_matches(const std::string& query, std::span<const RowSpec> rows) {
    return std::any_of(rows.begin(), rows.end(), [&](const RowSpec& r) { return row_matches(query, r); });
}

// --- Page state ----------------------------------------------------------------------------------

enum class TaskPhase { Unknown, Querying, Known, QueryFailed };

struct LogonTask {
    TaskPhase phase = TaskPhase::Unknown;
    bool registered = false;
    std::string next_run;
    bool changing = false;  // a register or unregister job runs
};

enum class UpdateOutcome { None, Current, Available, Failed };

struct UpdateCheck {
    UpdateOutcome outcome = UpdateOutcome::None;
    std::string latest_tag;
};

struct SettingsUi {
    std::string query;
    LogonTask task;
    UpdateCheck update;
    bool libraries_open = false;
};

SettingsUi g_ui;

// The dev capture's state override. Everything that reads it is capture-only: it renders a state
// from fabricated values instead of running the job that would produce it.
struct DevView {
    int scroll_to = -1;               // a section index
    std::optional<LogonTask> task;    // the automation row
    std::optional<UpdateCheck> update;
    bool update_checking = false;
    std::optional<std::string> skip;  // a skipped version
    std::optional<std::string> query;
};

DevView dev_view(const AppState& state) {
    DevView v;
    const std::string& s = state.dev_state;
    if (s.empty()) {
        return v;
    }
    for (int i = 0; i < kSectionCount; ++i) {
        if (s == std::string("section-") + kSectionIds[i]) {
            v.scroll_to = i;
        }
    }
    if (s == "search") {
        v.query = "backup";
    } else if (s == "search-empty") {
        v.query = "wallpaper";
    } else if (s == "task-loading") {
        v.task = LogonTask{.phase = TaskPhase::Querying};
        v.scroll_to = kAutomation;
    } else if (s == "task-on") {
        v.task = LogonTask{.phase = TaskPhase::Known, .registered = true, .next_run = "At sign-in"};
        v.scroll_to = kAutomation;
    } else if (s == "task-error") {
        v.task = LogonTask{.phase = TaskPhase::QueryFailed};
        v.scroll_to = kAutomation;
    } else if (s == "update-checking") {
        v.update_checking = true;
        v.scroll_to = kUpdates;
    } else if (s == "update-current") {
        v.update = UpdateCheck{UpdateOutcome::Current, {}};
        v.scroll_to = kUpdates;
    } else if (s == "update-available") {
        v.update = UpdateCheck{UpdateOutcome::Available, "v9.9.9"};
        v.scroll_to = kUpdates;
    } else if (s == "update-failed") {
        v.update = UpdateCheck{UpdateOutcome::Failed, {}};
        v.scroll_to = kUpdates;
    } else if (s == "update-skipping") {
        v.skip = "v9.9.9";
        v.scroll_to = kUpdates;
    }
    return v;
}

// --- Section frame -------------------------------------------------------------------------------

// Cards drawn so far in this frame's page body; every card after the first starts with the page's
// block gap.
int g_cards_drawn = 0;

// One titled card of settings rows (the shared page card). begin() asks the settings search and
// draws a hairline between rows; the caller ends each row with end(). The card fills the page
// column; the label column keeps its fixed width and controls take their natural width
// (settings_widgets::begin_row_content), so a control stays next to its label.
class SectionCard {
public:
    SectionCard(Section section, bool scroll_here) : section_(section) {
        if (g_cards_drawn++ > 0) {
            page_block_gap();
        }
        if (scroll_here) {
            ImGui::SetScrollHereY(0.0F);
        }
        begin_page_card(kSectionIds[section], kSectionTitles[section]);
    }
    ~SectionCard() { end_page_card(); }
    SectionCard(const SectionCard&) = delete;
    SectionCard& operator=(const SectionCard&) = delete;

    // True when the row matches the search: the row is begun (with `description`, or the
    // spec's own when null).
    bool begin(const RowSpec& row, const char* description = nullptr) {
        if (!facet::SettingsSearchMatch(row.label, {.description = row.description,
                                                     .keywords = row.keywords,
                                                     .section = kSectionTitles[section_]})) {
            return false;
        }
        if (rows_ > 0) {
            facet::Separator();
        }
        ++rows_;
        facet::BeginSettingsRow(row.label, {.description = description != nullptr ? description : row.description,
                                            .labelWidth = settings_widgets::kLabelWidth});
        settings_widgets::begin_row_content();
        return true;
    }
    // Ends the row begun by begin().
    void end() {
        settings_widgets::end_row_content();
        facet::EndSettingsRow();
    }

private:
    Section section_;
    int rows_ = 0;
};

// --- Appearance ----------------------------------------------------------------------------------

facet::ThemeMode to_facet(ThemeChoice choice) {
    switch (choice) {
        case ThemeChoice::Light: return facet::ThemeMode::Light;
        case ThemeChoice::Dark: return facet::ThemeMode::Dark;
        case ThemeChoice::System: break;
    }
    return facet::ThemeMode::System;
}

ThemeChoice from_facet(facet::ThemeMode mode) {
    switch (mode) {
        case facet::ThemeMode::Light: return ThemeChoice::Light;
        case facet::ThemeMode::Dark: return ThemeChoice::Dark;
        case facet::ThemeMode::System: break;
    }
    return ThemeChoice::System;
}

ImVec4 rgb_to_color(std::uint32_t rgb) {
    return ImVec4(static_cast<float>((rgb >> 16U) & 0xFFU) / 255.0F, static_cast<float>((rgb >> 8U) & 0xFFU) / 255.0F,
                  static_cast<float>(rgb & 0xFFU) / 255.0F, 1.0F);
}

std::uint32_t color_to_rgb(const ImVec4& c) {
    const auto channel = [](float v) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
    };
    return (channel(c.x) << 16U) | (channel(c.y) << 8U) | channel(c.z);
}

// The app's own blue first (the default of settings.json), then Facet's presets after its theme
// default, which stands for a different blue per theme and would not round-trip through one
// stored color.
std::span<const facet::AccentPreset> accent_presets() {
    static const std::vector<facet::AccentPreset> presets = [] {
        std::vector<facet::AccentPreset> list;
        list.push_back({"Default blue", rgb_to_color(stc::app::kDefaultAccentRgb)});
        const auto facet_presets = facet::AccentPresets();
        for (std::size_t i = 1; i < facet_presets.size(); ++i) {
            list.push_back(facet_presets[i]);
        }
        return list;
    }();
    return presets;
}

void appearance_section(AppState& state, bool scroll_here) {
    SectionCard card(kAppearance, scroll_here);
    if (card.begin(kThemeRow)) {
        facet::ThemeMode mode = to_facet(state.theme_mode);
        if (facet::ThemeModeControl("##value", &mode,
                                    {.apply = false, .width = settings_widgets::fit_width(kThemeControlWidth)})) {
            state.theme_mode = from_facet(mode);
            state.save_app_settings();
        }
        card.end();
    }
    if (card.begin(kAccentRow)) {
        ImVec4 accent = rgb_to_color(state.accent_rgb);
        // The swatches and the custom well side by side, instead of the well at the far end of
        // the row; narrower than that the picker wraps the swatches itself.
        const float side = facet::ControlHeight(facet::ControlSize::Inherit);
        const float gap = facet::Px(facet::T().spacing.xs);
        const float natural = static_cast<float>(accent_presets().size()) * (side + gap) + gap * 2.0F + side;
        ImGui::SetNextItemWidth(std::min(natural, settings_widgets::slot_width()));
        if (facet::AccentPicker("##value", nullptr, &accent, {.presets = accent_presets(), .apply = false})) {
            state.accent_rgb = color_to_rgb(accent);
            state.save_app_settings();
        }
        card.end();
    }
    if (card.begin(kScaleRow)) {
        float scale = state.ui_scale;
        if (facet::UiScaleControl("##value", &scale, stc::app::kMinUiScale, stc::app::kMaxUiScale,
                                  {.apply = false, .width = settings_widgets::fit_width(kScaleControlWidth)})) {
            state.ui_scale = scale;
            state.save_app_settings();
        }
        card.end();
    }
    if (card.begin(kContrastRow)) {
        if (facet::Toggle("##value", &state.increase_contrast)) {
            state.save_app_settings();
        }
        card.end();
    }
}

// --- Cleanup -------------------------------------------------------------------------------------

void cleanup_section(AppState& state, bool scroll_here) {
    SectionCard card(kCleanup, scroll_here);
    if (card.begin(kBackupRow)) {
        if (facet::Toggle("##value", &state.backup_by_default)) {
            state.save_app_settings();
        }
        card.end();
    }
    if (card.begin(kConfirmRow)) {
        if (facet::Toggle("##value", &state.confirm_full_wipe)) {
            state.save_app_settings();
        }
        card.end();
    }
    if (card.begin(kKeepRow)) {
        int keep = state.backup_keep_count;
        settings_widgets::InlineLine line;
        line.next();
        if (facet::NumberInput("##value", &keep, {.min = 0, .max = kKeepCountMax, .width = kKeepCountWidth}) &&
            keep != state.backup_keep_count) {
            state.backup_keep_count = std::clamp(keep, 0, kKeepCountMax);
            state.save_app_settings();
        }
        line.next(facet::MeasureText(facet::TextStyle::Footnote, "Ag").y);
        facet::StyledText(state.backup_keep_count == 0 ? "Keeps every backup" : "0 keeps every backup",
                          {.style = facet::TextStyle::Footnote, .secondary = true});
        card.end();
    }
}

// --- Automation ----------------------------------------------------------------------------------

void query_logon_task(AppState& state) {
    g_ui.task.phase = TaskPhase::Querying;
    state.tasks.submit<stc::core::scheduled::TaskInfo>(
        JobKind::Other, "Reading the Task Scheduler",
        [](JobContext&) { return stc::core::scheduled::query(); },
        [](stc::core::scheduled::TaskInfo info) {
            g_ui.task.phase = TaskPhase::Known;
            g_ui.task.registered = info.registered;
            g_ui.task.next_run = to_utf8(info.next_run);
        },
        [](const std::string&) { g_ui.task.phase = TaskPhase::QueryFailed; });
}

void change_logon_task(AppState& state, bool enable) {
    if (state.read_only) {
        // The dev modes never touch the Task Scheduler.
        facet::ShowToast("The sign-in task was not changed",
                         {.tone = facet::Tone::Warning, .description = "This run is read-only."});
        return;
    }
    g_ui.task.changing = true;
    if (enable) {
        const std::filesystem::path exe = stc::platform::exe_path();
        state.tasks.submit<bool>(
            JobKind::Other, "Adding the sign-in task",
            [exe](JobContext&) { return stc::core::scheduled::register_logon_task(exe); },
            [](bool ok) {
                g_ui.task.changing = false;
                g_ui.task.registered = ok;
                if (ok) {
                    facet::ShowToast("Quick Clean now runs when you sign in", {.tone = facet::Tone::Success});
                } else {
                    facet::ShowToast("Could not add the sign-in task",
                                     {.tone = facet::Tone::Destructive,
                                      .description = "Task Scheduler refused the change. The switch is off."});
                }
            },
            [](const std::string& error) {
                g_ui.task.changing = false;
                g_ui.task.registered = false;
                facet::ShowToast("Could not add the sign-in task",
                                 {.tone = facet::Tone::Destructive, .description = error.c_str()});
            });
    } else {
        state.tasks.submit<bool>(
            JobKind::Other, "Removing the sign-in task",
            [](JobContext&) { return stc::core::scheduled::unregister(); },
            [](bool ok) {
                g_ui.task.changing = false;
                g_ui.task.registered = !ok;
                if (ok) {
                    g_ui.task.next_run.clear();
                    facet::ShowToast("The sign-in task was removed", {.tone = facet::Tone::Success});
                } else {
                    facet::ShowToast("Could not remove the sign-in task",
                                     {.tone = facet::Tone::Destructive,
                                      .description = "Task Scheduler refused the change. The task is still there."});
                }
            },
            [](const std::string& error) {
                g_ui.task.changing = false;
                facet::ShowToast("Could not remove the sign-in task",
                                 {.tone = facet::Tone::Destructive, .description = error.c_str()});
            });
    }
}

void automation_section(AppState& state, bool scroll_here, const DevView& dev) {
    SectionCard card(kAutomation, scroll_here);
    const LogonTask& task = dev.task ? *dev.task : g_ui.task;
    if (card.begin(kLogonRow)) {
        const bool busy = task.phase == TaskPhase::Unknown || task.phase == TaskPhase::Querying || task.changing;
        bool on = task.registered;
        const bool failed = task.phase == TaskPhase::QueryFailed;
        facet::ToggleOptions options;
        options.loading = busy;
        options.readOnly = failed;
        if (facet::Toggle("##value", &on, options) && !dev.task && !busy && on != g_ui.task.registered) {
            g_ui.task.registered = on;
            change_logon_task(state, on);
        }
        // The messages under the switch use the slot's width (the Toggle's own helpText and
        // error wrap at the width of the switch).
        const float slot_units = settings_widgets::slot_width() / facet::Scale();
        if (failed) {
            facet::StyledText("Could not read the Task Scheduler state.",
                              {.style = facet::TextStyle::Footnote, .tone = facet::Tone::Destructive, .wrap = true,
                               .width = slot_units});
            if (facet::Link("Read it again", {.size = facet::ControlSize::Small}) && !dev.task) {
                query_logon_task(state);
            }
        } else if (task.phase == TaskPhase::Known && task.registered && !task.next_run.empty()) {
            const std::string next = "Next run: " + task.next_run;
            facet::StyledText(next.c_str(), {.style = facet::TextStyle::Footnote, .secondary = true, .wrap = true,
                                             .width = slot_units});
        }
        card.end();
    }
}

// --- Updates -------------------------------------------------------------------------------------

void check_for_updates(AppState& state) {
    if (state.offline) {
        facet::ShowToast("Update checks are off for this run", {.tone = facet::Tone::Warning});
        return;
    }
    using Result = std::optional<stc::core::update_check::Result>;
    state.tasks.submit<Result>(
        JobKind::UpdateCheck, "Checking for updates",
        [](JobContext&) -> Result {
            return stc::core::update_check::fetch_latest_release(stc::core::kRepoSlug, stc::core::kAppVersion);
        },
        [&state](Result r) {
            if (!r) {
                g_ui.update = {UpdateOutcome::Failed, {}};
            } else if (r->newer_than_current) {
                g_ui.update = {UpdateOutcome::Available, r->latest_tag};
                state.update_result = std::move(r);
            } else {
                g_ui.update = {UpdateOutcome::Current, r->latest_tag};
            }
        },
        [](const std::string&) { g_ui.update = {UpdateOutcome::Failed, {}}; });
}

void updates_section(AppState& state, bool scroll_here, const DevView& dev) {
    SectionCard card(kUpdates, scroll_here);
    if (card.begin(kStartupCheckRow)) {
        if (facet::Toggle("##value", &state.check_updates_on_launch)) {
            state.save_app_settings();
        }
        card.end();
    }

    // What the last check said: this page's own check, else the one at startup.
    UpdateCheck shown = dev.update ? *dev.update : g_ui.update;
    if (!dev.update && shown.outcome == UpdateOutcome::None && state.update_result &&
        state.update_result->newer_than_current) {
        shown = {UpdateOutcome::Available, state.update_result->latest_tag};
    }
    const bool checking = dev.update_checking || (!dev.update && state.tasks.is_running(JobKind::UpdateCheck));
    char description[160];
    switch (checking ? UpdateOutcome::None : shown.outcome) {
        case UpdateOutcome::Current:
            std::snprintf(description, sizeof(description), "This is the latest release.");
            break;
        case UpdateOutcome::Available:
            std::snprintf(description, sizeof(description), "Version %s is available on GitHub.",
                          shown.latest_tag.c_str());
            break;
        case UpdateOutcome::Failed:
            std::snprintf(description, sizeof(description), "Could not reach GitHub. Check the connection and try again.");
            break;
        case UpdateOutcome::None:
            std::snprintf(description, sizeof(description), "%s",
                          checking ? "Asking GitHub for the latest release." : kVersionRow.description);
            break;
    }
    if (card.begin(kVersionRow, description)) {
        char version[32];
        std::snprintf(version, sizeof(version), "v%s", stc::core::kAppVersion);
        settings_widgets::InlineLine line;
        line.next_text();
        facet::StyledText(version);
        line.next();
        if (facet::Button("Check now", {.icon = facet::Icon("refresh"), .loading = checking}) && !dev.update_checking) {
            check_for_updates(state);
        }
        if (!checking && shown.outcome == UpdateOutcome::Available) {
            line.next_text();
            facet::Link("Open releases page", {.url = kReleasesUrl});
        }
        card.end();
    }

    const std::string& skipped = dev.skip ? *dev.skip : state.version_check_skip_until;
    if (!skipped.empty()) {
        char description_skip[96];
        std::snprintf(description_skip, sizeof(description_skip), "The update notice stays hidden for %s.",
                      skipped.c_str());
        if (card.begin(kSkipRow, description_skip)) {
            settings_widgets::InlineLine line;
            line.next_text();
            facet::StyledText(skipped.c_str());
            line.next();
            if (facet::Button("Stop skipping") && !dev.skip) {
                state.version_check_skip_until.clear();
                state.update_modal_dismissed_this_session = false;
                state.save_app_settings();
            }
            card.end();
        }
    }
}

// --- Steam ---------------------------------------------------------------------------------------

void redetect_steam(AppState& state) {
    state.refresh_steam();
    if (state.install) {
        char description[96];
        std::snprintf(description, sizeof(description), "%zu %s, %zu library %s.", state.accounts.size(),
                      state.accounts.size() == 1 ? "account" : "accounts", state.libraries.size(),
                      state.libraries.size() == 1 ? "folder" : "folders");
        facet::ShowToast("Steam found", {.tone = facet::Tone::Success, .description = description});
    } else {
        facet::ShowToast("Steam was not found", {.tone = facet::Tone::Warning,
                                                 .description = "Install Steam or start it once, then re-detect."});
    }
}

// Found or not found, and the button that looks again, on one line.
void steam_status_row(AppState& state, SectionCard& card) {
    const bool found = state.install.has_value();
    char description[128];
    if (found) {
        std::snprintf(description, sizeof(description), "%zu %s and %zu library %s found. %s", state.accounts.size(),
                      state.accounts.size() == 1 ? "account" : "accounts", state.libraries.size(),
                      state.libraries.size() == 1 ? "folder" : "folders", kStatusRow.description);
    } else {
        std::snprintf(description, sizeof(description), "Install Steam or start it once, then re-detect.");
    }
    if (!card.begin(kStatusRow, description)) {
        return;
    }
    settings_widgets::InlineLine line;
    line.next_text();
    facet::StatusPill(found ? "Found###steam-status" : "Not found###steam-status",
                      {.tone = found ? facet::Tone::Success : facet::Tone::Warning,
                       .icon = facet::Icon(found ? "check" : "warning"),
                       .size = facet::ControlSize::Small});
    line.next();
    if (facet::Button("Re-detect Steam", {.icon = facet::Icon("refresh")})) {
        redetect_steam(state);
    }
    card.end();
}

void steam_section(AppState& state, bool scroll_here) {
    SectionCard card(kSteam, scroll_here);
    if (!state.install) {
        // Without an install there are no values to show; the alert says so in the card's
        // strongest voice and the status row under it can try again.
        facet::InlineAlert("Steam was not found###steam-missing", nullptr,
                           "Steam Tracer Cleaner looks for Steam in the registry and in Steam's default install "
                           "folder. Until it is found, Cleanup, Audit and Configs have nothing to work on.",
                           {.tone = facet::Tone::Warning});
        ImGui::Dummy(ImVec2(0.0F, facet::Px(facet::T().spacing.xs)));
        steam_status_row(state, card);
        return;
    }
    steam_status_row(state, card);
    const auto& install = *state.install;
    if (card.begin(kInstallRow)) {
        settings_widgets::path_value("install", install.install_path);
        card.end();
    }
    if (card.begin(kUserdataRow)) {
        settings_widgets::path_value("userdata", install.userdata_dir);
        card.end();
    }
    if (card.begin(kLibrariesRow)) {
        char summary[48];
        std::snprintf(summary, sizeof(summary), "%zu %s###libraries", state.libraries.size(),
                      state.libraries.size() == 1 ? "folder" : "folders");
        if (state.libraries.empty()) {
            settings_widgets::align_text_to_control();
            facet::StyledText("No library folders were found.", {.secondary = true});
        } else if (facet::BeginDisclosure(summary, &g_ui.libraries_open, {.style = facet::DisclosureStyle::Link})) {
            for (std::size_t i = 0; i < state.libraries.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                settings_widgets::path_value("library", state.libraries[i], false);
                ImGui::PopID();
            }
            facet::EndDisclosure();
        }
        card.end();
    }
    if (card.begin(kAccountsRow)) {
        char count[48];
        std::snprintf(count, sizeof(count), "%zu %s", state.accounts.size(),
                      state.accounts.size() == 1 ? "account signed in on this PC" : "accounts signed in on this PC");
        settings_widgets::InlineLine line;
        line.next_text();
        facet::StyledText(count);
        line.next_text();
        if (facet::Link("Choose accounts to keep")) {
            state.current_page = stc::app::Page::Exclusions;
        }
        card.end();
    }
}

// --- Storage -------------------------------------------------------------------------------------

void storage_section(AppState& state, bool scroll_here) {
    SectionCard card(kStorage, scroll_here);
    if (card.begin(kPortableRow)) {
        // The pill places itself on the text baseline of its line.
        settings_widgets::align_text_to_control();
        facet::StatusPill(state.portable_mode ? "On###portable" : "Off###portable",
                          {.tone = state.portable_mode ? facet::Tone::Success : facet::Tone::Neutral,
                           .size = facet::ControlSize::Small});
        card.end();
    }
    if (card.begin(kConfigDirRow)) {
        settings_widgets::path_value("config", state.config_dir);
        card.end();
    }
    if (card.begin(kDataDirRow)) {
        settings_widgets::path_value("data", state.data_dir);
        card.end();
    }
    if (card.begin(kLogDirRow)) {
        settings_widgets::path_value("logs", state.log_dir);
        card.end();
    }
}

// --- About ---------------------------------------------------------------------------------------

void about_section(bool scroll_here) {
    SectionCard card(kAbout, scroll_here);
    if (card.begin(kAppVersionRow)) {
        char version[64];
        std::snprintf(version, sizeof(version), "Steam Tracer Cleaner %s", stc::core::kAppVersion);
        settings_widgets::align_text_to_control();
        facet::StyledText(version);
        card.end();
    }
    if (card.begin(kRepoRow)) {
        settings_widgets::align_text_to_control();
        facet::Link("Repository on GitHub", {.url = kRepoUrl});
        card.end();
    }
    if (card.begin(kReleasesRow)) {
        settings_widgets::align_text_to_control();
        facet::Link("Releases and downloads", {.url = kReleasesUrl});
        card.end();
    }
}

}  // namespace

void draw_settings_page(AppState& state) {
    const DevView dev = dev_view(state);
    if (dev.query) {
        g_ui.query = *dev.query;
    }
    if (dev.scroll_to == kSteam) {
        g_ui.libraries_open = true;  // capture-only: show the library list
    }
    if (g_ui.task.phase == TaskPhase::Unknown) {
        query_logon_task(state);
    }

    stc::ui::page_header("Settings", "Changes are saved as you make them.");

    // The search stays in view above the body; it spans the page column like the cards below.
    facet::SearchField("##settings-search", &g_ui.query,
                       {.placeholder = "Search settings", .width = page_column_width() / facet::Scale()});
    page_block_gap();

    const std::string& q = g_ui.query;
    if (begin_page_body("settings-body")) {
        g_cards_drawn = 0;
        facet::BeginSettingsSearch(kSearchName, {.filter = q.c_str(), .filterSettings = &settings_widgets::kFilter});
        // Which cards have a matching row. The skipped-version row exists only while a version
        // is skipped, and without Steam only the status row does.
        const bool skipping = dev.skip || !state.version_check_skip_until.empty();
        bool shown[kSectionCount] = {
            any_row_matches(q, kAppearanceRows),
            any_row_matches(q, kCleanupRows),
            any_row_matches(q, kAutomationRows),
            any_row_matches(q, std::span<const RowSpec>(kUpdateRows).first(skipping ? 3 : 2)),
            state.install ? any_row_matches(q, kSteamRows) : row_matches(q, kStatusRow),
            any_row_matches(q, kStorageRows),
            any_row_matches(q, kAboutRows),
        };
        const auto here = [&](Section s) { return dev.scroll_to == s; };
        // Without Steam the Steam card leads, so the way to fix it is the first thing on the page.
        const bool steam_first = !state.install.has_value();
        if (steam_first && shown[kSteam]) steam_section(state, here(kSteam));
        if (shown[kAppearance]) appearance_section(state, here(kAppearance));
        if (shown[kCleanup]) cleanup_section(state, here(kCleanup));
        if (shown[kAutomation]) automation_section(state, here(kAutomation), dev);
        if (shown[kUpdates]) updates_section(state, here(kUpdates), dev);
        if (!steam_first && shown[kSteam]) steam_section(state, here(kSteam));
        if (shown[kStorage]) storage_section(state, here(kStorage));
        if (shown[kAbout]) about_section(here(kAbout));
        facet::EndSettingsSearch();

        if (std::none_of(std::begin(shown), std::end(shown), [](bool b) { return b; })) {
            char message[160];
            std::snprintf(message, sizeof(message), "No setting name or description contains \"%s\".", q.c_str());
            if (facet::EmptyState("No matching settings###no-match",
                                  {.message = message, .icon = facet::Icon("search"), .actionLabel = "Clear the search"})) {
                g_ui.query.clear();
            }
        }
        end_page_body();
    }
}

}  // namespace stc::ui::pages
