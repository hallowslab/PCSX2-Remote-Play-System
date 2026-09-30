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
};

} // namespace rps

#endif // _WIN32
