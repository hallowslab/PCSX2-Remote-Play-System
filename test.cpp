bool init_dxgi_capture(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context,
    ComPtr<IDXGIOutputDuplication>& duplication, int& width, int& height) {
    HRESULT hr;
    ComPtr<IDXGIFactory1> dxgiFactory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter1> adapter;
    hr = dxgiFactory->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, &output);
    if (FAILED(hr)) return false;

    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) return false;

    D3D_FEATURE_LEVEL level;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    device.As(&dxgiDevice);

    ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(&dxgiAdapter);

    hr = output1->DuplicateOutput(device.Get(), &duplication);
    if (FAILED(hr)) return false;

    width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    return true;
}