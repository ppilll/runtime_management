#include "frame.hpp"
#include <stdexcept>

namespace runtime::ipc {
namespace {
void put16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value));
    out.push_back(static_cast<char>(value >> 8));
}
void put32(std::string& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>(value >> shift));
}
std::uint16_t get16(const std::string& bytes, std::size_t offset) {
    return static_cast<std::uint8_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8);
}
std::uint32_t get32(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + i])) << (8 * i);
    return value;
}
} // namespace

std::string encode(const Frame& frame) {
    if (frame.payload.size() > max_payload) throw std::length_error("IPC payload too large");
    std::string bytes;
    bytes.reserve(header_size + frame.payload.size());
    put32(bytes, static_cast<std::uint32_t>(frame.payload.size()));
    put16(bytes, frame.type);
    put32(bytes, frame.request_id);
    bytes += frame.payload;
    return bytes;
}

Frame decode(const std::string& bytes) {
    if (bytes.size() < header_size) throw std::runtime_error("short IPC header");
    const auto length = get32(bytes, 0);
    if (length > max_payload || bytes.size() != header_size + length)
        throw std::runtime_error("invalid IPC frame length");
    return Frame{get16(bytes, 4), get32(bytes, 6), bytes.substr(header_size)};
}

} // namespace runtime::ipc
