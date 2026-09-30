#pragma once

#include <cstdint>
#include <memory>
#include <functional>
#include <string>

namespace rps {

// Captured frame data
struct CapturedFrame {
    uint8_t* data;          // Pointer to pixel data (BGRA format)
    int width;
    int height;
    int pitch;              // Row stride in bytes
    uint64_t timestamp_us;
    bool is_new_frame;      // Whether this is a new frame vs duplicate
};

// Capture configuration
struct CaptureConfig {
    int target_fps = 60;
    bool capture_cursor = false;
    std::string target_window;  // Empty = primary display
};

// Capture error types
enum class CaptureError {
    None,
    InitFailed,
    DeviceLost,
    FrameTimeout,
    WindowClosed,
    AccessDenied,
    Unknown
};

// Callback for frame events
using FrameCallback = std::function<void(const CapturedFrame& frame)>;
using ErrorCallback = std::function<void(CaptureError error)>;

// Abstract capture interface
class ICapture {
public:
    virtual ~ICapture() = default;
    
    // Initialize capture with config
    virtual bool init(const CaptureConfig& config) = 0;
    
    // Start capturing
    virtual bool start() = 0;
    
    // Stop capturing
    virtual void stop() = 0;
    
    // Acquire next frame (blocking with timeout)
    // Returns true if frame was acquired
    virtual bool acquireFrame(CapturedFrame& frame, int timeout_ms = 100) = 0;
    
    // Release the previously acquired frame
    virtual void releaseFrame() = 0;
    
    // Get current capture dimensions
    virtual void getDimensions(int& width, int& height) const = 0;
    
    // Check if capture is active
    virtual bool isCapturing() const = 0;
    
    // Get last error
    virtual CaptureError getLastError() const = 0;
};

// Factory to create platform-specific capture
std::unique_ptr<ICapture> createCapture();

} // namespace rps
