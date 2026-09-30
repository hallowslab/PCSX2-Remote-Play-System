#pragma once

namespace rps {

// Start the client connecting to the specified host
void start_client(const char *ip_addr, int port, bool &running);

} // namespace rps
