#pragma once

#include <string>

namespace rps {

// Start the client connecting to the specified host. launch_name (optional)
// requests that game be launched after connecting.
void start_client(const char *ip_addr, int port, bool &running,
                  const std::string &launch_name = "");

} // namespace rps
