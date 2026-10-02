#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace runtime::ipc {

constexpr std::size_t header_size = 10;
constexpr std::uint32_t max_payload = 64 * 1024;
enum class Type : std::uint16_t {
    start = 1, stop = 2, query_status = 3, heartbeat = 4, event = 5,
    restart_service = 6, get_service_list = 7, error = 255
};

struct Frame {
    std::uint16_t type;
    std::uint32_t request_id;
    std::string payload;
};

std::string encode(const Frame& frame);
Frame decode(const std::string& bytes);

} // namespace runtime::ipc
