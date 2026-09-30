// Project: PCSX2 Remote Play MVP

#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>

#include "CLI11.hpp"

#include "client/client.h"
#include "host/host.h"

#include <SDL3/SDL.h>

#if SDL_MAJOR_VERSION < 3
#error "You are not compiling with SDL3 headers!"
#endif

static bool running = true;

int main(int argc, char *argv[]) {
  CLI::App app{"PCSX2 Remote Play"};

  std::string mode;
  std::string ip;
  int port = 51234;
  bool debug_audio = false;

  app.add_option("-m,--mode", mode, "Mode: client or host")->required();

  app.add_option("-i,--ip", ip, "IP address of the server")->default_val("");

  app.add_option("-p,--port", port, "Port to connect/listen on")
      ->default_val("51234");

  app.add_flag("--debug-audio", debug_audio,
               "Record raw PCM on host for debugging");

  CLI11_PARSE(app, argc, argv);

  if (mode == "host") {
    rps::start_host_server(port, running, debug_audio);
  } else if (mode == "client") {
    if (ip == "") {
      std::cerr
          << "If running client you need to specify IP address: -i x.x.x.x\n";
      return 1;
    }
    rps::start_client(ip.c_str(), port, running);
  } else {
    std::cerr << "Invalid mode: use 'host' or 'client'\n";
    return 1;
  }

  return 0;
}