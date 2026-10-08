#include "overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <atomic>
#include <mutex>

#include "util.h"

namespace kcp {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn o_present = nullptr;
ResizeFn o_resize = nullptr;
void* g_presentTarget = nullptr;
void* g_resizeTarget = nullptr;

std::mutex g_modelMutex;
OverlayModel g_model;

IDXGISwapChain* g_swap = nullptr;   // the swap chain we initialised on (Kenshi's)
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_ready = false;
bool g_failed = false;
double g_lastFrame = 0;

void ReleaseRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool EnsureInit(IDXGISwapChain* swap) {
    if (g_ready) return swap == g_swap;
    if (g_failed) return false;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) { g_failed = true; return false; }
    g_device->GetImmediateContext(&g_ctx);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui::GetStyle().Alpha = 0.92f;
    if (!ImGui_ImplDX11_Init(g_device, g_ctx)) { g_failed = true; return false; }
    g_swap = swap;
    g_ready = true;
    Log("overlay initialised");
    return true;
}

void Draw(const OverlayModel& m, float w, float h) {
    if (m.visible) {
        ImGui::SetNextWindowPos(ImVec2(w - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGui::Begin("KenshiCoop", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", m.title.c_str());
        ImGui::Separator();
        for (const auto& l : m.lines) ImGui::TextUnformatted(l.c_str());
        if (!m.chat.empty()) {
            ImGui::Separator();
            for (const auto& l : m.chat) ImGui::TextUnformatted(l.c_str());
        }
        ImGui::End();
    }
    if (!m.toasts.empty()) {
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.12f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("KenshiCoopToast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
        for (const auto& t : m.toasts) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", t.c_str());
        ImGui::End();
    }
}

void Render(IDXGISwapChain* swap) {
    if (!EnsureInit(swap)) return;
    OverlayModel m;
    {
        std::lock_guard<std::mutex> lk(g_modelMutex);
        m = g_model;
    }
    if (!m.visible && m.toasts.empty()) return;

    if (!g_rtv) {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return;
        const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
        if (FAILED(hr)) return;
    }
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swap->GetDesc(&desc)) || desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(desc.BufferDesc.Width), float(desc.BufferDesc.Height));
    const double now = NowSeconds();
    io.DeltaTime = g_lastFrame > 0 ? float(std::max(1e-4, now - g_lastFrame)) : 1.0f / 60.0f;
    g_lastFrame = now;

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    Draw(m, io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();

    // The DX11 backend restores pipeline state but not render targets: do that ourselves.
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
}

void RenderSEH(IDXGISwapChain* swap) {
    __try {
        Render(swap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;   // never risk the game's frame twice
        Log("overlay: exception during rendering, overlay disabled");
    }
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) RenderSEH(swap);
    return o_present(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* swap, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (swap == g_swap) ReleaseRTV();   // the back buffer is about to be recreated
    return o_resize(swap, count, w, h, fmt, flags);
}

// Find IDXGISwapChain::Present/ResizeBuffers through a throwaway device on a hidden window.
bool FindSwapChainFunctions(void** present, void** resize, std::string* err) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KenshiCoopDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { if (err) *err = "cannot create dummy window"; return false; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, &fl, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, &fl, &ctx);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        void** vt = *reinterpret_cast<void***>(swap);
        *present = vt[8];
        *resize = vt[13];
    } else if (err) {
        *err = "cannot create a D3D11 device for the overlay";
    }
    if (swap) swap->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

bool OverlayInstall(std::string* err) {
    if (!FindSwapChainFunctions(&g_presentTarget, &g_resizeTarget, err)) return false;
    if (MH_CreateHook(g_presentTarget, reinterpret_cast<void*>(&hk_present), reinterpret_cast<void**>(&o_present)) != MH_OK ||
        MH_CreateHook(g_resizeTarget, reinterpret_cast<void*>(&hk_resize), reinterpret_cast<void**>(&o_resize)) != MH_OK ||
        MH_EnableHook(g_presentTarget) != MH_OK || MH_EnableHook(g_resizeTarget) != MH_OK) {
        if (err) *err = "cannot hook the swap chain";
        return false;
    }
    return true;
}

void OverlayPublish(OverlayModel model) {
    std::lock_guard<std::mutex> lk(g_modelMutex);
    g_model = std::move(model);
}

void OverlayShutdown() {
    if (g_presentTarget) MH_DisableHook(g_presentTarget);
    if (g_resizeTarget) MH_DisableHook(g_resizeTarget);
}

} // namespace kcp
