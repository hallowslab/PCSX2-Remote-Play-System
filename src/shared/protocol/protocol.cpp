#include "protocol.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <chrono>

namespace rps {

uint64_t get_timestamp_us() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

uint32_t to_network_u32(uint32_t value) {
    return htonl(value);
}

uint32_t from_network_u32(uint32_t value) {
    return ntohl(value);
}

uint64_t to_network_u64(uint64_t value) {
    // Manual conversion for 64-bit (htonll not always available)
    uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
    uint32_t low = htonl(static_cast<uint32_t>(value & 0xFFFFFFFF));
    return (static_cast<uint64_t>(low) << 32) | high;
}

uint64_t from_network_u64(uint64_t value) {
    uint32_t high = ntohl(static_cast<uint32_t>(value >> 32));
    uint32_t low = ntohl(static_cast<uint32_t>(value & 0xFFFFFFFF));
    return (static_cast<uint64_t>(low) << 32) | high;
}

} // namespace rps
