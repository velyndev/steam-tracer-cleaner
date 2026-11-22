#include "ui/pages/exclusions_page.hpp"

#include <imgui.h>

#include <facet/facet.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.hpp"
#include "core/ignore_list.hpp"
#include "ui/common.hpp"
#include "ui/pages/settings_widgets.hpp"

namespace stc::ui::pages {
namespace {

using stc::app::AppState;
using stc::core::IgnoreList;

// --- Where edits go --------------------------------------------------------------------------------

// The list the page edits. Normally the app's ignore list, saved on every change; in a dev
// capture state a fabricated copy that is never saved.
struct Target {
    IgnoreList* list = nullptr;
    AppState* state = nullptr;  // null for the capture-only copy
    std::uint64_t revision() const { return state != nullptr ? state->ignore_revision : 0; }
    void changed() const {
        if (state != nullptr) {
            state->ignore_list_changed();
        }
    }
};

// --- Text helpers ----------------------------------------------------------------------------------

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
    return s;
}

bool is_steamid64(std::string_view s) {
    return s.size() == 17 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return out;
}

// Why a registry entry can never match what the cleaner removes, or null when it is fine. The
// cleaner names keys with the short roots (HKCU\Software\Valve\Steam), so the entry must too.
const char* registry_problem(std::string_view entry) {
    entry = trim(entry);
    if (entry.empty()) {
        return nullptr;
    }
    const std::string_view key = entry.substr(0, entry.find("::"));
    const std::size_t slash = key.find('\\');
    const std::string root = upper(key.substr(0, slash));
    if (root.starts_with("HKEY_")) {
        return "Write the root as HKCU, HKLM, HKCR or HKU. The long name never matches.";
    }
    if (root != "HKCU" && root != "HKLM" && root != "HKCR" && root != "HKU" && root != "HKCC") {
        return "Start with HKCU, HKLM, HKCR or HKU and a backslash.";
    }
    if (slash == std::string_view::npos || slash + 1 >= key.size()) {
        return "Add the key path after the root, for example HKCU\\Software\\Valve\\Steam.";
    }
    return nullptr;
}

// --- Editable string lists -------------------------------------------------------------------------

// The rows of one list as the page shows them. Edits stay here while a field is focused and are
// saved when the edit ends (focus leaves the field, or Enter). Rows that are empty or only
// spaces are dropped from what is saved; an empty row stays on screen until it is removed or
// filled, so "Add" and then "Browse" keep their row.
struct ListDraft {
    std::vector<std::string> rows;
    std::uint64_t revision = UINT64_MAX;
    int last_frame = -2;
    int focus_row = -1;
};

void sync_draft(ListDraft& draft, const std::vector<std::wstring>& list, const Target& target) {
    const int frame = ImGui::GetFrameCount();
    // Coming back to the page, or a change from elsewhere: start again from the saved list.
    if (draft.revision != target.revision() || draft.last_frame < frame - 1) {
        draft.rows.clear();
        for (const auto& entry : list) {
            draft.rows.push_back(to_utf8(entry));
        }
        draft.revision = target.revision();
        draft.focus_row = -1;
    }
    draft.last_frame = frame;
}

void commit_draft(ListDraft& draft, std::vector<std::wstring>& list, const Target& target) {
    std::vector<std::wstring> next;
    for (const auto& row : draft.rows) {
        const std::string_view text = trim(row);
        if (!text.empty()) {
            next.push_back(from_utf8(text));
        }
    }
    if (next != list) {
        list = std::move(next);
        target.changed();
    }
    draft.revision = target.revision();
}

enum class ListKind { Paths, Registry };

// One editable row per entry with a remove button, then the add button.
void edit_list(const char* id, ListKind kind, ListDraft& draft, std::vector<std::wstring>& list,
               const Target& target) {
    ImGui::PushID(id);
    sync_draft(draft, list, target);
    const facet::Theme& t = facet::T();
    const float button = facet::ControlHeight(facet::ControlSize::Inherit);
    const float gap = facet::Px(t.spacing.xs);

    bool commit = false;
    int remove = -1;
    if (draft.rows.empty()) {
        facet::StyledText(kind == ListKind::Paths ? "No paths are excluded." : "No registry entries are excluded.",
                          {.secondary = true});
    }
    for (std::size_t i = 0; i < draft.rows.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const float field_w = (ImGui::GetContentRegionAvail().x - button - gap) / facet::Scale();
        if (draft.focus_row == static_cast<int>(i)) {
            ImGui::SetKeyboardFocusHere();
            draft.focus_row = -1;
        }
        const float row_y = ImGui::GetCursorPosY();
        if (kind == ListKind::Paths) {
            facet::PathField("##path", &draft.rows[i],
                             {.placeholder = "A folder or file, for example C:\\Steam\\config\\mine",
                              .width = field_w});
            const bool browse = facet::PathFieldBrowsePressed();
            commit = commit || ImGui::IsItemDeactivated();
            if (browse) {
                if (auto picked = pick_folder(L"Choose a folder to exclude")) {
                    draft.rows[i] = path_to_utf8(*picked);
                    commit = true;
                }
            }
        } else {
            facet::TextField("##entry", &draft.rows[i],
                             {.placeholder = "HKCU\\Software\\Valve\\Steam\\Users\\<id> or KEY::VALUE",
                              .width = field_w,
                              .error = registry_problem(draft.rows[i])});
            commit = commit || ImGui::IsItemDeactivated();
        }
        const float after_y = ImGui::GetCursorPosY();
        ImGui::SameLine(0.0F, gap);
        ImGui::SetCursorPosY(row_y);
        if (facet::IconButton(kind == ListKind::Paths ? "Remove this path###remove" : "Remove this entry###remove",
                              facet::Icon("close"))) {
            remove = static_cast<int>(i);
        }
        // The next row starts under the taller of the field (with its message) and the button.
        ImGui::SetCursorPosY(std::max(after_y, ImGui::GetCursorPosY()));
        ImGui::PopID();
    }
    if (remove >= 0) {
        draft.rows.erase(draft.rows.begin() + remove);
        commit = true;
    }
    if (commit) {
        commit_draft(draft, list, target);
    }

    ImGui::Dummy(ImVec2(0.0F, facet::Px(t.spacing.xs)));
    if (facet::Button(kind == ListKind::Paths ? "Add path" : "Add entry", {.icon = facet::Icon("plus")})) {
        // One empty row at a time: Add focuses the empty row that is already there.
        const auto empty = std::find_if(draft.rows.begin(), draft.rows.end(),
                                        [](const std::string& r) { return trim(r).empty(); });
        if (empty == draft.rows.end()) {
            draft.rows.emplace_back();
            draft.focus_row = static_cast<int>(draft.rows.size()) - 1;
        } else {
            draft.focus_row = static_cast<int>(empty - draft.rows.begin());
        }
    }
    ImGui::PopID();
}

// --- Sections --------------------------------------------------------------------------------------

struct ExclusionsUi {
    ListDraft paths;
    ListDraft registry;
    std::string id_error;  // the last rejected SteamID64 entry
    bool kept_open = false;
};

ExclusionsUi g_ui;

void kept_list_item(const char* text) {
    // A small dot in the secondary color, then the text, wrapped.
    const float indent = facet::Px(facet::T().spacing.md);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float line = facet::MeasureText(facet::TextStyle::Body, "Ag").y;
    const float r = std::max(1.5F, facet::Px(2.0F));
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + r, at.y + line * 0.5F), r,
                                                facet::ToU32(facet::T().colors.labelSecondary));
    ImGui::SetCursorScreenPos(ImVec2(at.x + indent, at.y));
    facet::StyledText(text, {.secondary = true, .wrap = true,
                             .width = (ImGui::GetContentRegionAvail().x - indent) / facet::Scale()});
}

void accounts_section(const AppState& state, const Target& target, bool dev_invalid) {
    begin_page_card("accounts", "Accounts to keep",
                    "A kept account keeps its sign-in entry, its files and its Steam settings through every cleanup.");
    IgnoreList& list = *target.list;
    std::set<std::wstring> detected;
    for (const auto& account : state.accounts) {
        detected.insert(account.steamid64);
    }

    if (state.accounts.empty()) {
        facet::StyledText(state.install ? "Steam was found, but no account has signed in on it."
                                        : "No Steam accounts were detected on this PC.",
                          {.secondary = true, .wrap = true});
    }
    bool first = true;
    for (const auto& account : state.accounts) {
        if (!first) {
            facet::Separator();
        }
        first = false;
        const std::string id = to_utf8(account.steamid64);
        const std::string login = to_utf8(account.account_name);
        std::string name = to_utf8(account.persona_name);
        if (name.empty()) {
            name = login.empty() ? id : login;
        }
        std::string description = login.empty() ? id : login + "  \xC2\xB7  " + id;
        if (account.most_recent) {
            description += "  \xC2\xB7  signed in last";
        }
        const std::string label = name + "###account-" + id;
        facet::BeginSettingsRow(label.c_str(),
                                {.description = description.c_str(), .labelWidth = settings_widgets::kLabelWidth});
        bool kept = list.preserves_account(account.steamid64);
        if (facet::Toggle("##value", &kept)) {
            auto& ids = list.preserved_account_ids;
            if (kept) {
                ids.push_back(account.steamid64);
            } else {
                ids.erase(std::remove(ids.begin(), ids.end(), account.steamid64), ids.end());
            }
            target.changed();
        }
        facet::EndSettingsRow();
    }
    if (!state.accounts.empty()) {
        facet::Separator();
    }

    // Ids of accounts that are not on this PC.
    std::vector<std::string> others;
    for (const auto& id : list.preserved_account_ids) {
        if (!detected.contains(id)) {
            others.push_back(to_utf8(id));
        }
    }
    const std::string& error = dev_invalid ? std::string("\"7656119800\" is not a Steam ID: it has 10 digits, not 17.")
                                           : g_ui.id_error;
    facet::BeginSettingsRow("Accounts not on this PC###other-ids",
                            {.description = "For accounts that never signed in here.",
                             .labelWidth = settings_widgets::kLabelWidth});
    std::vector<std::string> edited = others;
    // The row fills the card; the field stops at a readable width next to its label.
    const float field_units =
        std::min(ImGui::CalcItemWidth() / facet::Scale(), settings_widgets::kRowFieldMaxWidth);
    if (facet::TagInput("##value", &edited,
                        {.placeholder = "Type a Steam ID, then press Enter",
                         .width = field_units,
                         .error = error.empty() ? nullptr : error.c_str(),
                         .helpText = "A Steam ID has 17 digits and starts with 7656."})) {
        std::vector<std::wstring> valid;
        std::string rejected;
        for (const auto& tag : edited) {
            const std::string_view text = trim(tag);
            if (is_steamid64(text)) {
                valid.push_back(from_utf8(text));
            } else if (!text.empty()) {
                rejected = std::string(text);
            }
        }
        if (rejected.empty()) {
            g_ui.id_error.clear();
        } else {
            const bool digits = std::all_of(rejected.begin(), rejected.end(), [](char c) { return c >= '0' && c <= '9'; });
            char message[160];
            if (digits) {
                std::snprintf(message, sizeof(message), "\"%.40s\" is not a Steam ID: it has %zu digits, not 17.",
                              rejected.c_str(), rejected.size());
            } else {
                std::snprintf(message, sizeof(message), "\"%.40s\" is not a Steam ID: use digits only.",
                              rejected.c_str());
            }
            g_ui.id_error = message;
        }
        // Detected accounts keep their place; the others follow in the order of the field.
        std::vector<std::wstring> next;
        for (const auto& id : list.preserved_account_ids) {
            if (detected.contains(id)) {
                next.push_back(id);
            }
        }
        for (auto& id : valid) {
            if (std::find(next.begin(), next.end(), id) == next.end()) {
                next.push_back(std::move(id));
            }
        }
        if (next != list.preserved_account_ids) {
            list.preserved_account_ids = std::move(next);
            target.changed();
        }
    }
    facet::EndSettingsRow();

    ImGui::Dummy(ImVec2(0.0F, facet::Px(facet::T().spacing.xs)));
    if (facet::BeginDisclosure("What is kept for these accounts", &g_ui.kept_open)) {
        // Plain words first, the file or key after it for people who look it up.
        kept_list_item("Its sign-in entry in Steam's account list (loginusers.vdf).");
        kept_list_item("Its userdata folder, with its Steam settings and game configs.");
        kept_list_item("Its Steam registry entries (HKCU\\Software\\Valve\\Steam\\Users).");
        kept_list_item("Its controller configs.");
        kept_list_item("Automatic sign-in (AutoLoginUser, RememberPassword, LastGameNameUsed), when it points at a "
                       "kept account.");
        kept_list_item("The Remote Play and recent players files (remoteclients.vdf, coplay), whenever any account "
                       "is kept: they cannot be split per account.");
        facet::EndDisclosure();
    }
    end_page_card();
}

void ssfn_section(const Target& target) {
    begin_page_card("ssfn", "Steam Guard files");
    facet::BeginSettingsRow("Keep Steam Guard files###keep-ssfn",
                            {.description = "Older Steam installs keep Steam Guard files (ssfn) that spare this PC "
                                            "the Steam Guard code. They are kept all together or not at all.",
                             .labelWidth = settings_widgets::kLabelWidth});
    if (facet::Toggle("##value", &target.list->preserve_all_ssfn)) {
        target.changed();
    }
    facet::EndSettingsRow();
    end_page_card();
}

void paths_section(const Target& target) {
    begin_page_card("paths", "Paths",
                    "Files and folders a cleanup never removes. A folder keeps everything inside it. Upper and lower "
                    "case do not matter.");
    edit_list("paths", ListKind::Paths, g_ui.paths, target.list->preserved_paths, target);
    end_page_card();
}

void registry_section(const Target& target) {
    begin_page_card("registry", "Registry",
                    "Registry keys and values a cleanup never removes. Write an entry like this:");
    const float gap = facet::Px(facet::T().spacing.xs);
    facet::InlineCode("HKCU\\Software\\Valve\\Steam\\Users\\<id>###form-key");
    ImGui::SameLine(0.0F, gap);
    facet::StyledText("keeps the key and everything below it.", {.secondary = true});
    facet::InlineCode("KEY::VALUE###form-value");
    ImGui::SameLine(0.0F, gap);
    facet::StyledText("keeps one value of that key.", {.secondary = true});
    ImGui::Dummy(ImVec2(0.0F, facet::Px(facet::T().spacing.xs)));
    edit_list("registry", ListKind::Registry, g_ui.registry, target.list->preserved_registry_values, target);
    end_page_card();
}

std::string count_phrase(std::size_t n, const char* one, const char* many) {
    return n == 0 ? std::string("no ") + many : std::to_string(n) + " " + (n == 1 ? one : many);
}

// One sentence for the page header: what every cleanup keeps right now.
std::string kept_summary(const IgnoreList& list) {
    const auto filled = [](const std::vector<std::wstring>& rows) {
        return static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [](const std::wstring& r) {
            return r.find_first_not_of(L" \t") != std::wstring::npos;
        }));
    };
    std::vector<std::string> parts = {
        count_phrase(list.preserved_account_ids.size(), "account", "accounts"),
        count_phrase(filled(list.preserved_paths), "path", "paths"),
        count_phrase(filled(list.preserved_registry_values), "registry entry", "registry entries"),
    };
    if (list.preserve_all_ssfn) {
        parts.emplace_back("the Steam Guard files");
    }
    std::string text = "Every cleanup leaves these alone. Kept now: ";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            text += i + 1 == parts.size() ? " and " : ", ";
        }
        text += parts[i];
    }
    return text + ".";
}

}  // namespace

void draw_exclusions_page(AppState& state) {
    // Capture-only: "empty" shows an ignore list with nothing in it, "invalid" one with entries
    // the page rejects or warns about ("invalid-registry": the same, scrolled to the registry
    // card; "section-registry": the real list scrolled there). The fabricated copies are never
    // saved.
    static IgnoreList dev_list;
    Target target{&state.ignore_list, &state};
    const bool dev_invalid = state.dev_state == "invalid" || state.dev_state == "invalid-registry";
    const bool dev_scroll_registry = state.dev_state == "section-registry" || state.dev_state == "invalid-registry";
    if (state.dev_state == "empty" || dev_invalid) {
        static bool filled = false;
        if (!filled) {
            if (dev_invalid) {
                dev_list.preserved_paths = {L"D:\\Games\\Steam\\config\\custom"};
                dev_list.preserved_registry_values = {L"HKEY_CURRENT_USER\\Software\\Valve\\Steam",
                                                      L"HKCU\\Software\\Valve\\Steam::AutoLoginUser", L"Steam"};
            }
            filled = true;
        }
        target = Target{&dev_list, nullptr};
    }

    // The header says what is protected right now, so the answer comes before the detail.
    const std::string summary = kept_summary(*target.list);
    stc::ui::page_header("Exclusions", summary.c_str());

    if (begin_page_body("exclusions-body")) {
        accounts_section(state, target, dev_invalid);
        page_block_gap();
        ssfn_section(target);
        page_block_gap();
        paths_section(target);
        page_block_gap();
        if (dev_scroll_registry) {
            ImGui::SetScrollHereY(0.0F);
        }
        registry_section(target);
        end_page_body();
    }
}

}  // namespace stc::ui::pages
