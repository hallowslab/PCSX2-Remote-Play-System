#include "host.h"
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include "encoder/encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

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

int send_all(int sock, const char* data, int len) {
    int total_sent = 0;
    while (total_sent < len) {
        int sent = send(sock, data + total_sent, len - total_sent, 0);
        if (sent <= 0) return sent;
        total_sent += sent;
    }
    return total_sent;
}

void start_host_server(int port, bool& running) {
    #ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    #endif


    #ifdef _WIN32
        SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    #else
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    #endif
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, 1);
    std::cout << "[Host] Waiting for client...\n";

    #ifdef _WIN32
        SOCKET client_fd = accept(server_fd, nullptr, nullptr);
    #else
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            std::cerr << "Failed to accept client\n";
            return;
        }
    #endif
    std::cout << "[Host] Client connected!\n";

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    int width = 0, height = 0;

    if (!init_dxgi_capture(device, context, duplication, width, height)) {
        std::cerr << "[Host] DXGI initialization failed\n";
        return;
    }

    ComPtr<ID3D11Texture2D> stagingTex;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    device->CreateTexture2D(&desc, nullptr, &stagingTex);

    EncoderSettings settings = {
        width,              // int
        height,             // int
        30,                 // fps
        5'000'000,          // bitrate
        EncoderType::NVENC, // preferred encoder
        AV_PIX_FMT_BGRA     // input pixel format
    };

    EncoderContext enc;
    if (!init_encoder(settings, enc)) {
        std::cerr << "Failed to initialize encoder\n";
        return;
    }

    SwsContext* sws_ctx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
        width, height, AV_PIX_FMT_YUV420P, 0, 0, 0, 0);

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 32);

    AVPacket* pkt = av_packet_alloc();
    
    int64_t frame_index = 0;

    while (running) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> desktopResource;
        HRESULT hr = duplication->AcquireNextFrame(100, &frameInfo, &desktopResource);
        if (FAILED(hr)) continue;

        ComPtr<ID3D11Texture2D> tex;
        desktopResource.As(&tex);

        context->CopyResource(stagingTex.Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped;
        context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);

        uint8_t* inData[1] = { (uint8_t*)mapped.pData };
        int inLinesize[1] = { (int)mapped.RowPitch };

        sws_scale(enc.sws_ctx, inData, inLinesize, 0, enc.codec_ctx->height, enc.frame->data, enc.frame->linesize);
        enc.frame->pts = frame_index++;
        avcodec_send_frame(enc.codec_ctx, enc.frame);
        
        while (avcodec_receive_packet(enc.codec_ctx, enc.pkt) == 0) {
            int net_size = enc.pkt->size;
            int net_size_be = htonl(net_size);
            if (send_all(client_fd, (char*)&net_size_be, sizeof(net_size_be)) != sizeof(net_size_be)) {
                std::cerr << "[Host] Failed to send packet size\n";
                break;
            }
            if (send_all(client_fd, (char*)enc.pkt->data, net_size) != net_size) {
                std::cerr << "[Host] Failed to send packet data\n";
                break;
            }
            av_packet_unref(enc.pkt);
        }

        context->Unmap(stagingTex.Get(), 0);
        duplication->ReleaseFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    destroy_encoder(enc);

#ifdef _WIN32
    closesocket(client_fd);
    closesocket(server_fd);
    WSACleanup();
#else
    close(client_fd);
    close(server_fd);
#endif
}
