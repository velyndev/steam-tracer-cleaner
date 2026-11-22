#include "app/capture.hpp"

#include <windows.h>

#include <d3d11.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_internal.h>

#include <facet/facet.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_state.hpp"
#include "app/fixture.hpp"
#include "core/dry_run.hpp"
#include "core/targets.hpp"
#include "core/log.hpp"
#include "ui/app_shell.hpp"
#include "ui/appearance.hpp"
#include "ui/assets.hpp"
#include "ui/common.hpp"

namespace stc::app {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kFrameTime = 1.0F / 60.0F;
constexpr int kDefaultWidth = 1240;
constexpr int kDefaultHeight = 820;
constexpr int kWarmupFrames = 45;
constexpr int kSettleFrames = 45;
constexpr double kJobTimeoutSeconds = 30.0;

constexpr const char* kPageIds[kPageCount] = {"cleanup", "backups", "exclusions", "audit", "configs", "settings"};

// --- Output ----------------------------------------------------------------------------------------

// Every line goes to stdout and, when open, to the log file.
class DevLog {
public:
    ~DevLog() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }
    bool open(const std::filesystem::path& path) {
        file_ = _wfopen(path.c_str(), L"w");
        return file_ != nullptr;
    }
    void line(const char* fmt, ...) IM_FMTARGS(2) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        std::fprintf(stdout, "%s\n", buf);
        std::fflush(stdout);
        if (file_ != nullptr) {
            std::fprintf(file_, "%s\n", buf);
            std::fflush(file_);
        }
    }

private:
    FILE* file_ = nullptr;
};

// A WIN32-subsystem program has no console: attach to the parent's, unless stdout is already
// redirected to a file or pipe.
void connect_stdout() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

bool write_png(const std::filesystem::path& path, std::vector<unsigned char> rgba, int width, int height) {
    // RGBA to opaque BGRA, the WIC PNG encoder's native format.
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        std::swap(rgba[i], rgba[i + 2]);
        rgba[i + 3] = 255;
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA) {
        return false;
    }
    const UINT stride = static_cast<UINT>(width) * 4U;
    return SUCCEEDED(frame->WritePixels(static_cast<UINT>(height), stride, static_cast<UINT>(rgba.size()), rgba.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// --- Options ---------------------------------------------------------------------------------------

enum class ThemePick { Light, Dark, Both };

struct Options {
    bool capture = false;
    bool selftest = false;
    std::filesystem::path directory;
    std::filesystem::path log_file;
    std::optional<int> page;  // index into kPageIds
    ThemePick theme = ThemePick::Both;
    bool theme_given = false;
    int width = kDefaultWidth;
    int height = kDefaultHeight;
    bool size_given = false;
    float scale = 1.0F;
    bool scale_given = false;
    bool no_steam = false;
    std::string dev_state;          // --state
    std::filesystem::path fixture;  // --fixture, absolute
    bool warp = false;
};

std::string narrow(const wchar_t* text) { return ui::to_utf8(text); }

bool parse_options(int argc, wchar_t** argv, Options& o, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto value = [&]() -> const wchar_t* {
            if (i + 1 >= argc) {
                error = narrow(arg.c_str()) + " needs a value";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == L"--capture") {
            const wchar_t* v = value();
            if (v == nullptr) return false;
            o.capture = true;
            o.directory = v;
        } else if (arg == L"--selftest") {
            o.selftest = true;
        } else if (arg == L"--page") {
            const wchar_t* v = value();
            if (v == nullptr) return false;
            const std::string id = narrow(v);
            for (int p = 0; p < kPageCount; ++p) {
                if (id == kPageIds[p]) o.page = p;
            }
            if (!o.page) {
                error = "unknown page '" + id + "' (cleanup, backups, exclusions, audit, configs, settings)";
                return false;
            }
        } else if (arg == L"--theme") {
            const wchar_t* v = value();
            if (v == nullptr) return false;
            const std::wstring t = v;
            if (t == L"light") o.theme = ThemePick::Light;
            else if (t == L"dark") o.theme = ThemePick::Dark;
            else if (t == L"both") o.theme = ThemePick::Both;
            else {
                error = "--theme expects light, dark or both";
                return false;
            }
            o.theme_given = true;
        } else if (arg == L"--size") {
            const wchar_t* v = value();
            if (v == nullptr) return false;
            int w = 0;
            int h = 0;
            if (std::swscanf(v, L"%dx%d", &w, &h) != 2 || w < 320 || h < 240 || w > 8192 || h > 8192) {
                error = "--size expects WxH, for example 760x520";
                return false;
            }
            o.width = w;
            o.height = h;
            o.size_given = true;
        } else if (arg == L"--scale") {
            const wchar_t* v = value();
            if (v == nullptr) return false;
            wchar_t* end = nullptr;
            const float s = std::wcstof(v, &end);
            if (end == v || *end != 0 || !(s >= kMinUiScale && s <= kMaxUiScale)) {
                error = "--scale expects a number from 0.75 to 2";
                return false;
            }
            o.scale = s;
            o.scale_given = true;
        } else if (arg == L"--state") {
            const wchar_t* v = value();
            if (v == nullptr) return false;
            o.dev_state = narrow(v);
            const bool valid = !o.dev_state.empty() && o.dev_state.size() <= 32 &&
                               std::all_of(o.dev_state.begin(), o.dev_state.end(), [](char c) {
                                   return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                               });
            if (!valid) {
                error = "--state expects a name of lowercase letters, digits and '-', for example running";
                return false;
            }
        } else if (arg == L"--fixture") {
            const wchar_t* v = value();
            if (v == nullptr) return false;
            std::error_code ec;
            o.fixture = std::filesystem::absolute(v, ec).lexically_normal();
            if (ec) {
                error = "--fixture: cannot resolve the path";
                return false;
            }
        } else if (arg == L"--no-steam") {
            o.no_steam = true;
        } else if (arg == L"--warp") {
            o.warp = true;
        } else if (arg == L"--log") {
            const wchar_t* v = value();
            if (v == nullptr) return false;
            o.log_file = v;
        } else {
            error = "unknown argument '" + narrow(arg.c_str()) + "'";
            return false;
        }
    }
    if (o.capture && o.selftest) {
        error = "--capture and --selftest cannot be combined";
        return false;
    }
    if (o.selftest && (o.page || o.theme_given)) {
        error = "--selftest visits every page and both themes; --page and --theme do not apply";
        return false;
    }
    if (o.selftest && !o.dev_state.empty()) {
        error = "--state applies to --capture only";
        return false;
    }
    if (o.no_steam && !o.fixture.empty()) {
        error = "--no-steam and --fixture cannot be combined";
        return false;
    }
    if (o.capture && !o.log_file.empty()) {
        error = "--capture writes capture.log into its directory; --log does not apply";
        return false;
    }
    return true;
}

// --- Offscreen rendering ---------------------------------------------------------------------------

// The host of an offscreen frame: no window, nothing to minimize or close.
class NullHost final : public ui::HostWindow {
public:
    void minimize() override {}
    void toggle_maximize() override {}
    void request_close() override {}
    bool maximized() const override { return false; }
    void* native_handle() const override { return nullptr; }
    void set_caption_blocked(bool) override {}
};

class Offscreen {
public:
    ~Offscreen() { end(); }

    bool create_device(bool warp, DevLog& log) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = E_FAIL;
        for (const D3D_DRIVER_TYPE type : {warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            hr = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, 1, D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr,
                                   context_.GetAddressOf());
            if (SUCCEEDED(hr)) {
                log.line("device: %s", type == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware");
                return true;
            }
        }
        log.line("error: D3D11CreateDevice failed (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    // A fresh Dear ImGui context and Facet session at width x height pixels.
    bool begin(int width, int height) {
        end();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, texture_.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateRenderTargetView(texture_.Get(), nullptr, view_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        width_ = width;
        height_ = height;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        active_ = true;
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // read-only: no ui.ini
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
        io.ConfigInputTextCursorBlink = false;
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        io.DeltaTime = kFrameTime;
        io.AddFocusEvent(true);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        // Recoverable Dear ImGui and Facet errors (unbalanced Begin/End, bad ids) are counted and
        // reported instead of asserting, so a dev run lists them and keeps going.
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        errors_.clear();
        ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext*, void* user, const char* msg) {
            auto* list = static_cast<std::vector<std::string>*>(user);
            if (list->size() < 64) {
                list->emplace_back(msg != nullptr ? msg : "");
            }
        };
        ImGui::GetCurrentContext()->ErrorCallbackUserData = &errors_;

        facet::SetScaleMode(facet::ScaleMode::FollowDpi);
        facet::SetDpiScale(1.0F);
        facet::SetFollowSystemMotion(false);
        facet::SyncImGuiStyle(true);
        facet::ApplyImGuiStyle();
        if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
            return false;
        }
        backend_ = true;
        facet::InitDesc init;
        init.device = device_.Get();
        init.context = context_.Get();
        if (!facet::Init(init)) {
            return false;
        }
        ui::assets::load();
        ui::reset_appearance();
        return true;
    }

    void resize_display(int width, int height) {
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    }

    void frame(AppState& state) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = kFrameTime;
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        facet::NewFrame();
        ui::draw_app(state, host_);
        facet::EndFrame();
        ImGui::Render();

        const ImVec4& bg = facet::T().colors.bg;
        const float clear[4] = {bg.x, bg.y, bg.z, 1.0F};
        ID3D11RenderTargetView* views[] = {view_.Get()};
        context_->OMSetRenderTargets(1, views, nullptr);
        context_->ClearRenderTargetView(view_.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    void frames(AppState& state, int count) {
        for (int i = 0; i < count; ++i) {
            frame(state);
        }
    }

    // Renders until no background job runs (or the timeout passes). Returns false on timeout.
    bool settle_jobs(AppState& state) {
        const auto start = std::chrono::steady_clock::now();
        while (!state.tasks.idle()) {
            frame(state);
            Sleep(1);
            const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed.count() > kJobTimeoutSeconds) {
                return false;
            }
        }
        frame(state);  // the frame that polls the last completion
        return true;
    }

    bool readback(std::vector<unsigned char>& rgba) {
        context_->CopyResource(staging_.Get(), texture_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        const std::size_t row = static_cast<std::size_t>(width_) * 4U;
        rgba.resize(row * static_cast<std::size_t>(height_));
        for (int y = 0; y < height_; ++y) {
            std::memcpy(rgba.data() + row * static_cast<std::size_t>(y),
                        static_cast<const unsigned char*>(mapped.pData) + static_cast<std::size_t>(mapped.RowPitch) * static_cast<std::size_t>(y),
                        row);
        }
        context_->Unmap(staging_.Get(), 0);
        return true;
    }

    // Facet first, then the backend, then the context.
    void end() {
        if (!active_) {
            return;
        }
        ui::assets::release();
        facet::Shutdown();
        if (backend_) {
            ImGui_ImplDX11_Shutdown();
        }
        backend_ = false;
        ImGui::DestroyContext();
        active_ = false;
        if (context_) {
            context_->ClearState();
            context_->Flush();
        }
    }

    // Recoverable errors reported since begin().
    const std::vector<std::string>& errors() const { return errors_; }

    int width() const { return width_; }
    int height() const { return height_; }

private:
    NullHost host_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11RenderTargetView> view_;
    ComPtr<ID3D11Texture2D> staging_;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::string> errors_;
    bool active_ = false;
    bool backend_ = false;
};

bool init_state(AppState& state, const Options& o, DevLog& log) {
    state.read_only = true;
    state.offline = true;
    state.simulate_no_steam = o.no_steam;
    state.dev_state = o.dev_state;
    if (!o.fixture.empty()) {
        std::string error;
        int written = 0;
        if (!ensure_fixture(o.fixture, &error, &written)) {
            log.line("error: fixture: %s", error.c_str());
            return false;
        }
        log.line("fixture: %s (%d file(s) written, %s)", ui::path_to_utf8(o.fixture).c_str(), written,
                 written == 0 ? "reused" : "created or completed");
        state.fixture_root = o.fixture;
    }
    try {
        state.initialize();
    } catch (const std::exception& e) {
        log.line("error: AppState init failed: %s", e.what());
        return false;
    }
    state.ui_scale = o.scale;
    log.line("steam: %s", state.install ? "found" : "not found");
    if (!o.fixture.empty() && state.install) {
        // What the fixture gives the planner: every target, with the fixture's ignore list.
        // Read-only, like the plan building of the Cleanup page.
        std::vector<std::string> ids;
        for (const auto& target : stc::core::all_targets()) {
            ids.push_back(target.id);
        }
        stc::core::ResolveContext ctx{*state.install, state.accounts, state.libraries};
        stc::core::PlanOptions plan_options;
        plan_options.ignore = &state.ignore_list;
        const auto plan = stc::core::build_plan_by_ids(ids, ctx, plan_options);
        std::size_t with_steps = 0;
        for (const auto& id : ids) {
            with_steps += std::ranges::any_of(plan.steps, [&](const auto& s) { return s.target_id == id; }) ? 1 : 0;
        }
        log.line("fixture: %zu accounts, %zu libraries; plan over all %zu targets: %zu steps, %zu targets with steps, "
                 "%s, %llu files",
                 state.accounts.size(), state.libraries.size(), ids.size(), plan.steps.size(), with_steps,
                 ui::format_bytes(plan.total_bytes).c_str(), static_cast<unsigned long long>(plan.total_file_count));
    }
    return true;
}

// --- Capture ---------------------------------------------------------------------------------------

int run_capture(const Options& o) {
    std::error_code ec;
    std::filesystem::create_directories(o.directory, ec);
    DevLog log;
    if (ec || !log.open(o.directory / "capture.log")) {
        std::fprintf(stderr, "error: cannot write into %s\n", ui::path_to_utf8(o.directory).c_str());
        return 1;
    }
    stc::core::log::init(o.directory, "capture-app");

    Offscreen screen;
    if (!screen.create_device(o.warp, log)) {
        return 1;
    }
    AppState state;
    if (!init_state(state, o, log)) {
        return 1;
    }

    std::string suffix;
    if (o.size_given) suffix += "-" + std::to_string(o.width) + "x" + std::to_string(o.height);
    if (o.scale_given) {
        char s[16];
        std::snprintf(s, sizeof(s), "@%g", static_cast<double>(o.scale));
        suffix += s;
    }
    if (o.no_steam) suffix += "-nosteam";
    if (!o.fixture.empty()) suffix += "-fx";
    if (!o.dev_state.empty()) suffix += "-state-" + o.dev_state;

    std::vector<bool> themes;
    if (o.theme != ThemePick::Light) themes.push_back(true);
    if (o.theme != ThemePick::Dark) themes.push_back(false);

    int failures = 0;
    std::vector<unsigned char> pixels;
    for (int p = 0; p < kPageCount; ++p) {
        if (o.page && *o.page != p) {
            continue;
        }
        for (const bool dark : themes) {
            const std::string name = std::string(kPageIds[p]) + (dark ? "-dark" : "-light") + suffix + ".png";
            if (!screen.begin(o.width, o.height)) {
                log.line("FAIL %s: offscreen session did not start", name.c_str());
                ++failures;
                continue;
            }
            state.current_page = static_cast<Page>(p);
            state.theme_mode = dark ? ThemeChoice::Dark : ThemeChoice::Light;
            screen.frames(state, kWarmupFrames);
            if (!screen.settle_jobs(state)) {
                log.line("warning %s: background jobs still running after %.0f s", name.c_str(), kJobTimeoutSeconds);
            }
            screen.frames(state, kSettleFrames);
            const std::filesystem::path path = o.directory / name;
            if (screen.readback(pixels) && write_png(path, pixels, screen.width(), screen.height())) {
                log.line("wrote %s", ui::path_to_utf8(path).c_str());
            } else {
                log.line("FAIL %s: readback or PNG write failed", name.c_str());
                ++failures;
            }
            for (const std::string& message : screen.errors()) {
                log.line("FAIL %s: Dear ImGui / Facet error: %s", name.c_str(), message.c_str());
            }
            if (!screen.errors().empty()) {
                ++failures;
            }
            screen.end();
        }
    }
    state.tasks.shutdown();
    log.line("capture: %s", failures == 0 ? "ok" : "FAILED");
    stc::core::log::flush_all();
    return failures == 0 ? 0 : 1;
}

// --- Selftest --------------------------------------------------------------------------------------

class Selftest {
public:
    Selftest(Offscreen& screen, AppState& state, DevLog& log) : screen_(screen), state_(state), log_(log) {}

    bool check(bool condition, const char* what) {
        log_.line("%s: %s", condition ? "pass" : "FAIL", what);
        return condition;
    }

    bool errors_ok(const char* what) {
        for (const std::string& message : screen_.errors()) {
            log_.line("  error: %s", message.c_str());
        }
        return check(screen_.errors().empty(), what);
    }

    // The page column: the first page card of the last frame (when the page drew one) starts at
    // the header's left edge and ends at the header's right edge, which the trailing actions end
    // at too, whether the page body overflows or not.
    bool edges_ok(const char* page) {
        const ui::PageLayoutDebug& d = ui::page_layout_debug();
        const int last = ImGui::GetFrameCount() - 1;
        char what[160];
        if (d.header_frame < last) {
            std::snprintf(what, sizeof(what), "'%s' draws the page header", page);
            return check(false, what);
        }
        bool ok = true;
        if (d.header_actions_right > 0.0F) {
            std::snprintf(what, sizeof(what), "'%s': the header actions end at the column's right edge", page);
            ok = check(std::abs(d.header_actions_right - d.header_right) <= 0.5F, what) && ok;
        }
        if (d.card_frame >= last) {
            log_.line("  %s: header %.1f .. %.1f, first card %.1f .. %.1f", page, static_cast<double>(d.header_left),
                      static_cast<double>(d.header_right), static_cast<double>(d.card_left), static_cast<double>(d.card_right));
            std::snprintf(what, sizeof(what), "'%s': the first card spans the header's column", page);
            ok = check(std::abs(d.card_left - d.header_left) <= 0.5F && std::abs(d.card_right - d.header_right) <= 0.5F, what) && ok;
        } else {
            log_.line("  %s: no page card drawn", page);
        }
        return ok;
    }

    // The column scrolls of the last frame: Facet's padding and gutter match the app's formulas, and
    // the Configs detail column ends on the card content's right edge with nothing drawn in its bar
    // strip. Returns whether the detail column overflowed (through *overflowed).
    bool columns_ok(const char* where, bool* overflowed = nullptr) {
        // Between frames GetFrameCount() is the number of the frame drawn last.
        const int last = ImGui::GetFrameCount();
        const float pad = facet::Px(facet::T().spacing.lg);
        const ui::PageLayoutDebug& page = ui::page_layout_debug();
        bool ok = true;
        bool detail_seen = false;
        char what[200];
        for (const ui::ColumnScrollDebug& c : ui::column_scroll_debug()) {
            if (c.frame != last) {
                continue;
            }
            log_.line("  %s: column '%s' %.1f .. %.1f, area right %.1f, content right %.1f, %s, inset %.1f/%.1f, edge %.1f/%.1f",
                      where, c.id, static_cast<double>(c.column_left), static_cast<double>(c.column_right),
                      static_cast<double>(c.area_right), static_cast<double>(c.content_max_x),
                      c.overflows ? "overflows" : "fits", static_cast<double>(c.facet_inset), static_cast<double>(c.inset),
                      static_cast<double>(c.facet_gutter), static_cast<double>(c.overflows ? c.gutter : c.inset));
            std::snprintf(what, sizeof(what), "%s, '%s': Facet's padding and bar gutter match the app's formulas", where, c.id);
            const float edge = c.overflows ? c.gutter : c.inset;
            ok = check(std::abs(c.facet_inset - c.inset) <= 0.5F && std::abs(c.facet_gutter - edge) <= 0.5F &&
                           std::abs(c.area_right - c.gutter - c.column_right) <= 0.5F,
                       what) && ok;
            if (std::string_view(c.id) == "##detail-scroll") {
                detail_seen = true;
                if (overflowed != nullptr) *overflowed = c.overflows;
                std::snprintf(what, sizeof(what), "%s: the Configs detail ends on the card content's right edge (%s)", where,
                              c.overflows ? "scrolling" : "not scrolling");
                ok = check(page.card_frame >= last && std::abs(c.column_right - (page.card_right - pad)) <= 0.5F, what) && ok;
                std::snprintf(what, sizeof(what), "%s: nothing in the Configs detail reaches into its bar strip", where);
                ok = check(c.content_max_x > c.column_left && c.content_max_x <= c.column_right + 0.5F, what) && ok;
            }
        }
        if (overflowed != nullptr) {
            std::snprintf(what, sizeof(what), "%s: the Configs detail column is drawn", where);
            ok = check(detail_seen, what) && ok;
        }
        return ok;
    }

    // The fixture's config library has entries, so Configs shows its detail column.
    bool with_library() const { return state_.install && !state_.fixture_root.empty(); }

    void click(ImVec2 at) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        screen_.frame(state_);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        screen_.frame(state_);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        screen_.frame(state_);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        screen_.frames(state_, 20);
    }

    void key(ImGuiKey k, bool ctrl = false) {
        ImGuiIO& io = ImGui::GetIO();
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(k, true);
        screen_.frame(state_);
        io.AddKeyEvent(k, false);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        screen_.frames(state_, 20);
    }

    void type(const char* text) {
        ImGui::GetIO().AddInputCharactersUTF8(text);
        screen_.frames(state_, 10);
    }

    int run() {
        screen_.frames(state_, kWarmupFrames);

        // The chrome: no status bar under the content (the sidebar column ends one inset above the
        // window's bottom edge), and a flat sidebar (the five page items follow each other at one
        // pitch, with no section header between them).
        {
            const ui::ShellDebug& d = ui::shell_debug();
            const float inset = facet::Px(facet::T().spacing.sm);
            const float under = d.window_bottom - d.sidebar_bottom;
            if (!check(under >= 0.0F && under <= inset + 1.0F, "the sidebar runs to the bottom of the window (no status bar)")) return 1;
            const float pitch = d.sidebar_items[1].min.y - d.sidebar_items[0].min.y;
            bool flat = pitch > 0.0F;
            for (int i = 1; i + 1 < kPageCount - 1; ++i) {
                const float step = d.sidebar_items[i + 1].min.y - d.sidebar_items[i].min.y;
                flat = flat && std::abs(step - pitch) <= 1.0F;
            }
            if (!check(flat, "the sidebar is one flat list (Cleanup, Backups, Exclusions, Audit, Configs)")) return 1;
            if (!check(d.sidebar_items[static_cast<int>(Page::Settings)].min.y > d.sidebar_items[static_cast<int>(Page::Configs)].max.y + pitch,
                       "Settings is pinned at the bottom of the sidebar")) return 1;
            if (!check(screen_.settle_jobs(state_), "the start page's jobs finish")) return 1;
            screen_.frames(state_, 5);
            if (!check(!d.activity.valid(), "no activity indicator while nothing runs")) return 1;
        }

        // Every sidebar item, in an order where each click changes the page.
        constexpr Page order[] = {Page::Backups, Page::Exclusions, Page::Audit, Page::Configs, Page::Settings, Page::Cleanup};
        for (const Page page : order) {
            const ui::ShellRect rect = ui::shell_debug().sidebar_items[static_cast<int>(page)];
            char what[96];
            std::snprintf(what, sizeof(what), "sidebar item '%s' is laid out", kPageIds[static_cast<int>(page)]);
            if (!check(rect.valid(), what)) return 1;
            click(rect.center());
            std::snprintf(what, sizeof(what), "a click on '%s' opens that page", kPageIds[static_cast<int>(page)]);
            if (!check(state_.current_page == page, what)) return 1;
            // The page's background work (plan build, audit, backup listing) ends, and drawing it
            // reports no recoverable Dear ImGui or Facet error.
            std::snprintf(what, sizeof(what), "the jobs of '%s' finish", kPageIds[static_cast<int>(page)]);
            if (!check(screen_.settle_jobs(state_), what)) return 1;
            screen_.frames(state_, 10);
            std::snprintf(what, sizeof(what), "'%s' reports no Dear ImGui or Facet error", kPageIds[static_cast<int>(page)]);
            if (!errors_ok(what)) return 1;
            if (!edges_ok(kPageIds[static_cast<int>(page)])) return 1;
            if (page == Page::Configs && with_library()) {
                if (!columns_ok("configs at the start size", &configs_overflow_[0])) return 1;
            } else if (!columns_ok(kPageIds[static_cast<int>(page)])) {
                return 1;
            }
        }

        // The task runner: a job that does one step, waits until the test releases it (at most ten
        // seconds), does two more and returns; its result is applied on the UI thread by poll().
        // While it waits, the title bar shows it with its progress.
        int delivered = 0;
        auto release = std::make_shared<std::atomic<bool>>(false);
        state_.tasks.submit<int>(
            JobKind::Other, "Selftest job",
            [release](JobContext& ctx) {
                ctx.set_total(3);
                ctx.advance();
                for (int waited = 0; waited < 1000 && !release->load() && !ctx.stop_requested(); ++waited) {
                    Sleep(10);
                }
                ctx.advance(2);
                return 42;
            },
            [&delivered](int value) { delivered = value; });
        if (!check(state_.tasks.is_running(JobKind::Other), "a submitted job reports as running")) return 1;
        screen_.frames(state_, 3);
        if (!check(!ui::shell_debug().activity.valid(), "a job that just started is not shown yet")) return 1;
        screen_.frames(state_, 40);  // past the indicator's delay (frames advance 1/60 s each)
        {
            const ui::ShellDebug& d = ui::shell_debug();
            log_.line("  activity: '%s'", d.activity_label.c_str());
            if (!check(d.activity.valid() && d.activity_label == "Selftest job 1 of 3",
                       "the title bar shows the running job with its progress")) return 1;
            if (!check(d.activity.max.x <= d.palette_button.min.x,
                       "the activity indicator sits before the command palette button")) return 1;
        }
        release->store(true);
        if (!check(screen_.settle_jobs(state_) && delivered == 42, "the job's result arrives through poll()")) return 1;
        screen_.frames(state_, 3);
        if (!check(!ui::shell_debug().activity.valid(), "the activity indicator is gone when the job is done")) return 1;

        key(ImGuiKey_K, true);
        if (!check(ui::shell_debug().palette_open, "Ctrl+K opens the command palette")) return 1;
        key(ImGuiKey_Escape);
        if (!check(!ui::shell_debug().palette_open, "Escape closes the command palette")) return 1;

        const ui::ShellRect button = ui::shell_debug().palette_button;
        if (!check(button.valid(), "the title bar has the command palette button")) return 1;
        click(button.center());
        if (!check(ui::shell_debug().palette_open, "the title bar button opens the command palette")) return 1;
        key(ImGuiKey_Escape);
        if (!check(!ui::shell_debug().palette_open, "Escape closes it again")) return 1;

        // Theme switches through palette commands (navigation-only: the settings save is a no-op in
        // read-only mode).
        key(ImGuiKey_K, true);
        type("dark theme");
        key(ImGuiKey_Enter);
        screen_.frames(state_, 30);
        if (!check(state_.theme_mode == ThemeChoice::Dark && facet::T().dark,
                   "the palette command 'Use the dark theme' switches to dark")) return 1;
        key(ImGuiKey_K, true);
        type("light theme");
        key(ImGuiKey_Enter);
        screen_.frames(state_, 30);
        if (!check(state_.theme_mode == ThemeChoice::Light && !facet::T().dark,
                   "the palette command 'Use the light theme' switches to light")) return 1;
        if (!check(!ui::shell_debug().palette_open, "the palette closes after a pick")) return 1;

        // The sidebar turns into the icon rail at the minimum window size.
        const int w = screen_.width();
        const int h = screen_.height();
        const float s = facet::Scale();
        screen_.resize_display(static_cast<int>(760.0F * s), static_cast<int>(520.0F * s));
        screen_.frames(state_, 30);
        if (!check(ui::shell_debug().sidebar_rail, "the sidebar is an icon rail at 760 x 520 units")) return 1;
        const ui::ShellRect settings = ui::shell_debug().sidebar_items[static_cast<int>(Page::Settings)];
        click(settings.center());
        if (!check(state_.current_page == Page::Settings, "a rail item still navigates")) return 1;
        if (!check(screen_.settle_jobs(state_), "the jobs of 'settings' finish at 760 x 520")) return 1;
        screen_.frames(state_, 10);
        if (!edges_ok("settings at 760 x 520")) return 1;
        // The page body scrolls: the wheel over it moves the content, not only the bar.
        {
            const ui::PageLayoutDebug& before = ui::page_layout_debug();
            const float top = before.body_content_top;
            log_.line("  settings body: scroll max %.0f px", static_cast<double>(before.body_scroll_max));
            if (before.body_frame >= ImGui::GetFrameCount() - 1 && before.body_scroll_max > 0.0F) {
                const ImVec2 at((before.body_rect.min.x + before.body_rect.max.x) * 0.5F,
                                (before.body_rect.min.y + before.body_rect.max.y) * 0.5F);
                ImGuiIO& io = ImGui::GetIO();
                io.AddMousePosEvent(at.x, at.y);
                screen_.frame(state_);
                io.AddMouseWheelEvent(0.0F, -3.0F);
                screen_.frames(state_, 40);
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                screen_.frames(state_, 2);
                const ui::PageLayoutDebug& after = ui::page_layout_debug();
                log_.line("  settings body: content top %.0f -> %.0f, scroll %.0f", static_cast<double>(top),
                          static_cast<double>(after.body_content_top), static_cast<double>(after.body_scroll_y));
                if (!check(after.body_scroll_y > 0.0F && after.body_content_top < top - 1.0F,
                           "the wheel scrolls the page body and its content moves")) return 1;
                if (!edges_ok("settings at 760 x 520, scrolled")) return 1;
            } else {
                log_.line("  note: the settings body does not overflow here; the wheel check is skipped");
            }
        }
        // Configs at the minimum size: the detail column scrolls there, and it must keep the same
        // right edge as when it fits.
        if (with_library()) {
            click(ui::shell_debug().sidebar_items[static_cast<int>(Page::Configs)].center());
            if (!check(state_.current_page == Page::Configs, "a rail item opens Configs at 760 x 520")) return 1;
            if (!check(screen_.settle_jobs(state_), "the jobs of 'configs' finish at 760 x 520")) return 1;
            screen_.frames(state_, 10);
            if (!edges_ok("configs at 760 x 520")) return 1;
            if (!columns_ok("configs at 760 x 520", &configs_overflow_[1])) return 1;
            if (!check(configs_overflow_[1], "the Configs detail column scrolls at 760 x 520")) return 1;
            log_.line("  configs detail: %s at the start size, %s at 760 x 520",
                      configs_overflow_[0] ? "scrolls" : "fits", configs_overflow_[1] ? "scrolls" : "fits");
        }
        screen_.resize_display(w, h);
        screen_.frames(state_, 30);
        if (w >= static_cast<int>(900.0F * s)) {
            if (!check(!ui::shell_debug().sidebar_rail, "the sidebar shows labels again at the start size")) return 1;
        }
        if (!errors_ok("the whole run reports no Dear ImGui or Facet error")) return 1;
        log_.line("selftest: ok");
        return 0;
    }

private:
    Offscreen& screen_;
    AppState& state_;
    DevLog& log_;
    bool configs_overflow_[2] = {false, false};  // the Configs detail scrolled at the start size, at 760 x 520
};

int run_selftest(const Options& o) {
    DevLog log;
    if (!o.log_file.empty() && !log.open(o.log_file)) {
        std::fprintf(stderr, "error: cannot write %s\n", ui::path_to_utf8(o.log_file).c_str());
        return 1;
    }
    // No log file: spdlog stays at its default sink, quieted to warnings.
    spdlog::set_level(spdlog::level::warn);

    Offscreen screen;
    if (!screen.create_device(o.warp, log)) {
        return 1;
    }
    AppState state;
    if (!init_state(state, o, log)) {
        return 1;
    }
    state.theme_mode = ThemeChoice::Dark;
    if (!screen.begin(o.width, o.height)) {
        log.line("FAIL: offscreen session did not start");
        return 1;
    }
    Selftest test(screen, state, log);
    const int result = test.run();
    screen.end();
    state.tasks.shutdown();
    if (result != 0) {
        log.line("selftest: FAILED");
    }
    return result;
}

}  // namespace

bool run_dev_mode(const wchar_t* command_line, int* exit_code) {
    if (command_line == nullptr ||
        (std::wcsstr(command_line, L"--capture") == nullptr && std::wcsstr(command_line, L"--selftest") == nullptr)) {
        return false;
    }
    connect_stdout();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Options options;
    std::string error;
    const bool parsed = argv != nullptr && parse_options(argc, argv, options, error);
    LocalFree(argv);
    if (!parsed) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        std::fprintf(stderr,
                     "usage:\n"
                     "  steam-tracer-cleaner --capture <dir> [--page id] [--theme light|dark|both] [--size WxH]\n"
                     "                       [--scale f] [--no-steam | --fixture dir] [--state name] [--warp]\n"
                     "  steam-tracer-cleaner --selftest [--size WxH] [--scale f] [--no-steam | --fixture dir] [--warp]\n"
                     "                       [--log file]\n");
        *exit_code = 2;
        return true;
    }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    *exit_code = options.capture ? run_capture(options) : run_selftest(options);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    return true;
}

}  // namespace stc::app
