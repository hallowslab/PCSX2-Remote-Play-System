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
  m_outputLeft = outputDesc.DesktopCoordinates.left;
  m_outputTop = outputDesc.DesktopCoordinates.top;

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

bool DXGICapture::reinitDXGI() {
  // Recreate only the duplication + staging texture on the SAME D3D11 device.
  // Recreating the device itself can invalidate the freshly created
  // duplication on some drivers, producing a perpetual ACCESS_LOST loop.
  m_duplication.Reset();
  m_stagingTexture.Reset();

  ComPtr<IDXGIFactory1> dxgiFactory;
  if (FAILED(
          CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&dxgiFactory)))
    return false;
  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(dxgiFactory->EnumAdapters1(0, &adapter)))
    return false;
  ComPtr<IDXGIOutput> output;
  if (FAILED(adapter->EnumOutputs(0, &output)))
    return false;
  DXGI_OUTPUT_DESC outputDesc;
  output->GetDesc(&outputDesc);
  m_width =
      outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
  m_height =
      outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
  m_outputLeft = outputDesc.DesktopCoordinates.left;
  m_outputTop = outputDesc.DesktopCoordinates.top;
  ComPtr<IDXGIOutput1> output1;
  if (FAILED(output.As(&output1)))
    return false;
  if (FAILED(output1->DuplicateOutput(m_device.Get(), &m_duplication)))
    return false;
  if (!createStagingTexture())
    return false;
  std::cout << "[DXGICapture] Reinitialized: " << m_width << "x" << m_height
            << "\n";
  return true;
}

bool DXGICapture::reinitAll() {
  m_duplication.Reset();
  m_stagingTexture.Reset();
  m_context.Reset();
  m_device.Reset();
  if (!initDXGI())
    return false;
  if (!createStagingTexture())
    return false;
  std::cout << "[DXGICapture] Full reinit: " << m_width << "x" << m_height
            << "\n";
  return true;
}

// Query adapter 0 / output 0 desktop rectangle without a D3D device, so the
// GDI fallback can adapt to resolution / mode changes after a fullscreen
// toggle.
bool DXGICapture::queryOutput0(int &w, int &h, int &left, int &top) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(
          CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)))
    return false;
  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(factory->EnumAdapters1(0, &adapter)))
    return false;
  ComPtr<IDXGIOutput> output;
  if (FAILED(adapter->EnumOutputs(0, &output)))
    return false;
  DXGI_OUTPUT_DESC desc;
  output->GetDesc(&desc);
  w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
  h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
  left = desc.DesktopCoordinates.left;
  top = desc.DesktopCoordinates.top;
  return true;
}

// GDI fallback: BitBlt the screen DC. Captures whatever DWM composites, which
// works even when Desktop Duplication is stuck in an ACCESS_LOST loop.
bool DXGICapture::initGdi() {
  m_gdiScreen = GetDC(nullptr);
  if (!m_gdiScreen)
    return false;
  m_gdiMem = CreateCompatibleDC(m_gdiScreen);
  if (!m_gdiMem) {
    releaseGdi();
    return false;
  }

  // DIB section gives us directly writable/readable BGRA pixels. Do not use
  // GetDIBits here: Win32 requires its bitmap to be deselected from every DC.
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = m_width;
  bi.bmiHeader.biHeight = -m_height; // top-down rows
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  m_gdiBmp = CreateDIBSection(m_gdiScreen, &bi, DIB_RGB_COLORS, &bits, nullptr,
                              0);
  if (!m_gdiBmp || !bits) {
    releaseGdi();
    return false;
  }
  m_gdiBits = static_cast<uint8_t *>(bits);
  m_gdiOldBitmap = SelectObject(m_gdiMem, m_gdiBmp);
  if (!m_gdiOldBitmap || m_gdiOldBitmap == HGDI_ERROR) {
    m_gdiOldBitmap = nullptr;
    releaseGdi();
    return false;
  }
  m_gdiWidth = m_width;
  m_gdiHeight = m_height;
  std::cout << "[DXGICapture] GDI fallback ready: " << m_width << "x"
            << m_height << " at " << m_outputLeft << "," << m_outputTop
            << "\n";
  return true;
}

void DXGICapture::releaseGdi() {
  if (m_gdiMem && m_gdiOldBitmap)
    SelectObject(m_gdiMem, m_gdiOldBitmap);
  if (m_gdiBmp)
    DeleteObject(m_gdiBmp);
  if (m_gdiMem)
    DeleteDC(m_gdiMem);
  if (m_gdiScreen)
    ReleaseDC(nullptr, m_gdiScreen);
  m_gdiScreen = nullptr;
  m_gdiMem = nullptr;
  m_gdiBmp = nullptr;
  m_gdiOldBitmap = nullptr;
  m_gdiBits = nullptr;
  m_gdiWidth = 0;
  m_gdiHeight = 0;
}

bool DXGICapture::gdiAcquire(CapturedFrame &frame) {
  // Re-query the desktop rect every frame so the fallback recovers from a
  // fullscreen<->windowed toggle that changed the display mode or resolution.
  int w = 0, h = 0, left = 0, top = 0;
  if (!queryOutput0(w, h, left, top))
    return false;
  if (!m_gdiMem || m_gdiWidth != w || m_gdiHeight != h || m_outputLeft != left ||
      m_outputTop != top) {
    m_width = w;
    m_height = h;
    m_outputLeft = left;
    m_outputTop = top;
    releaseGdi();
    if (!initGdi())
      return false;
  }

  if (!BitBlt(m_gdiMem, 0, 0, w, h, m_gdiScreen, left, top,
              SRCCOPY | CAPTUREBLT))
    return false;

  frame.data = m_gdiBits;
  frame.width = w;
  frame.height = h;
  frame.pitch = w * 4;
  frame.timestamp_us = get_timestamp_us();
  frame.is_new_frame = true;
  m_lastError = CaptureError::None;
  m_lastGoodTick = GetTickCount64();
  if (!m_gdiFirstFrameLogged) {
    std::cout << "[DXGICapture] First GDI fallback frame captured " << w << "x"
              << h << "\n";
    m_gdiFirstFrameLogged = true;
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
  releaseGdi();
}

bool DXGICapture::acquireFrame(CapturedFrame &frame, int timeout_ms) {
  if (!m_capturing)
    return false;
  if (m_gdiMode)
    return gdiAcquire(frame);
  if (!m_duplication) {
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
    // A static desktop yields WAIT_TIMEOUT normally. But if we previously
    // captured frames and now see a long timeout streak, DWM likely stopped
    // presenting (RPCS3 exclusive fullscreen). Desktop Duplication can sit in
    // this state forever without ACCESS_LOST, so force a fallback to GDI.
    ULONGLONG now = GetTickCount64();
    if (m_lastGoodTick != 0 && now - m_lastGoodTick > 2000) {
      std::cerr << "[DXGICapture] no frame for 2s, falling back to GDI\n";
      m_gdiMode = true;
      m_duplication.Reset();
      m_stagingTexture.Reset();
      m_context.Reset();
      m_device.Reset();
      return gdiAcquire(frame);
    }
    m_lastError = CaptureError::FrameTimeout;
    return false;
  }
  if (FAILED(hr)) {
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED) {
      // Recreate the duplication (or the whole pipeline for a removed
      // device), then retry this frame once. If the desktop has been stuck
      // failing for a while, Desktop Duplication may never recover (known
      // Win11/NVIDIA issue), so fall back to GDI capture.
      ULONGLONG now = GetTickCount64();
      if (m_failStartTick == 0)
        m_failStartTick = now;
      if (now - m_failStartTick > 2000) {
        std::cerr << "[DXGICapture] Desktop Duplication stuck (hr=0x"
                  << std::hex << (unsigned)hr << std::dec
                  << "), falling back to GDI capture\n";
        m_gdiMode = true;
        m_duplication.Reset();
        m_stagingTexture.Reset();
        m_context.Reset();
        m_device.Reset();
        return gdiAcquire(frame);
      }
      if (now - m_lastReinitTick >= 500) {
        m_lastReinitTick = now;
        std::cerr << "[DXGICapture] Device lost (hr=0x" << std::hex
                  << (unsigned)hr << std::dec << "), reinitializing\n";
        bool ok = (hr == DXGI_ERROR_DEVICE_REMOVED) ? reinitAll()
                                                    : reinitDXGI();
        if (ok)
          return acquireFrame(frame, timeout_ms);
      }
      m_lastError = CaptureError::DeviceLost;
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
    std::cout << "[DXGICapture] Resolution changed: " << m_width << "x"
              << m_height << " -> " << texDesc.Width << "x" << texDesc.Height
              << "\n";
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

  // Diagnostic: every ~120 frames, log average luminance so we can tell a
  // black capture (avg_lum ~ 0) from a content-carrying capture.
  static uint32_t diag_count = 0;
  if (++diag_count % 120 == 0) {
    double sum = 0;
    size_t n = 0;
    const uint8_t *p = frame.data;
    const uint8_t *row_end = p + (size_t)m_height * frame.pitch;
    for (const uint8_t *row = p; row < row_end; row += frame.pitch) {
      for (int x = 0; x < m_width; x += 32) {
        const uint8_t *px = row + (size_t)x * 4;
        if (px + 4 <= row + frame.pitch) {
          sum += 0.114 * px[0] + 0.587 * px[1] + 0.299 * px[2];
          n++;
        }
      }
    }
    double avg = n ? sum / n : 0;
    std::cout << "[DXGICapture] frame " << m_width << "x" << m_height
              << " avg_lum " << (int)avg << " (last_error "
              << (int)m_lastError << ")\n";
  }

  // Release the DXGI frame (we've copied to staging)
  m_duplication->ReleaseFrame();

  m_lastError = CaptureError::None;
  m_lastGoodTick = GetTickCount64();
  m_failStartTick = 0;
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
