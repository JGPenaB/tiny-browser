#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace tb {

// Direct3D 11 device + flip-model swap chain with a Direct2D 1.1 device context on top.
// All page and UI drawing goes through the GPU; WARP is used only if no hardware device exists.
class Gpu {
public:
    bool init(HWND h);
    void resize();
    ID2D1DeviceContext* begin();
    void end();
    ID2D1Factory1* factory() { return f_.Get(); }
    uint64_t gen = 0;
    bool warp = false;
    float dpi = 96;
    std::wstring adapter;

private:
    bool createDevice();
    void createTarget();
    void releaseDevice();
    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory1> f_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> sc_;
    Microsoft::WRL::ComPtr<ID2D1Device> dev_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
};

}  // namespace tb
