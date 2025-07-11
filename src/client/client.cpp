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


int recvall(int sock, char* buf, int len) {
    int total = 0;
    while (total < len) {
        int r = recv(sock, buf + total, len - total, 0);
        if (r <= 0) return r;
        total += r;
    }
    return total;
}

void start_client(const char* ip_addr,int port, bool& running) {
    #ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    #endif

    #ifdef _WIN32
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        char flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    #else
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    #endif

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip_addr, &server_addr.sin_addr);

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
        1980, 1020, SDL_WINDOW_RESIZABLE);

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
        int net_size = 0;
        int received = recvall(sock, (char*)&net_size, sizeof(net_size));
        if (received <= 0) {
            std::cout << "[Client] Connection closed or error on size recv\n";
            break;
        }
        net_size = ntohl(net_size);
        //std::cout << "[Client] recv size returned: " << received << ", size: " << net_size << std::endl;
        if (received <= 0) {
            std::cout << "[Client] Connection closed or error on size recv\n";
            break;
        }

        std::vector<uint8_t> buffer(net_size);
        received = recvall(sock, (char*)buffer.data(), net_size);
        if (received <= 0) {
            std::cout << "[Client] Connection closed or error on frame data recv\n";
            running = false;
            break;
        }

        //av_packet_unref(pkt);
        av_new_packet(pkt, net_size);
        memcpy(pkt->data, buffer.data(), net_size);

        int ret = avcodec_send_packet(codec_ctx, pkt);
        if (ret < 0) {
            std::cerr << "Error sending packet to decoder: " << ret << "\n";
            break;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            else if (ret < 0) {
                std::cerr << "Error receiving frame from decoder: " << ret << "\n";
                break;
            }

            SDL_UpdateYUVTexture(texture, nullptr,
                frame->data[0], frame->linesize[0],
                frame->data[1], frame->linesize[1],
                frame->data[2], frame->linesize[2]);

            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        // Poll SDL events to allow window closing
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
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