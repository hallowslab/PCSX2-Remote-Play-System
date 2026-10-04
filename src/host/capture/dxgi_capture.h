#pragma once

#include "capture.h"

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace rps {

class DXGICapture : public ICapture {
public:
    DXGICapture();
    ~DXGICapture() override;

    bool init(const CaptureConfig& config) override;
    bool start() override;
    void stop() override;
    bool acquireFrame(CapturedFrame& frame, int timeout_ms = 100) override;
    void releaseFrame() override;
    void getDimensions(int& width, int& height) const override;
    bool isCapturing() const override;
    CaptureError getLastError() const override;

private:
    bool initDXGI();
    bool createStagingTexture();
    bool reinitDXGI();
    bool reinitAll();
    bool queryOutput0(int &w, int &h, int &left, int &top);
    bool initGdi();
    void releaseGdi();
    bool gdiAcquire(CapturedFrame& frame);

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Texture2D> m_stagingTexture;
    
    CaptureConfig m_config;
    int m_width = 0;
    int m_height = 0;
    bool m_capturing = false;
    bool m_frameMapped = false;
    CaptureError m_lastError = CaptureError::None;
    D3D11_MAPPED_SUBRESOURCE m_mappedResource = {};
    ULONGLONG m_lastReinitTick = 0;
    // GDI fallback state (Desktop Duplication is unstable on some Win11/NVIDIA
    // setups and can loop on DXGI_ERROR_ACCESS_LOST forever).
    bool m_gdiMode = false;
    ULONGLONG m_lastGoodTick = 0;
    ULONGLONG m_failStartTick = 0;
    HDC m_gdiScreen = nullptr;
    HDC m_gdiMem = nullptr;
    HBITMAP m_gdiBmp = nullptr;
    HGDIOBJ m_gdiOldBitmap = nullptr;
    uint8_t *m_gdiBits = nullptr;
    int m_gdiWidth = 0;
    int m_gdiHeight = 0;
    int m_outputLeft = 0;
    int m_outputTop = 0;
    bool m_gdiFirstFrameLogged = false;
};

} // namespace rps

#endif // _WIN32
