#pragma once

#include <string>

namespace rps {

// Start the host server on the specified port. config_path points to the game
// library config (config.ini); empty = no control plane.
void start_host_server(int port, bool &running, bool debug_audio = false,
                       const std::string &config_path = "config.ini");

} // namespace rps
