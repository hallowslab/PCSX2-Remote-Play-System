#include "dxgi_capture.h"
#include "../../shared/protocol/protocol.h"


#ifdef _WIN32

#include <iostream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace rps {

DXGICapture::DXGICapture() = default;

DXGICapture::~DXGICapture() { stop(); }

bool DXGICapture::init(const CaptureConfig &config) {
  m_config = config;

  if (!initDXGI()) {
    m_lastError = CaptureError::InitFailed;
    return false;
  }

  if (!createStagingTexture()) {
    m_lastError = CaptureError::InitFailed;
    return false;
  }

  std::cout << "[DXGICapture] Initialized: " << m_width << "x" << m_height
            << std::endl;
  return true;
}

bool DXGICapture::initDXGI() {
  HRESULT hr;

  // Create DXGI Factory
  ComPtr<IDXGIFactory1> dxgiFactory;
  hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&dxgiFactory);
  if (FAILED(hr)) {
    std::cerr << "[DXGICapture] Failed to create DXGI factory" << std::endl;
    return false;
  }

  // Get primary adapter
  ComPtr<IDXGIAdapter1> adapter;
  hr = dxgiFactory->EnumAdapters1(0, &adapter);
  if (FAILED(hr)) {
    std::cerr << "[DXGICapture] Failed to enumerate adapters" << std::endl;
    return false;
  }

  // Get primary output
  ComPtr<IDXGIOutput> output;
  hr = adapter->EnumOutputs(0, &output);
  if (FAILED(hr)) {
    std::cerr << "[DXGICapture] Failed to enumerate outputs" << std::endl;
    return false;
  }

  // Get output description for dimensions
  DXGI_OUTPUT_DESC outputDesc;
  output->GetDesc(&outputDesc);
  m_width =
      outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
  m_height =
      outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

  // QueryInterface for IDXGIOutput1
  ComPtr<IDXGIOutput1> output1;
  hr = output.As(&output1);
  if (FAILED(hr)) {
    std::cerr << "[DXGICapture] Failed to get IDXGIOutput1" << std::endl;
    return false;
  }

  // Create D3D11 device
  D3D_FEATURE_LEVEL featureLevel;
  hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr,
                         0, D3D11_SDK_VERSION, &m_device, &featureLevel,
                         &m_context);
  if (FAILED(hr)) {
    std::cerr << "[DXGICapture] Failed to create D3D11 device" << std::endl;
    return false;
  }

  // Duplicate output
  hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
  if (FAILED(hr)) {
    std::cerr << "[DXGICapture] Failed to duplicate output (hr=0x" << std::hex
              << hr << std::dec << ")" << std::endl;
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
      std::cerr
          << "[DXGICapture] Too many applications using desktop duplication"
          << std::endl;
    } else if (hr == E_ACCESSDENIED) {
      m_lastError = CaptureError::AccessDenied;
    }
    return false;
  }

  return true;
}

bool DXGICapture::createStagingTexture() {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = m_width;
  desc.Height = m_height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;

  HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
  if (FAILED(hr)) {
    std::cerr << "[DXGICapture] Failed to create staging texture" << std::endl;
    return false;
  }

  return true;
}

bool DXGICapture::start() {
  if (!m_duplication) {
    m_lastError = CaptureError::InitFailed;
    return false;
  }
  m_capturing = true;
  return true;
}

void DXGICapture::stop() {
  if (m_frameMapped) {
    releaseFrame();
  }
  m_capturing = false;
}

bool DXGICapture::acquireFrame(CapturedFrame &frame, int timeout_ms) {
  if (!m_capturing || !m_duplication) {
    m_lastError = CaptureError::InitFailed;
    return false;
  }

  // Release any previously mapped frame
  if (m_frameMapped) {
    releaseFrame();
  }

  DXGI_OUTDUPL_FRAME_INFO frameInfo;
  ComPtr<IDXGIResource> desktopResource;

  HRESULT hr =
      m_duplication->AcquireNextFrame(timeout_ms, &frameInfo, &desktopResource);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
    m_lastError = CaptureError::FrameTimeout;
    return false;
  }
  if (FAILED(hr)) {
    if (hr == DXGI_ERROR_ACCESS_LOST) {
      m_lastError = CaptureError::DeviceLost;
      std::cerr << "[DXGICapture] Device lost, need to reinitialize"
                << std::endl;
    } else {
      m_lastError = CaptureError::Unknown;
    }
    return false;
  }

  // Get the desktop texture
  ComPtr<ID3D11Texture2D> desktopTexture;
  hr = desktopResource.As(&desktopTexture);
  if (FAILED(hr)) {
    m_duplication->ReleaseFrame();
    m_lastError = CaptureError::Unknown;
    return false;
  }

  // Check if dimensions changed
  D3D11_TEXTURE2D_DESC texDesc;
  desktopTexture->GetDesc(&texDesc);
  if (texDesc.Width != static_cast<UINT>(m_width) ||
      texDesc.Height != static_cast<UINT>(m_height)) {
    // Resolution changed, need to recreate staging texture
    m_width = texDesc.Width;
    m_height = texDesc.Height;
    m_duplication->ReleaseFrame();

    if (!createStagingTexture()) {
      m_lastError = CaptureError::InitFailed;
      return false;
    }

    // Re-acquire the frame
    return acquireFrame(frame, timeout_ms);
  }

  // Copy to staging texture
  m_context->CopyResource(m_stagingTexture.Get(), desktopTexture.Get());

  // Map the staging texture
  hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0,
                      &m_mappedResource);
  if (FAILED(hr)) {
    m_duplication->ReleaseFrame();
    m_lastError = CaptureError::Unknown;
    return false;
  }

  m_frameMapped = true;

  // Fill in the frame data
  frame.data = static_cast<uint8_t *>(m_mappedResource.pData);
  frame.width = m_width;
  frame.height = m_height;
  frame.pitch = m_mappedResource.RowPitch;
  frame.timestamp_us = get_timestamp_us();
  frame.is_new_frame = (frameInfo.LastPresentTime.QuadPart != 0);

  // Release the DXGI frame (we've copied to staging)
  m_duplication->ReleaseFrame();

  m_lastError = CaptureError::None;
  return true;
}

void DXGICapture::releaseFrame() {
  if (m_frameMapped && m_context && m_stagingTexture) {
    m_context->Unmap(m_stagingTexture.Get(), 0);
    m_frameMapped = false;
  }
}

void DXGICapture::getDimensions(int &width, int &height) const {
  width = m_width;
  height = m_height;
}

bool DXGICapture::isCapturing() const { return m_capturing; }

CaptureError DXGICapture::getLastError() const { return m_lastError; }

// Factory implementation for Windows
std::unique_ptr<ICapture> createCapture() {
  return std::make_unique<DXGICapture>();
}

} // namespace rps

#endif // _WIN32
