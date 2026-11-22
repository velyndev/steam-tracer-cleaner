// Win32 + Direct3D 11 host for the Facet UI.
//
//   steam-tracer-cleaner                the app window
//   steam-tracer-cleaner --scheduled    headless Quick Clean for the scheduled task
//   steam-tracer-cleaner --capture ...  read-only offscreen captures (app/capture.hpp)
//   steam-tracer-cleaner --selftest ... read-only scripted navigation test (app/capture.hpp)
//
// The window is borderless: WM_NCCALCSIZE gives the whole window to the client area and Facet
// draws the title bar. It keeps the WS_OVERLAPPEDWINDOW style bits, so Windows still resizes it
// from the edges, drags and snaps it, and animates minimize and maximize. The non-client hit test
// is answered from facet::AppFrameHitTest; the maximize button answers HTMAXBUTTON so Windows
// shows its snap layouts over it.

#include <windows.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <facet/facet.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <functional>
#include <string>

#include "app/app_state.hpp"
#include "app/capture.hpp"
#include "core/cleaner.hpp"
#include "core/dry_run.hpp"
#include "core/log.hpp"
#include "core/profile.hpp"
#include "core/version.hpp"
#include "platform/paths.hpp"
#include "ui/app_shell.hpp"
#include "ui/appearance.hpp"
#include "ui/assets.hpp"
#include "ui/common.hpp"

// imgui_impl_win32.h keeps this declaration inside `#if 0` so its header doesn't drag in
// <windows.h>. Must stay in the global namespace; an anonymous-namespace forward decl gets
// internal linkage and the link fails.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"steam-tracer-cleaner";
constexpr wchar_t kWindowTitle[] = L"Steam Tracer Cleaner";
constexpr int kInitialWidth = 1240;   // units
constexpr int kInitialHeight = 820;
constexpr int kMinWidth = 760;        // units; used until the first frame reports AppFrameMinSize
constexpr int kMinHeight = 520;
constexpr const char* kPlacementId = "main-window";
constexpr int kAppIconResource = 101;  // IDI_APP_ICON in app.rc

int resize_border(UINT dpi) {
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// --- The window -------------------------------------------------------------------------------------

class Window final : public stc::ui::HostWindow {
public:
    bool create(HINSTANCE instance, const stc::app::AppState& state);
    void destroy();

    bool create_device();
    void release_device();
    bool create_back_buffer();
    // Resizes the swap chain when needed and binds and clears the back buffer. False when there
    // is nothing to draw (minimized, occluded or no device).
    bool begin_frame();
    // Renders the draw data and presents. False when the device was lost.
    bool end_frame();

    // Applies the saved placement (or the default one) and shows the window.
    void show(const stc::app::AppState& state);
    void save_placement();

    // stc::ui::HostWindow
    void minimize() override { PostMessageW(hwnd_, WM_SYSCOMMAND, SC_MINIMIZE, 0); }
    void toggle_maximize() override { PostMessageW(hwnd_, WM_SYSCOMMAND, maximized() ? SC_RESTORE : SC_MAXIMIZE, 0); }
    void request_close() override { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }
    bool maximized() const override { return hwnd_ != nullptr && IsZoomed(hwnd_) != FALSE; }
    void* native_handle() const override { return hwnd_; }
    void set_caption_blocked(bool blocked) override { caption_blocked_ = blocked; }

    HWND hwnd() const { return hwnd_; }
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    float dpi_scale() const { return static_cast<float>(dpi_) / static_cast<float>(USER_DEFAULT_SCREEN_DPI); }
    bool minimized() const { return minimized_; }
    bool close_requested() const { return close_requested_; }
    bool consume_placement_dirty() {
        const bool dirty = placement_dirty_;
        placement_dirty_ = false;
        return dirty;
    }

    void set_imgui_ready(bool ready) { imgui_ready_ = ready; }
    void set_live_resize(std::function<void()> callback) { live_resize_ = std::move(callback); }

private:
    static LRESULT CALLBACK wnd_proc_thunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT wnd_proc(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT hit_test(LPARAM lp) const;
    void remember_restored_rect();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11RenderTargetView> back_buffer_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT buffer_w_ = 0;
    UINT buffer_h_ = 0;
    UINT pending_w_ = 0;
    UINT pending_h_ = 0;
    RECT restored_{};
    bool have_restored_ = false;
    bool minimized_ = false;
    bool occluded_ = false;
    bool in_size_move_ = false;
    bool in_frame_ = false;
    bool in_live_resize_ = false;
    bool close_requested_ = false;
    bool imgui_ready_ = false;
    bool caption_blocked_ = false;
    bool placement_dirty_ = false;
    std::function<void()> live_resize_;
};

bool Window::create(HINSTANCE instance, const stc::app::AppState& state) {
    instance_ = instance;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::wnd_proc_thunk;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kAppIconResource));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        spdlog::error("RegisterClassExW failed ({})", GetLastError());
        return false;
    }
    // Created hidden: the saved placement is applied before it shows (see show()).
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            kInitialWidth, kInitialHeight, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        spdlog::error("CreateWindowExW failed ({})", GetLastError());
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    (void)state;

    // A one pixel DWM frame extension keeps the system drop shadow, the rounded corners and the
    // snap animations. SWP_FRAMECHANGED runs WM_NCCALCSIZE, which removes the system frame.
    const MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void Window::destroy() {
    release_device();
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(kWindowClass, instance_);
}

bool Window::create_device() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = E_FAIL;
    for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                               device_.ReleaseAndGetAddressOf(), nullptr, context_.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) {
            if (type == D3D_DRIVER_TYPE_WARP) {
                spdlog::warn("No hardware D3D11 device, using WARP");
            }
            break;
        }
    }
    if (FAILED(hr)) {
        spdlog::error("D3D11CreateDevice failed: hr=0x{:08x}", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        spdlog::error("Cannot reach the DXGI factory");
        return false;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    desc.Height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swap_chain_);
    if (FAILED(hr)) {
        spdlog::error("CreateSwapChainForHwnd failed: hr=0x{:08x}", static_cast<unsigned>(hr));
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    buffer_w_ = desc.Width;
    buffer_h_ = desc.Height;
    return create_back_buffer();
}

bool Window::create_back_buffer() {
    ComPtr<ID3D11Texture2D> buffer;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
    if (SUCCEEDED(hr)) {
        hr = device_->CreateRenderTargetView(buffer.Get(), nullptr, back_buffer_.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        spdlog::error("Cannot create the back buffer view: hr=0x{:08x}", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

void Window::release_device() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    back_buffer_.Reset();
    swap_chain_.Reset();
    context_.Reset();
    device_.Reset();
}

bool Window::begin_frame() {
    if (!swap_chain_ || minimized_) {
        return false;
    }
    if (occluded_) {
        if (swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            return false;
        }
        occluded_ = false;
    }
    if (pending_w_ != 0 && pending_h_ != 0 && (pending_w_ != buffer_w_ || pending_h_ != buffer_h_)) {
        back_buffer_.Reset();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        if (SUCCEEDED(swap_chain_->ResizeBuffers(0, pending_w_, pending_h_, DXGI_FORMAT_UNKNOWN, 0))) {
            buffer_w_ = pending_w_;
            buffer_h_ = pending_h_;
        }
        create_back_buffer();
    }
    pending_w_ = pending_h_ = 0;
    if (!back_buffer_) {
        return false;
    }
    in_frame_ = true;
    return true;
}

bool Window::end_frame() {
    const ImVec4& bg = facet::T().colors.bg;
    const float clear[4] = {bg.x, bg.y, bg.z, 1.0F};
    // The flip model unbinds the back buffer on every Present, so bind it every frame.
    ID3D11RenderTargetView* views[] = {back_buffer_.Get()};
    context_->OMSetRenderTargets(1, views, nullptr);
    context_->ClearRenderTargetView(back_buffer_.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = swap_chain_->Present(1, 0);
    in_frame_ = false;
    if (hr == DXGI_STATUS_OCCLUDED) {
        occluded_ = true;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        spdlog::error("Device lost: reason 0x{:08x}", static_cast<unsigned>(device_->GetDeviceRemovedReason()));
        return false;
    }
    return true;
}

void Window::remember_restored_rect() {
    if (hwnd_ == nullptr || IsZoomed(hwnd_) || IsIconic(hwnd_)) {
        return;
    }
    RECT r{};
    if (GetWindowRect(hwnd_, &r)) {
        restored_ = r;
        have_restored_ = true;
    }
}

void Window::show(const stc::app::AppState& state) {
    facet::FramePlacement placement;
    bool use_saved = facet::LoadAppFramePlacement(kPlacementId, &placement) && placement.restoredSize.x > 0.0F &&
                     placement.restoredSize.y > 0.0F;
    HMONITOR monitor = nullptr;
    if (use_saved) {
        const POINT corner{static_cast<LONG>(placement.position.x), static_cast<LONG>(placement.position.y)};
        // The placement counts only when its top-left area is on a monitor that still exists.
        const POINT probe{corner.x + 40, corner.y + 20};
        monitor = MonitorFromPoint(probe, MONITOR_DEFAULTTONULL);
        use_saved = monitor != nullptr;
    }
    if (monitor == nullptr) {
        monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const RECT work = info.rcWork;
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    UINT dpi_x = USER_DEFAULT_SCREEN_DPI;
    UINT dpi_y = USER_DEFAULT_SCREEN_DPI;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
        dpi_x = GetDpiForWindow(hwnd_);
    }
    const float scale = static_cast<float>(dpi_x) / 96.0F * state.ui_scale;

    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    bool maximize = false;
    if (use_saved) {
        width = static_cast<int>(std::lround(placement.restoredSize.x * scale));
        height = static_cast<int>(std::lround(placement.restoredSize.y * scale));
        width = std::clamp(width, std::min(work_w, static_cast<int>(kMinWidth * scale)), work_w);
        height = std::clamp(height, std::min(work_h, static_cast<int>(kMinHeight * scale)), work_h);
        x = std::clamp(static_cast<int>(placement.position.x), static_cast<int>(work.left), static_cast<int>(work.right) - width);
        y = std::clamp(static_cast<int>(placement.position.y), static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
        maximize = placement.maximized;
    } else {
        const int want_w = static_cast<int>(std::lround(kInitialWidth * scale));
        const int want_h = static_cast<int>(std::lround(kInitialHeight * scale));
        width = std::min(want_w, work_w);
        height = std::min(want_h, work_h);
        maximize = width < want_w || height < want_h;
        x = work.left + (work_w - width) / 2;
        y = work.top + (work_h - height) / 2;
    }
    SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    dpi_ = GetDpiForWindow(hwnd_);
    remember_restored_rect();
    ShowWindow(hwnd_, maximize ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
}

void Window::save_placement() {
    remember_restored_rect();
    if (!have_restored_) {
        return;
    }
    const float scale = facet::Scale();
    facet::FramePlacement placement;
    placement.position = ImVec2(static_cast<float>(restored_.left), static_cast<float>(restored_.top));
    placement.restoredSize = ImVec2(static_cast<float>(restored_.right - restored_.left) / scale,
                                    static_cast<float>(restored_.bottom - restored_.top) / scale);
    placement.scale = scale;
    placement.maximized = maximized();
    facet::SaveAppFramePlacement(kPlacementId, placement);
}

LRESULT CALLBACK Window::wnd_proc_thunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        auto* self = static_cast<Window*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    if (self == nullptr || msg == WM_NCDESTROY) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->wnd_proc(msg, wp, lp);
}

LRESULT Window::hit_test(LPARAM lp) const {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd_, &pt);
    if (imgui_ready_) {
        // The app frame answers from its last frame. Only the maximize button goes to Windows
        // (HTMAXBUTTON shows the snap layouts); minimize and close take ordinary clicks.
        switch (facet::AppFrameHitTest(ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y)))) {
            case facet::FrameHit::Left: return HTLEFT;
            case facet::FrameHit::Right: return HTRIGHT;
            case facet::FrameHit::Top: return HTTOP;
            case facet::FrameHit::Bottom: return HTBOTTOM;
            case facet::FrameHit::TopLeft: return HTTOPLEFT;
            case facet::FrameHit::TopRight: return HTTOPRIGHT;
            case facet::FrameHit::BottomLeft: return HTBOTTOMLEFT;
            case facet::FrameHit::BottomRight: return HTBOTTOMRIGHT;
            case facet::FrameHit::Caption: return caption_blocked_ ? HTCLIENT : HTCAPTION;
            case facet::FrameHit::MaximizeButton: return caption_blocked_ ? HTCLIENT : HTMAXBUTTON;
            case facet::FrameHit::MinimizeButton:
            case facet::FrameHit::CloseButton:
            case facet::FrameHit::Client: return HTCLIENT;
        }
        return HTCLIENT;
    }
    // Before the first frame: the system resize band along the edges.
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (maximized()) {
        return HTCLIENT;
    }
    const int border = resize_border(dpi_);
    const bool left = pt.x < client.left + border;
    const bool right = pt.x >= client.right - border;
    const bool top = pt.y < client.top + border;
    const bool bottom = pt.y >= client.bottom - border;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTCLIENT;
}

LRESULT Window::wnd_proc(UINT msg, WPARAM wp, LPARAM lp) {
    if (imgui_ready_ && ImGui_ImplWin32_WndProcHandler(hwnd_, msg, wp, lp)) {
        return 1;
    }
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp == TRUE) {
                // The whole window is client area. A maximized window overhangs the monitor by the
                // frame thickness, so inset it (DPI-aware) to keep the content on screen.
                if (IsZoomed(hwnd_)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                    const UINT dpi = GetDpiForWindow(hwnd_);
                    const int fx = resize_border(dpi);
                    const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    params->rgrc[0].left += fx;
                    params->rgrc[0].right -= fx;
                    params->rgrc[0].top += fy;
                    params->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;

        case WM_NCHITTEST:
            return hit_test(lp);

        // Over HTMAXBUTTON Windows sends non-client button messages. The Dear ImGui backend turns
        // non-client mouse moves into positions but not the buttons: they are passed on here and
        // kept from DefWindowProc, so the drawn maximize button shows hover and press and reports
        // its activation.
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON && imgui_ready_) {
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, msg != WM_NCLBUTTONUP);
                return 0;
            }
            break;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            ImVec2 min_size = imgui_ready_ ? facet::AppFrameMinSize() : ImVec2(0.0F, 0.0F);
            if (min_size.x <= 0.0F || min_size.y <= 0.0F) {
                const float scale = static_cast<float>(hwnd_ != nullptr ? GetDpiForWindow(hwnd_) : USER_DEFAULT_SCREEN_DPI) / 96.0F;
                min_size = ImVec2(kMinWidth * scale, kMinHeight * scale);
            }
            mmi->ptMinTrackSize.x = static_cast<LONG>(std::ceil(min_size.x));
            mmi->ptMinTrackSize.y = static_cast<LONG>(std::ceil(min_size.y));
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                minimized_ = true;
                return 0;
            }
            minimized_ = false;
            pending_w_ = LOWORD(lp);
            pending_h_ = HIWORD(lp);
            if (wp == SIZE_RESTORED) {
                remember_restored_rect();
            }
            placement_dirty_ = true;
            // Inside the modal size loop the main loop does not run; render from here instead.
            if (in_size_move_ && live_resize_ && !in_frame_ && !in_live_resize_) {
                in_live_resize_ = true;
                live_resize_();
                in_live_resize_ = false;
            }
            return 0;

        case WM_ENTERSIZEMOVE:
            in_size_move_ = true;
            return 0;

        case WM_EXITSIZEMOVE:
            in_size_move_ = false;
            remember_restored_rect();
            placement_dirty_ = true;
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_SYSCOMMAND:
            // Alt alone would open the hidden window menu and freeze input until the next key.
            // Alt+Space still opens the system menu, so the window can be moved from the keyboard.
            if ((wp & 0xFFF0) == SC_KEYMENU && lp != VK_SPACE) {
                return 0;
            }
            break;

        case WM_CLOSE:
            // The main loop decides: it waits for a running clean, then tears down and destroys.
            close_requested_ = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// --- The app -----------------------------------------------------------------------------------------

void configure_facet_look(float dpi_scale) {
    facet::SetScaleMode(facet::ScaleMode::FollowDpi);
    facet::SetDpiScale(dpi_scale);
    facet::SyncImGuiStyle(true);
    facet::ApplyImGuiStyle();
}

bool start_facet(Window& window) {
    if (!ImGui_ImplDX11_Init(window.device(), window.context())) {
        spdlog::error("ImGui_ImplDX11_Init failed");
        return false;
    }
    facet::InitDesc init;
    init.device = window.device();
    init.context = window.context();
    if (!facet::Init(init)) {
        spdlog::error("facet::Init failed");
        return false;
    }
    stc::ui::assets::load();
    return true;
}

int run_window(HINSTANCE instance) {
    // Native dialogs (IFileDialog) need COM on this thread.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Settings and directories come first: the ini path and the appearance depend on them.
    stc::app::AppState state;
    try {
        state.initialize();
    } catch (const std::exception& e) {
        spdlog::error("AppState init failed: {}", e.what());
    }

    Window window;
    if (!window.create(instance, state) || !window.create_device()) {
        window.destroy();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Dear ImGui keeps the pointer: the string lives until after DestroyContext.
    const std::string ini_path = stc::ui::path_to_utf8(state.config_dir / "ui.ini");
    io.IniFilename = ini_path.c_str();
    io.LogFilename = nullptr;

    configure_facet_look(window.dpi_scale());
    stc::ui::sync_appearance(state, false);
    ImGui_ImplWin32_Init(window.hwnd());
    if (!start_facet(window)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        window.destroy();
        return 1;
    }
    window.set_imgui_ready(true);

    // The placement is read before the first frame so the window shows at its saved place.
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    window.show(state);

    bool device_ok = true;
    auto frame = [&]() -> bool {
        if (!window.begin_frame()) {
            return false;
        }
        if (facet::DpiScale() != window.dpi_scale()) {
            facet::SetDpiScale(window.dpi_scale());
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        facet::NewFrame();
        stc::ui::draw_app(state, window);
        facet::EndFrame();
        ImGui::Render();
        device_ok = window.end_frame();
        return true;
    };
    window.set_live_resize([&] { frame(); });

    for (;;) {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (window.close_requested() || quit) {
            // A clean or restore is never cut off: keep drawing until it is done.
            if (!state.tasks.must_finish_running()) {
                break;
            }
            state.close_pending = true;
        }
        if (window.consume_placement_dirty()) {
            window.save_placement();
        }
        if (!frame()) {
            // Minimized or occluded: wait for a message (or poll the window again shortly, so a
            // pending close still happens when the job ends).
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        if (!device_ok) {
            // A new device: Facet and the backend stop, the device is created again, both start
            // again and the images load again (handles do not survive facet::Shutdown).
            spdlog::warn("Recreating the D3D11 device");
            stc::ui::assets::release();
            facet::Shutdown();
            ImGui_ImplDX11_Shutdown();
            window.release_device();
            if (!window.create_device() || !start_facet(window)) {
                spdlog::error("Device recreation failed, exiting");
                break;
            }
            device_ok = true;
        }
    }

    window.set_live_resize(nullptr);
    window.save_placement();
    state.tasks.shutdown();
    window.set_imgui_ready(false);
    stc::ui::assets::release();
    facet::Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();  // writes ui.ini, with Facet's sections and the placement
    window.destroy();
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    return 0;
}

int run_headless_clean() {
    stc::app::AppState state;
    try {
        state.initialize();
    } catch (const std::exception& e) {
        spdlog::error("AppState init failed: {}", e.what());
        return 1;
    }
    if (!state.install) {
        spdlog::error("Steam install not found, aborting scheduled clean");
        return 1;
    }

    auto profiles = stc::core::built_in_profiles();
    if (profiles.empty()) {
        return 1;
    }
    const auto& profile = profiles.front();  // Quick Clean

    stc::core::ResolveContext ctx{*state.install, state.accounts, state.libraries};
    stc::core::PlanOptions opts;
    opts.ignore = &state.ignore_list;

    auto plan = stc::core::build_plan_by_ids(profile.target_ids, ctx, opts);

    // The same rule as a cleanup started from the window: without the backup nothing is removed.
    auto session = stc::core::backup::Session::create(state.backups_dir);
    if (!session) {
        spdlog::error("The backup folder could not be created, aborting scheduled clean");
        return 1;
    }
    stc::core::CleanOptions cleaner_opts;
    cleaner_opts.backup = &*session;
    auto result = stc::core::execute(plan, cleaner_opts);
    spdlog::info("Headless clean finished: {} ok, {} failed, {} bytes freed", result.succeeded, result.failed,
                 result.bytes_freed);
    // Each sign-in adds a backup; keep to the "Backups to keep" setting like the window does.
    if (state.backup_keep_count > 0) {
        const auto pruned = stc::core::backup::prune_backups(
            state.backups_dir, static_cast<std::size_t>(state.backup_keep_count));
        spdlog::info("Removed {} old backups", pruned);
    }
    return result.failed > 0 ? 2 : 0;
}

bool has_flag(const wchar_t* cmdline, const wchar_t* flag) { return cmdline && wcsstr(cmdline, flag) != nullptr; }

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    // The dev modes are read-only: they set up their own logging and never touch the data dir.
    int dev_exit = 0;
    if (stc::app::run_dev_mode(cmdline, &dev_exit)) {
        return dev_exit;
    }

    try {
        // The same directory AppState uses for its data, so the log is in AppState::log_dir
        // (beside the exe in portable mode, where it used to go to %LOCALAPPDATA% regardless).
        stc::core::log::init(stc::app::default_data_dir());
    } catch (...) {
        // Logging is best-effort. If the data directory is unavailable for some reason, fall back
        // to the executable directory.
        try {
            stc::core::log::init(stc::platform::exe_directory());
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }
    spdlog::info("Starting steam-tracer-cleaner {}", stc::core::kAppVersion);

    if (has_flag(cmdline, L"--scheduled")) {
        spdlog::info("Headless mode (--scheduled)");
        int rc = run_headless_clean();
        stc::core::log::flush_all();
        return rc;
    }

    const int rc = run_window(inst);
    spdlog::info("Shutdown clean");
    stc::core::log::flush_all();
    return rc;
}
