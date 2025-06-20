// Project: PCSX2 Remote Play MVP

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <SDL3/SDL.h>


#if SDL_MAJOR_VERSION < 3
#error "You are not compiling with SDL3 headers!"
#endif

#define PORT 9000
std::atomic<bool> running = true;

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

void start_host_server() {
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
    server_addr.sin_port = htons(PORT);
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

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    codec_ctx->bit_rate = 400000;
    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->time_base = { 1, 30 };
    codec_ctx->framerate = { 30, 1 };
    codec_ctx->gop_size = 10;
    codec_ctx->max_b_frames = 1;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    av_opt_set(codec_ctx->priv_data, "preset", "ultrafast", 0);
    avcodec_open2(codec_ctx, codec, nullptr);

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

        sws_scale(sws_ctx, inData, inLinesize, 0, height, frame->data, frame->linesize);

        frame->pts = frame_index++;

        avcodec_send_frame(codec_ctx, frame);
        
        while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
            int size = pkt->size;
            send(client_fd, (char*)&size, sizeof(int), 0);
            send(client_fd, (char*)pkt->data, size, 0);
            av_packet_unref(pkt);
        }

        context->Unmap(stagingTex.Get(), 0);
        duplication->ReleaseFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    avcodec_free_context(&codec_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws_ctx);

#ifdef _WIN32
    closesocket(client_fd);
    closesocket(server_fd);
    WSACleanup();
#else
    close(client_fd);
    close(server_fd);
#endif
}

void start_client() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
#endif

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "[Client] Connection failed\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }
    std::cout << "[Client] Connected to host.\n";

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Failed to allocate codec context\n";
        return;
    }
    avcodec_open2(codec_ctx, codec, nullptr);

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    // === SDL3 initialization ===
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
        return;
    }

    SDL_Window* win = SDL_CreateWindow("Remote Play",
        1980, 1020, SDL_WINDOW_FULLSCREEN);

    if (!win) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(win, nullptr);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(win);
        SDL_Quit();
        return;
    }

    SDL_Texture* texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_YV12,
        SDL_TEXTUREACCESS_STREAMING,
        1980, 1020);

    if (!texture) {
        std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return;
    }

    while (running) {
        int size = 0;
        int received = recv(sock, (char*)&size, sizeof(int), 0);
        if (received <= 0) break;

        std::vector<uint8_t> buffer(size);
        int total = 0;
        while (total < size) {
            int r = recv(sock, (char*)buffer.data() + total, size - total, 0);
            if (r <= 0) break;
            total += r;
        }

        // Create a packet from received data
        av_packet_from_data(pkt, buffer.data(), size);
        avcodec_send_packet(codec_ctx, pkt);

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            SDL_UpdateYUVTexture(texture, nullptr,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                frame->data[2], frame->linesize[2]);

            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }
    }

    // Cleanup
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(win);
    SDL_Quit();

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: remote-play [host|client]\n";
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "host") {
        start_host_server();
    }
    else if (mode == "client") {
        start_client();
    }
    else {
        std::cout << "Invalid mode. Use 'host' or 'client'.\n";
        return 1;
    }

    return 0;
}