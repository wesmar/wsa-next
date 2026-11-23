// WSA-Next © 2026 Marek Wesołowski - wesmar
// Little-endian byte building and reading for HCI packets.
#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace WN::Bt {

class Bytes {
public:
    Bytes& u8(std::uint8_t v) { m_data.push_back(v); return *this; }
    Bytes& u16(std::uint16_t v) { return u8(static_cast<std::uint8_t>(v)).u8(static_cast<std::uint8_t>(v >> 8)); }
    // The low `count` bytes of v (addresses are 6 bytes, feature masks 8).
    Bytes& le(std::uint64_t v, int count) {
        for (int i = 0; i < count; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Bytes& raw(std::span<const std::uint8_t> data) { m_data.insert(m_data.end(), data.begin(), data.end()); return *this; }
    Bytes& zeros(std::size_t count) { m_data.insert(m_data.end(), count, 0); return *this; }

    [[nodiscard]] const std::vector<std::uint8_t>& data() const { return m_data; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(m_data); }
    [[nodiscard]] std::size_t size() const { return m_data.size(); }

private:
    std::vector<std::uint8_t> m_data;
};

// Bounds-checked reader over a received packet's parameters.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : m_data(data) {}
    [[nodiscard]] std::uint8_t u8() { Need(1); return m_data[m_pos++]; }
    [[nodiscard]] std::uint16_t u16() { Need(2); const auto v = static_cast<std::uint16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8)); m_pos += 2; return v; }
    [[nodiscard]] std::uint64_t le(int count) {
        Need(static_cast<std::size_t>(count));
        std::uint64_t v = 0;
        for (int i = 0; i < count; ++i) v |= static_cast<std::uint64_t>(m_data[m_pos + i]) << (8 * i);
        m_pos += static_cast<std::size_t>(count);
        return v;
    }
    [[nodiscard]] std::span<const std::uint8_t> bytes(std::size_t count) { Need(count); auto s = m_data.subspan(m_pos, count); m_pos += count; return s; }
    [[nodiscard]] std::size_t left() const { return m_data.size() - m_pos; }

private:
    void Need(std::size_t count) const {
        if (m_data.size() - m_pos < count) throw std::out_of_range("HCI packet too short");
    }
    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
};

}  // namespace WN::Bt
