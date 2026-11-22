#include "ui/common.hpp"

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <facet/facet.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <system_error>
#include <utility>
#include <vector>

#include "app/app_state.hpp"

namespace stc::ui {
namespace {

using Microsoft::WRL::ComPtr;

HWND g_dialog_owner = nullptr;

// A toast whose action is run by poll_toast_actions().
struct RoutedToast {
    facet::ToastId id;
    std::function<void()> on_action;
    int shown_frame = 0;
};
std::vector<RoutedToast> g_routed_toasts;

// Dialog filter specs; the pointers stay valid during the call.
std::vector<COMDLG_FILTERSPEC> to_specs(std::span<const FileFilter> filters) {
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const auto& f : filters) {
        specs.push_back({f.name, f.pattern});
    }
    return specs;
}

std::optional<std::filesystem::path> run_dialog(IFileDialog* dialog) {
    if (FAILED(dialog->Show(g_dialog_owner))) {
        return std::nullopt;  // canceled or failed
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return std::nullopt;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path result{path};
    CoTaskMemFree(path);
    return result;
}

struct HeaderFrame {
    bool active = false;
    ImVec2 start{};
    float width = 0.0F;
    float row_height = 0.0F;
    ImVec2 actions_start{};
    ImGuiID actions_id = 0;
    const char* description = nullptr;
};

HeaderFrame g_header;

// The page title is the main heading of the window: Title1, one step above the section headings
// (Title3) and card titles inside the pages.
constexpr facet::TextStyle kHeaderTitleStyle = facet::TextStyle::Title1;

// --- Page layout tokens ------------------------------------------------------------------------------
// The one place the page layout sizes come from (see the layout rule in ui/common.hpp).
// The inner padding of a page card is facet::BeginCard's own (spacing.lg); the card is Facet's, so
// that padding, the corner radius and the surface are the same on every page by construction.
// Between two blocks of a page, and under the page header.
float kPageBlockGap() { return facet::T().spacing.xl; }
// Between a card's heading (title and description) and its content.
float kCardHeadingGap() { return facet::T().spacing.md; }
// Between a card's title and its description: they read as one heading.
float kCardTitleGap() { return facet::T().spacing.xs * 0.5F; }

PageLayoutDebug g_layout_debug;

// --- Column scroll metrics ---------------------------------------------------------------------------
// A Facet scroll area pads its content on every side by the focus ring outset (a hairline plus the
// focus ring width), and while its content overflows it keeps a gutter free beside its vertical
// bar: barInset + barThicknessHover + barGap from the area's right edge, the padding inside it. Both
// are internal to Facet (FocusRingOutset, ScrollBarGutter); these are the same formulas from public
// tokens. Every frame the area checks them against what it measures (see ColumnScrollDebug), and
// the selftest fails when they drift.
float column_inset() {
    return facet::HairlineThickness() + std::max(1.0F, facet::PxSnapped(facet::T().metrics.focusRingWidth));
}

struct ColumnFrame {
    float column_left = 0.0F;
    float column_right = 0.0F;
    float parent_max_x = 0.0F;  // the parent's content right extent before the area
    ImGuiWindow* parent = nullptr;
    ImGuiWindow* area = nullptr;
    int debug_index = -1;
};
constexpr int kColumnDepth = 4;
ColumnFrame g_columns[kColumnDepth];
int g_column_depth = 0;
int g_body_depth = -1;  // the column frame of the open page body; -1 when none is open
std::vector<ColumnScrollDebug> g_column_debug;
int g_column_debug_frame = -1;

}  // namespace

std::string to_utf8(std::wstring_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring from_utf8(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string path_to_utf8(const std::filesystem::path& p) { return to_utf8(p.native()); }

std::string format_bytes(std::uint64_t bytes) {
    constexpr std::uint64_t kKiB = 1024ULL;
    constexpr std::uint64_t kMiB = kKiB * 1024ULL;
    constexpr std::uint64_t kGiB = kMiB * 1024ULL;
    char buf[64];
    if (bytes >= kGiB) {
        std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / static_cast<double>(kGiB));
    } else if (bytes >= kMiB) {
        std::snprintf(buf, sizeof(buf), "%.1f MiB", static_cast<double>(bytes) / static_cast<double>(kMiB));
    } else if (bytes >= kKiB) {
        std::snprintf(buf, sizeof(buf), "%.0f KiB", static_cast<double>(bytes) / static_cast<double>(kKiB));
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

namespace {

// A stored backup timestamp "YYYYMMDDThhmmssZ" as a UTC SYSTEMTIME; false when it is not in that form.
bool parse_backup_timestamp(std::wstring_view stamp, SYSTEMTIME* utc) {
    auto digits = [&](std::size_t at, std::size_t count, WORD* out) {
        unsigned value = 0;
        for (std::size_t i = at; i < at + count; ++i) {
            if (stamp[i] < L'0' || stamp[i] > L'9') {
                return false;
            }
            value = value * 10U + static_cast<unsigned>(stamp[i] - L'0');
        }
        *out = static_cast<WORD>(value);
        return true;
    };
    *utc = {};
    return stamp.size() == 16 && stamp[8] == L'T' && stamp[15] == L'Z' && digits(0, 4, &utc->wYear) &&
           digits(4, 2, &utc->wMonth) && digits(6, 2, &utc->wDay) && digits(9, 2, &utc->wHour) &&
           digits(11, 2, &utc->wMinute) && digits(13, 2, &utc->wSecond);
}

std::string count_ago(long long n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many) + " ago";
}

}  // namespace

std::string format_backup_age(std::wstring_view stamp) {
    SYSTEMTIME utc{};
    FILETIME then{};
    if (!parse_backup_timestamp(stamp, &utc) || !SystemTimeToFileTime(&utc, &then)) {
        return {};
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](FILETIME f) {
        return static_cast<long long>((static_cast<unsigned long long>(f.dwHighDateTime) << 32U) | f.dwLowDateTime);
    };
    // FILETIME counts 100 ns ticks. A stamp from the future (a clock that was ahead) reads as now.
    const long long seconds = std::max(0LL, (ticks(now) - ticks(then)) / 10'000'000LL);
    constexpr long long kMinute = 60;
    constexpr long long kHour = 60 * kMinute;
    constexpr long long kDay = 24 * kHour;
    if (seconds < kMinute) return "Just now";
    if (seconds < kHour) return count_ago(seconds / kMinute, "minute", "minutes");
    if (seconds < kDay) return count_ago(seconds / kHour, "hour", "hours");
    if (seconds < 2 * kDay) return "Yesterday";
    if (seconds < 14 * kDay) return count_ago(seconds / kDay, "day", "days");
    if (seconds < 60 * kDay) return count_ago(seconds / (7 * kDay), "week", "weeks");
    if (seconds < 365 * kDay) return count_ago(seconds / (30 * kDay), "month", "months");
    return count_ago(seconds / (365 * kDay), "year", "years");
}

std::string format_backup_timestamp(std::wstring_view stamp) {
    SYSTEMTIME utc{};
    const bool parsed = parse_backup_timestamp(stamp, &utc);
    SYSTEMTIME local{};
    if (!parsed || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return to_utf8(stamp);
    }
    wchar_t date[64] = {};
    wchar_t time[64] = {};
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 64, nullptr) == 0 ||
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, time, 64) == 0) {
        return to_utf8(stamp);
    }
    return to_utf8(date) + " " + to_utf8(time);
}

void open_url(std::string_view url) {
    const std::wstring w = from_utf8(url);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool open_in_explorer(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false;
    }
    if (std::filesystem::is_directory(path, ec)) {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    const std::wstring args = L"/select,\"" + path.native() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    return true;
}

void set_dialog_owner(void* hwnd) { g_dialog_owner = static_cast<HWND>(hwnd); }

std::optional<std::filesystem::path> open_file_dialog(const wchar_t* title, std::span<const FileFilter> filters) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        spdlog::warn("open_file_dialog: CoCreateInstance failed");
        return std::nullopt;
    }
    const auto specs = to_specs(filters);
    if (!specs.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    }
    dialog->SetTitle(title);
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    return run_dialog(dialog.Get());
}

std::optional<std::filesystem::path> save_file_dialog(const wchar_t* title, std::span<const FileFilter> filters,
                                                      const wchar_t* default_name,
                                                      const wchar_t* default_extension) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        spdlog::warn("save_file_dialog: CoCreateInstance failed");
        return std::nullopt;
    }
    const auto specs = to_specs(filters);
    if (!specs.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    }
    dialog->SetTitle(title);
    if (default_name != nullptr) {
        dialog->SetFileName(default_name);
    }
    if (default_extension != nullptr) {
        dialog->SetDefaultExtension(default_extension);
    }
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    return run_dialog(dialog.Get());
}

std::optional<std::filesystem::path> pick_folder(const wchar_t* title) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        spdlog::warn("pick_folder: CoCreateInstance failed");
        return std::nullopt;
    }
    dialog->SetTitle(title);
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    return run_dialog(dialog.Get());
}

void begin_page_header(const char* title, const char* description) {
    IM_ASSERT(!g_header.active && "page headers do not nest");
    const facet::Theme& t = facet::T();
    ImGui::PushID(title);
    g_header.active = true;
    g_header.description = description;
    g_header.start = ImGui::GetCursorScreenPos();
    g_header.width = ImGui::GetContentRegionAvail().x;
    g_header.actions_id = ImGui::GetID("##header-actions");

    const float control_h = facet::ControlHeight(facet::ControlSize::Inherit);
    const float title_h = facet::MeasureText(kHeaderTitleStyle, "Ag").y;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const float actions_w = storage->GetFloat(g_header.actions_id, 0.0F);
    // The row grows to the control height only when there are actions to center on it.
    g_header.row_height = actions_w > 0.0F ? std::max(control_h, title_h) : title_h;
    const float gap = facet::Px(t.spacing.md);
    const float title_room = std::max(1.0F, g_header.width - (actions_w > 0.0F ? actions_w + gap : 0.0F));

    ImGui::SetCursorScreenPos(ImVec2(g_header.start.x, g_header.start.y + (g_header.row_height - title_h) * 0.5F));
    facet::TruncatedText(title, {.style = kHeaderTitleStyle, .width = title_room / facet::Scale()});

    g_header.actions_start = ImVec2(g_header.start.x + g_header.width - actions_w,
                                    g_header.start.y + (g_header.row_height - control_h) * 0.5F);
    ImGui::SetCursorScreenPos(g_header.actions_start);
    ImGui::BeginGroup();
}

void end_page_header() {
    if (!g_header.active) {
        return;
    }
    const facet::Theme& t = facet::T();
    ImGui::EndGroup();
    const float actions_w = std::max(0.0F, ImGui::GetItemRectMax().x - g_header.actions_start.x);
    ImGui::GetStateStorage()->SetFloat(g_header.actions_id, actions_w);
    g_layout_debug.header_frame = ImGui::GetFrameCount();
    g_layout_debug.header_left = g_header.start.x;
    g_layout_debug.header_right = g_header.start.x + g_header.width;
    g_layout_debug.header_actions_right = actions_w > 0.0F ? ImGui::GetItemRectMax().x : 0.0F;

    // The description sits under the title line (spacing.xs, no item spacing), so title and
    // description read as one block. It is Body, the reading size, in the secondary label color,
    // and wraps under the actions when the row is narrow. spacing.xl separates the block from the
    // page, one step more than the gaps inside a page, so the page content does not start cramped.
    // The title row is measured from the title's own height; with actions it is the control
    // height, and the description starts right under the title text, not under the buttons.
    const float title_h = facet::MeasureText(kHeaderTitleStyle, "Ag").y;
    const float title_bottom = g_header.start.y + (g_header.row_height + title_h) * 0.5F;
    float bottom = g_header.start.y + g_header.row_height;
    if (g_header.description != nullptr && g_header.description[0] != '\0') {
        ImGui::SetCursorScreenPos(ImVec2(g_header.start.x, std::round(title_bottom + facet::Px(t.spacing.xs))));
        facet::StyledText(g_header.description,
                          {.secondary = true, .wrap = true, .width = g_header.width / facet::Scale()});
        bottom = std::max(bottom, ImGui::GetItemRectMax().y);
    }
    ImGui::SetCursorScreenPos(ImVec2(g_header.start.x, bottom));
    ImGui::Dummy(ImVec2(0.0F, std::max(0.0F, facet::Px(kPageBlockGap()) - ImGui::GetStyle().ItemSpacing.y)));
    g_header.active = false;
    ImGui::PopID();
}

void page_header(const char* title, const char* description) {
    begin_page_header(title, description);
    end_page_header();
}

float page_column_width() { return ImGui::GetContentRegionAvail().x; }

float column_scroll_gutter() {
    const facet::ScrollTokens& s = facet::T().scroll;
    return std::round(facet::Px(s.barInset) + facet::Px(s.barThicknessHover) + facet::Px(s.barGap));
}

bool begin_column_scroll(const char* id, float width, float height) {
    if (g_column_depth >= kColumnDepth) {
        IM_ASSERT(false && "column scrolls nest at most four deep");
        return false;
    }
    ImGuiWindow* parent = ImGui::GetCurrentWindow();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float column_w = std::max(1.0F, width > 0.0F ? facet::Px(width) : avail.x);
    const float column_h = std::max(1.0F, height > 0.0F ? facet::Px(height) : avail.y);
    const float inset = column_inset();
    const float gutter = column_scroll_gutter();
    // The area starts one inset up and to the left of the column, so its padded content starts on
    // the column's top-left corner, and ends one gutter right of the column. While the content
    // overflows, Facet moves the layout's right edge in by that gutter (padding included), which is
    // then exactly the column's right edge, and the bar runs in the strip right of the column. While
    // it fits, the right edge is pinned to the column below, so the content edges never depend on
    // overflow, and Facet's trial layouts (KeepsBarGutter) see the same width as every other frame.
    ImGui::SetCursorScreenPos(ImVec2(start.x - inset, start.y - inset));
    ColumnFrame& f = g_columns[g_column_depth];
    f = ColumnFrame{};
    f.column_left = start.x;
    f.column_right = start.x + column_w;
    f.parent = parent;
    f.parent_max_x = parent->DC.CursorMaxPos.x;
    const float area_w = column_w + inset + gutter;
    const float area_h = column_h + inset;
    if (!facet::BeginScrollArea(id, {.width = area_w / facet::Scale(), .height = area_h / facet::Scale()})) {
        parent->DC.CursorMaxPos.x = std::max(f.parent_max_x, std::min(parent->DC.CursorMaxPos.x, f.column_right));
        return false;
    }
    ImGuiWindow* area = ImGui::GetCurrentWindow();
    f.area = area;
    ++g_column_depth;

    // What Facet did, for the selftest: its padding and, while the content overflows, its gutter.
    const int frame = ImGui::GetFrameCount();
    if (g_column_debug_frame != frame) {
        g_column_debug.clear();
        g_column_debug_frame = frame;
    }
    ColumnScrollDebug d;
    std::snprintf(d.id, sizeof(d.id), "%s", id);
    d.frame = frame;
    d.column_left = f.column_left;
    d.column_right = f.column_right;
    d.area_right = area->Pos.x + area->Size.x;
    d.overflows = area->ScrollMax.y > 0.0F;
    d.facet_inset = area->WindowPadding.x;
    d.facet_gutter = d.area_right - area->WorkRect.Max.x;  // the padding alone while it fits
    d.inset = inset;
    d.gutter = gutter;
    d.scroll_y = area->Scroll.y;
    d.scroll_max = area->ScrollMax.y;
    d.rect_min = area->Pos;
    d.rect_max = ImVec2(area->Pos.x + area->Size.x, area->Pos.y + area->Size.y);
    f.debug_index = static_cast<int>(g_column_debug.size());
    g_column_debug.push_back(d);

    area->WorkRect.Max.x = f.column_right;
    area->ContentRegionRect.Max.x = f.column_right;
    area->ParentWorkRect.Max.x = f.column_right;
    // Only x is corrected (when Facet's padding differs from the formula); y is the scrolled
    // position.
    const ImVec2 inner = ImGui::GetCursorScreenPos();
    g_column_debug.back().content_top = inner.y;
    if (inner.x != start.x) {
        ImGui::SetCursorScreenPos(ImVec2(start.x, inner.y));
    }
    return true;
}

void end_column_scroll() {
    if (g_column_depth == 0) {
        return;
    }
    ColumnFrame& f = g_columns[--g_column_depth];
    if (f.debug_index >= 0 && f.debug_index < static_cast<int>(g_column_debug.size()) &&
        g_column_debug_frame == ImGui::GetFrameCount() && f.area == ImGui::GetCurrentWindow()) {
        g_column_debug[static_cast<std::size_t>(f.debug_index)].content_max_x = f.area->DC.CursorMaxPos.x;
    }
    facet::EndScrollArea();
    // The area reaches one gutter past the column; that strip holds only the bar and must not count
    // as content of the region around it, or the region could scroll sideways by keyboard navigation.
    ImGuiWindow* parent = ImGui::GetCurrentWindow();
    parent->DC.CursorMaxPos.x = std::max(f.parent_max_x, std::min(parent->DC.CursorMaxPos.x, f.column_right));
}

const std::vector<ColumnScrollDebug>& column_scroll_debug() { return g_column_debug; }

bool begin_page_body(const char* id) {
    IM_ASSERT(g_body_depth < 0 && "page bodies do not nest");
    const int depth = g_column_depth;
    if (!begin_column_scroll(id)) {
        return false;
    }
    g_body_depth = depth;
    const ColumnScrollDebug& d = g_column_debug.back();
    g_layout_debug.body_frame = ImGui::GetFrameCount();
    g_layout_debug.body_content_top = d.content_top;
    g_layout_debug.body_scroll_y = d.scroll_y;
    g_layout_debug.body_scroll_max = d.scroll_max;
    g_layout_debug.body_rect = {d.rect_min, d.rect_max};
    return true;
}

void end_page_body() {
    if (g_body_depth < 0) {
        return;
    }
    while (g_column_depth > g_body_depth) {
        end_column_scroll();
    }
    g_body_depth = -1;
}

bool begin_page_card(const char* id, const char* title, const char* description) {
    ImGui::PushID(id);
    facet::BeginCard("##page-card");
    const bool has_title = title != nullptr && title[0] != '\0';
    const bool has_description = description != nullptr && description[0] != '\0';
    if (has_title || has_description) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
        if (has_title) {
            facet::StyledText(title, {.style = facet::TextStyle::Headline, .wrap = true});
        }
        if (has_title && has_description) {
            ImGui::Dummy(ImVec2(0.0F, std::round(facet::Px(kCardTitleGap()))));
        }
        if (has_description) {
            facet::StyledText(description, {.secondary = true, .wrap = true});
        }
        ImGui::Dummy(ImVec2(0.0F, std::round(facet::Px(kCardHeadingGap()))));
        ImGui::PopStyleVar();
    }
    return true;
}

void end_page_card() {
    facet::EndCard();
    ImGui::PopID();
    if (g_layout_debug.card_frame != ImGui::GetFrameCount()) {
        g_layout_debug.card_frame = ImGui::GetFrameCount();
        g_layout_debug.card_left = ImGui::GetItemRectMin().x;
        g_layout_debug.card_right = ImGui::GetItemRectMax().x;
    }
}

const PageLayoutDebug& page_layout_debug() { return g_layout_debug; }

void page_block_gap() {
    ImGui::Dummy(ImVec2(0.0F, std::max(0.0F, facet::Px(kPageBlockGap()) - ImGui::GetStyle().ItemSpacing.y)));
}

facet::ToastId show_action_toast(const char* text, const facet::ToastOptions& options,
                                 std::function<void()> on_action) {
    const facet::ToastId id = facet::ShowToast(text, options);
    if (id.IsValid() && on_action) {
        g_routed_toasts.push_back({id, std::move(on_action), ImGui::GetFrameCount()});
    }
    return id;
}

void poll_toast_actions() {
    const int frame = ImGui::GetFrameCount();
    for (std::size_t i = 0; i < g_routed_toasts.size();) {
        RoutedToast& toast = g_routed_toasts[i];
        if (facet::ToastActionPressed(toast.id)) {
            // Moved out first: the action may show another routed toast, which grows the vector.
            const std::function<void()> action = std::move(toast.on_action);
            g_routed_toasts.erase(g_routed_toasts.begin() + static_cast<std::ptrdiff_t>(i));
            action();
            continue;
        }
        // A toast is queued in the frame that shows it and drawn at the end of that frame, so it
        // gets a few frames before "not visible" means it is gone for good.
        if (frame - toast.shown_frame > 2 && !facet::IsToastVisible(toast.id)) {
            g_routed_toasts.erase(g_routed_toasts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void steam_not_found_state(stc::app::AppState& state) {
    const float height = ImGui::GetContentRegionAvail().y / facet::Scale();
    facet::EmptyStateOptions options;
    options.message =
        "Steam Tracer Cleaner looks for Steam in the registry and in its default install folder. "
        "Install Steam or start it once, then re-detect.";
    options.icon = facet::Icon("warning");
    options.actionLabel = "Re-detect Steam";
    options.height = std::max(0.0F, height);
    if (facet::EmptyState("Steam was not found###steam-not-found", options)) {
        state.refresh_steam();
        if (state.install) {
            facet::ShowToast("Steam found", {.tone = facet::Tone::Success});
        } else {
            facet::ShowToast("Steam is still not found", {.tone = facet::Tone::Warning});
        }
    }
}

}  // namespace stc::ui
