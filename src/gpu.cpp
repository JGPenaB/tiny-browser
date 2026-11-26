#include "gpu.h"

#include <string>

using Microsoft::WRL::ComPtr;

namespace tb {

static uint64_t g_gen = 1;

bool Gpu::init(HWND h) {
    hwnd_ = h;
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                 reinterpret_cast<void**>(f_.GetAddressOf()))))
        return false;
    return createDevice();
}

bool Gpu::createDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, ARRAYSIZE(fls),
                                   D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    warp = false;
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
                               &d3d_, nullptr, nullptr);
        warp = true;
    }
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice1> dxgi;
    if (FAILED(d3d_.As(&dxgi))) return false;
    dxgi->SetMaximumFrameLatency(1);
    if (FAILED(f_->CreateDevice(dxgi.Get(), &dev_))) return false;
    if (FAILED(dev_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    ComPtr<IDXGIAdapter> ad;
    dxgi->GetAdapter(&ad);
    DXGI_ADAPTER_DESC desc{};
    if (ad && SUCCEEDED(ad->GetDesc(&desc))) adapter = desc.Description;
    ComPtr<IDXGIFactory2> xf;
    if (!ad || FAILED(ad->GetParent(IID_PPV_ARGS(&xf)))) return false;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Scaling = DXGI_SCALING_NONE;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(xf->CreateSwapChainForHwnd(d3d_.Get(), hwnd_, &d, nullptr, nullptr, &sc_))) return false;
    xf->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    gen = ++g_gen;
    createTarget();
    return true;
}

void Gpu::createTarget() {
    if (!sc_ || !dc_) return;
    dpi = (float)GetDpiForWindow(hwnd_);
    if (dpi <= 0) dpi = 96;
    ComPtr<IDXGISurface> surf;
    if (FAILED(sc_->GetBuffer(0, IID_PPV_ARGS(&surf)))) return;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target_))) return;
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi, dpi);
}

void Gpu::releaseDevice() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    dc_.Reset();
    dev_.Reset();
    sc_.Reset();
    d3d_.Reset();
}

void Gpu::resize() {
    if (!sc_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) return;
    dc_->SetTarget(nullptr);
    target_.Reset();
    if (FAILED(sc_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0))) {
        releaseDevice();
        return;
    }
    createTarget();
}

ID2D1DeviceContext* Gpu::begin() {
    if (!dc_ && !createDevice()) return nullptr;
    if (!target_) createTarget();
    if (!target_) return nullptr;
    dc_->BeginDraw();
    return dc_.Get();
}

void Gpu::end() {
    HRESULT hr = dc_->EndDraw();
    HRESULT pr = sc_->Present(1, 0);
    if (hr == D2DERR_RECREATE_TARGET || pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) {
        releaseDevice();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

}  // namespace tb
