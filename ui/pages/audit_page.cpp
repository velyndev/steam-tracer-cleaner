#include "ui/pages/audit_page.hpp"

#include <imgui.h>

#include <facet/facet.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "app/app_state.hpp"
#include "core/audit.hpp"
#include "ui/common.hpp"

namespace stc::ui::pages {
namespace {

using stc::app::AppState;
using stc::app::JobKind;
using stc::core::audit::Report;

struct AuditUi {
    std::optional<Report> report;
    facet::DataState state = facet::DataState::Loading;
    std::string error;
    bool requested = false;

    bool dev_applied = false;
    int last_drawn_frame = -100;
};

AuditUi g_ui;

// The narrowest a figure of the summary card may be side by side with the other two (units).
constexpr float kSummaryMinFigure = 150.0F;

const facet::DataTableColumn kBrowserColumns[] = {
    {.label = "Browser", .width = 96.0F, .flags = ImGuiTableColumnFlags_NoHide},
    {.label = "Profile", .width = 120.0F},
    {.label = "Cookie file"},
    // No state column: the scan lists only databases that exist and never tests whether a
    // browser holds them open, so a "Readable" pill on every row would claim something unchecked.
    {.label = "Steam cookies", .width = 112.0F, .align = facet::Align::End},
};
const facet::DataTableColumn kDumpColumns[] = {
    {.label = "Folder", .flags = ImGuiTableColumnFlags_NoHide},
    {.label = "Files", .width = 80.0F, .align = facet::Align::End},
    {.label = "Size", .width = 96.0F, .align = facet::Align::End},
    {.label = "##Open", .width = 48.0F, .flags = ImGuiTableColumnFlags_NoResize},
};

// The report is built on a worker from copies of the install and the accounts.
void build(AppState& state) {
    if (!state.install || state.tasks.is_running(JobKind::AuditReport)) {
        return;
    }
    g_ui.requested = true;
    if (!g_ui.report) {
        g_ui.state = facet::DataState::Loading;
    }
    auto install = *state.install;
    auto accounts = state.accounts;
    state.tasks.submit<Report>(
        JobKind::AuditReport, "Building the audit report",
        [install, accounts](stc::app::JobContext&) { return stc::core::audit::build_report(install, accounts); },
        [](Report report) {
            g_ui.report = std::move(report);
            g_ui.state = facet::DataState::Ready;
            g_ui.error.clear();
        },
        [](const std::string& error) {
            g_ui.report.reset();
            g_ui.state = facet::DataState::Error;
            g_ui.error = error;
        });
}

void show_export_result(bool ok, const std::filesystem::path& path) {
    const std::string where = path_to_utf8(path);
    if (ok) {
        show_action_toast(
            "Audit report saved",
            {.tone = facet::Tone::Success, .description = where.c_str(), .actionLabel = "Show in Explorer"},
            [path] { open_in_explorer(path); });
    } else {
        facet::ShowToast("The report could not be saved",
                         {.tone = facet::Tone::Destructive, .description = where.c_str()});
    }
}

void export_report() {
    if (!g_ui.report) {
        return;
    }
    const FileFilter filters[] = {{L"Text files", L"*.txt"}, {L"All files", L"*.*"}};
    const auto path = save_file_dialog(L"Export audit report", filters, L"audit-report.txt", L"txt");
    if (!path) {
        return;  // canceled
    }
    const std::string text = stc::core::audit::render_text(*g_ui.report);
    std::ofstream file(*path, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    show_export_result(!file.fail(), *path);
}

// --- Capture-only ----------------------------------------------------------------------------------
// state.dev_state is set only by the dev capture mode (--state). These branches fabricate states that
// would otherwise need a slow or failing scan, or a file written by Export. Nothing is written here.
// Returns true when the report must not be built for this state.
bool apply_dev_state(AppState& state) {
    const std::string& s = state.dev_state;
    if (s.empty()) {
        return false;
    }
    if (s == "loading") {
        g_ui.report.reset();
        g_ui.state = facet::DataState::Loading;
        return true;
    }
    if (s == "error") {
        g_ui.report.reset();
        g_ui.state = facet::DataState::Error;
        g_ui.error = "The Steam userdata folder could not be read.";
        return true;
    }
    if (s == "empty") {
        g_ui.report = Report{};
        g_ui.state = facet::DataState::Ready;
        return true;
    }
    if ((s == "result-ok" || s == "result-failed") && !g_ui.dev_applied && g_ui.report) {
        g_ui.dev_applied = true;
        show_export_result(s == "result-ok", state.data_dir / "audit-report.txt");
    }
    return false;
}

// --- Drawing ---------------------------------------------------------------------------------------

float fit_table_height(int rows) {
    const facet::Theme& t = facet::T();
    return t.table.headerHeight + static_cast<float>(std::max(rows, 1)) * t.list.rowHeight + 2.0F;
}

// The empty state inside a section's card.
void empty_note(const char* label) { (void)facet::EmptyState(label, {.icon = facet::Icon("info"), .compact = true}); }

// A table's loading placeholder inside a section's card.
void table_skeleton() { facet::Skeleton({.shape = facet::SkeletonShape::Rect, .height = 104.0F}); }

void draw_stats(const Report* report) {
    std::uint64_t userdata = 0;
    std::uint64_t dumps = 0;
    double accounts = 0.0;
    if (report != nullptr) {
        for (const auto& a : report->accounts) {
            userdata += a.userdata_bytes;
        }
        for (const auto& d : report->crash_dumps) {
            dumps += d.total_bytes;
        }
        accounts = static_cast<double>(report->accounts.size());
    }
    // One summary card, the same box every page leads with: three figures in equal thirds, each a
    // value in Title2 over its caption in the secondary color. Too narrow for three side by side,
    // they stack. A skeleton stands in for each value while the report is built.
    const facet::Theme& t = facet::T();
    const std::string values[3] = {
        std::to_string(static_cast<long long>(accounts)),
        format_bytes(userdata),
        format_bytes(dumps),
    };
    const char* const captions[3] = {"Accounts", "Account data on disk", "Crash dumps on disk"};

    begin_page_card("summary");
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float gap = facet::PxSnapped(t.spacing.lg);
    const bool stacked = width < 3.0F * facet::Px(kSummaryMinFigure) + 2.0F * gap;
    const float cell = stacked ? width : std::floor((width - 2.0F * gap) / 3.0F);
    const float value_h = facet::MeasureText(facet::TextStyle::Title2, "Ag").y;
    float y = start.y;
    float bottom = start.y;
    for (int i = 0; i < 3; ++i) {
        // Side by side the last third takes the rounding remainder, so the row ends on the edge.
        const float x = stacked ? start.x : start.x + static_cast<float>(i) * (cell + gap);
        const float w = stacked || i < 2 ? cell : start.x + width - x;
        if (stacked && i > 0) {
            y = bottom + facet::Px(t.spacing.md);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        // Value and caption close together, so they read as one figure.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, facet::Px(t.spacing.xs)));
        ImGui::BeginGroup();
        if (report == nullptr) {
            facet::Skeleton({.width = std::min(96.0F, w / facet::Scale()), .height = value_h / facet::Scale()});
        } else {
            facet::TruncatedText(values[i].c_str(), {.style = facet::TextStyle::Title2, .width = w / facet::Scale()});
        }
        facet::TruncatedText(captions[i], {.secondary = true, .width = w / facet::Scale()});
        ImGui::EndGroup();
        ImGui::PopStyleVar();
        bottom = std::max(bottom, ImGui::GetItemRectMax().y);
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, bottom));
    ImGui::Dummy(ImVec2(0.0F, 0.0F));
    end_page_card();
}

void draw_account_row(AppState& state, const stc::core::audit::AccountSummary& a) {
    const facet::Theme& t = facet::T();
    const std::string steamid = to_utf8(a.steamid64);
    const std::string persona = a.persona_name.empty() ? std::string("Unnamed account") : to_utf8(a.persona_name);
    const bool excluded = state.ignore_list.preserves_account(a.steamid64);
    ImGui::PushID(steamid.c_str());

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float gap = facet::Px(t.spacing.md);
    const float avatar = facet::Px(40.0F);

    // The exclusion button sits at the end of the first line; its width is the last frame's.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID button_key = ImGui::GetID("##button-width");
    const float button_w = storage->GetFloat(button_key, 0.0F);
    ImGui::SetCursorScreenPos(ImVec2(start.x + width - button_w, start.y));
    const char* button = excluded ? "Stop excluding###exclude" : "Exclude from cleanups###exclude";
    if (facet::Button(button, {.size = facet::ControlSize::Small})) {
        auto& ids = state.ignore_list.preserved_account_ids;
        if (excluded) {
            std::erase(ids, a.steamid64);
        } else {
            ids.push_back(a.steamid64);
        }
        state.ignore_list_changed();
    }
    storage->SetFloat(button_key, ImGui::GetItemRectSize().x);

    // Initials only: Avatar draws them from the visible label part; no picture is invented.
    std::string avatar_label = persona;
    std::erase(avatar_label, '#');
    avatar_label += "###avatar";
    ImGui::SetCursorScreenPos(start);
    (void)facet::Avatar(avatar_label.c_str(), facet::ImageHandle{}, {.diameter = 40.0F});

    const float text_x = start.x + avatar + gap;
    const float text_w = std::max(1.0F, width - avatar - gap - (button_w > 0.0F ? button_w + gap : 0.0F));
    ImGui::SetCursorScreenPos(ImVec2(text_x, start.y));
    ImGui::BeginGroup();
    facet::TruncatedText(persona.c_str(), {.style = facet::TextStyle::Headline, .width = text_w / facet::Scale()});
    std::string detail = to_utf8(a.account_name);
    detail = (detail.empty() ? std::string("No sign-in name") : "Sign-in name " + detail) + ", " +
             format_bytes(a.userdata_bytes) + " of account data";
    facet::TruncatedText(detail.c_str(), {.secondary = true, .width = text_w / facet::Scale()});

    const std::string id_text = "Steam ID " + steamid;
    facet::StyledText(id_text.c_str());
    const float text_mid = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5F;
    ImGui::SameLine(0.0F, facet::Px(t.spacing.xs));
    // Centered on the text line it copies, not on its top edge.
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
                                     std::floor(text_mid - facet::ControlHeight(facet::ControlSize::Small) * 0.5F)));
    (void)facet::CopyButton("Copy Steam ID###copy-id", steamid.c_str(), {.size = facet::ControlSize::Small});

    // What Steam keeps for the account, in plain words; the file behind each is in the tooltip.
    // The words carry the state, the icon repeats it, the tone is never alone. Steam Guard (ssfn)
    // files are not listed: they belong to the install, not to one account, and the report does
    // not check them per account.
    struct Fact {
        const char* present;
        const char* missing;
        const char* tooltip;
        bool value;
    };
    const Fact facts[] = {
        {"Settings file present###lc", "Settings file missing###lc",
         "The account's Steam settings: localconfig.vdf in its userdata folder.", a.has_localconfig},
        {"Sign-in entry present###lu", "Sign-in entry missing###lu",
         "Its entry in Steam's list of accounts that signed in on this PC (loginusers.vdf).",
         a.has_loginusers_entry},
    };
    facet::PushControlSize(facet::ControlSize::Small);
    facet::BeginFlow("facts", {.gap = facet::Gap::Tight, .width = text_w / facet::Scale()});
    if (excluded && facet::FlowNext()) {
        facet::StatusPill("Excluded from cleanups###excluded", {.tone = facet::Tone::Success, .icon = facet::Icon("lock")});
    }
    for (const Fact& f : facts) {
        if (facet::FlowNext()) {
            facet::StatusPill(f.value ? f.present : f.missing, {.icon = facet::Icon(f.value ? "check" : "minus")});
            facet::ItemTooltip("%s", f.tooltip);
        }
    }
    facet::EndFlow();
    facet::PopControlSize();
    ImGui::EndGroup();

    const float bottom = std::max(ImGui::GetItemRectMax().y, start.y + avatar);
    ImGui::SetCursorScreenPos(ImVec2(start.x, bottom));
    ImGui::Dummy(ImVec2(width, 0.0F));
    ImGui::PopID();
}

void draw_accounts(AppState& state, const Report* report) {
    const facet::Theme& t = facet::T();
    begin_page_card("accounts", "Accounts", "Steam accounts found on this PC, and what Steam keeps for each.");
    if (report == nullptr) {
        for (int i = 0; i < 2; ++i) {
            if (i > 0) {
                ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.md)));
            }
            const ImVec2 start = ImGui::GetCursorScreenPos();
            facet::Skeleton({.shape = facet::SkeletonShape::Circle, .height = 40.0F});
            ImGui::SetCursorScreenPos(ImVec2(start.x + facet::Px(40.0F + t.spacing.md), start.y));
            facet::Skeleton({.lines = 3, .width = 260.0F});
        }
        end_page_card();
        return;
    }
    if (report->accounts.empty()) {
        empty_note("No Steam accounts were found on this PC.");
        end_page_card();
        return;
    }
    bool first = true;
    for (const auto& a : report->accounts) {
        if (!first) {
            ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.md)));
            facet::Separator();
            ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.md)));
        }
        first = false;
        draw_account_row(state, a);
    }
    end_page_card();
}

void draw_hardware(const Report* report) {
    const facet::Theme& t = facet::T();
    begin_page_card("hwid", "Hardware identifiers", "Values Windows uses to identify this PC. Select a value to copy it.");
    if (report == nullptr) {
        facet::Skeleton({.lines = 3});
    } else {
        auto value = [](const std::wstring& v) { return v.empty() ? std::string("Not found") : to_utf8(v); };
        const std::string name = value(report->hwid.computer_name);
        const std::string guid = value(report->hwid.machine_guid);
        const std::string mid = value(report->hwid.machine_id);
        facet::BeginKeyValueList("hwid-values", {.labelWidth = 136.0F, .copyable = true});
        (void)facet::KeyValue("Computer name", name.c_str());
        (void)facet::KeyValue("Machine GUID", guid.c_str());
        (void)facet::KeyValue("Machine ID", mid.c_str());
        facet::EndKeyValueList();
    }
    ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.md)));
    (void)facet::InlineAlert("##hwid-note", nullptr, "Read-only. A cleanup does not change these.",
                             {.icon = facet::Icon("info")});
    end_page_card();
}

void draw_browser_rows(const Report* report);

void draw_browsers(const Report* report) {
    begin_page_card("browsers", "Browser cookies",
                    "Browser profiles on this PC. Their cookies can keep you signed in to the Steam website.");
    if (report == nullptr) {
        table_skeleton();
    } else if (report->browsers.empty()) {
        empty_note("No supported browser profiles found.");
    } else {
        draw_browser_rows(report);
    }
    end_page_card();
}

void draw_browser_rows(const Report* report) {
    const auto& rows = report->browsers;
    // The cookie count column shows only when the scan counted any (-1 means not counted).
    const bool counted =
        std::any_of(rows.begin(), rows.end(), [](const auto& b) { return b.steam_cookies >= 0; });
    const std::span<const facet::DataTableColumn> columns(kBrowserColumns, counted ? 4U : 3U);
    const int count = static_cast<int>(rows.size());
    if (!facet::BeginDataTable("browsers", columns, count, nullptr, {.height = fit_table_height(count)})) {
        return;
    }
    facet::RowRange range;
    while (facet::NextVisibleRows(&range)) {
        for (int row = range.first; row < range.last; ++row) {
            const auto& b = rows[static_cast<std::size_t>(row)];
            ImGui::PushID(row);
            (void)facet::DataTableRow(row);
            ImGui::TableNextColumn();
            const std::string browser = to_utf8(b.browser);
            facet::TruncatedText(browser.c_str());
            ImGui::TableNextColumn();
            const std::string profile = to_utf8(b.profile);
            facet::TruncatedText(profile.c_str());
            ImGui::TableNextColumn();
            const std::string path = path_to_utf8(b.cookies_db);
            facet::TruncatedText(path.c_str(), {.mode = facet::TruncateMode::Middle, .secondary = true});
            if (counted) {
                ImGui::TableNextColumn();
                const std::string n = b.steam_cookies >= 0 ? std::to_string(b.steam_cookies) : std::string("Not counted");
                facet::StyledText(n.c_str(),
                                  {.secondary = b.steam_cookies < 0, .align = facet::Align::End, .width = facet::kFill});
            }
            ImGui::PopID();
        }
    }
    facet::EndDataTable();
}

void draw_dump_rows(const Report* report);

void draw_dumps(const Report* report) {
    begin_page_card("dumps", "Crash dumps",
                    "Files Windows and Steam write when a program crashes. They can hold account and system details.");
    if (report == nullptr) {
        table_skeleton();
    } else if (report->crash_dumps.empty()) {
        empty_note("No crash dump folders found.");
    } else {
        draw_dump_rows(report);
    }
    end_page_card();
}

void draw_dump_rows(const Report* report) {
    const auto& rows = report->crash_dumps;
    const int count = static_cast<int>(rows.size());
    if (!facet::BeginDataTable("dumps", kDumpColumns, count, nullptr, {.height = fit_table_height(count)})) {
        return;
    }
    facet::RowRange range;
    while (facet::NextVisibleRows(&range)) {
        for (int row = range.first; row < range.last; ++row) {
            const auto& d = rows[static_cast<std::size_t>(row)];
            ImGui::PushID(row);
            // Enter or a double click on the row opens the folder, like the button at its end.
            if (facet::DataTableRow(row)) {
                open_in_explorer(d.dir);
            }
            const float row_top = ImGui::GetItemRectMin().y;
            const float row_bottom = ImGui::GetItemRectMax().y;
            ImGui::TableNextColumn();
            const std::string path = path_to_utf8(d.dir);
            facet::TruncatedText(path.c_str(), {.mode = facet::TruncateMode::Middle});
            ImGui::TableNextColumn();
            const std::string n = std::to_string(d.count);
            facet::StyledText(n.c_str(), {.align = facet::Align::End, .width = facet::kFill});
            ImGui::TableNextColumn();
            const std::string size = format_bytes(d.total_bytes);
            facet::StyledText(size.c_str(), {.align = facet::Align::End, .width = facet::kFill});
            ImGui::TableNextColumn();
            // Facet centers cell text on the row; the button, taller than text, is centered here.
            const float button_h = facet::ControlHeight(facet::ControlSize::Small);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
                                             std::floor((row_top + row_bottom - button_h) * 0.5F)));
            if (facet::IconButton("Show in Explorer###open", facet::Icon("folder"), {.size = facet::ControlSize::Small})) {
                open_in_explorer(d.dir);
            }
            ImGui::PopID();
        }
    }
    facet::EndDataTable();
}

}  // namespace

void draw_audit_page(AppState& state) {
    const facet::Theme& t = facet::T();
    // Capture-only: each capture pass (a new frame sequence) shows its dev state again.
    const int frame = ImGui::GetFrameCount();
    if (frame - g_ui.last_drawn_frame != 1) {
        g_ui.dev_applied = false;
    }
    g_ui.last_drawn_frame = frame;
    const bool dev_fixed = apply_dev_state(state);
    const bool building = state.tasks.is_running(JobKind::AuditReport);

    begin_page_header("Audit", "A read-only look at what Steam and your browsers keep about your accounts on this PC.");
    if (state.install) {
        if (facet::Button("Refresh", {.icon = facet::Icon("refresh"), .loading = building}) && !dev_fixed) {
            build(state);
        }
        ImGui::SameLine(0.0F, facet::Px(t.spacing.sm));
        ImGui::BeginDisabled(!g_ui.report.has_value());
        if (facet::Button("Export report", {.variant = facet::ButtonVariant::Primary, .icon = facet::Icon("file")})) {
            export_report();
        }
        ImGui::EndDisabled();
    }
    end_page_header();

    if (!state.install) {
        steam_not_found_state(state);
        return;
    }
    if (!g_ui.requested && !dev_fixed) {
        build(state);
    }

    if (g_ui.state == facet::DataState::Error) {
        const std::string message = "Nothing was changed. " + g_ui.error;
        if (facet::EmptyState("The audit report could not be built###audit-error",
                              {.message = message.c_str(),
                               .icon = facet::Icon("error"),
                               .actionLabel = "Try again",
                               .tone = facet::Tone::Destructive,
                               .height = ImGui::GetContentRegionAvail().y / facet::Scale()}) &&
            !dev_fixed) {
            build(state);
        }
        return;
    }

    const Report* report = g_ui.report ? &*g_ui.report : nullptr;
    // What a cleanup can remove comes first; the hardware ids, which no cleanup changes, close
    // the page.
    if (begin_page_body("audit-body")) {
        draw_stats(report);
        page_block_gap();
        draw_accounts(state, report);
        page_block_gap();
        draw_browsers(report);
        page_block_gap();
        draw_dumps(report);
        page_block_gap();
        draw_hardware(report);
        end_page_body();
    }
}

}  // namespace stc::ui::pages
