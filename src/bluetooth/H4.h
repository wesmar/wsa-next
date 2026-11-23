// WSA-Next © 2026 Marek Wesołowski - wesmar
// H4 framing over the vsock stream: one packet indicator byte, then the HCI packet. The host
// sends commands, ACL, SCO and ISO data; the bridge answers with events and ACL data.
#pragma once
#include "bluetooth/Hci.h"
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace WN::Bt {

class H4Reader {
public:
    using PacketFn = std::function<void(Hci::PacketType, std::span<const std::uint8_t>)>;
    explicit H4Reader(PacketFn onPacket) : m_onPacket(std::move(onPacket)) {}

    // Feeds received bytes; calls onPacket for every complete packet. False: framing is broken
    // (unknown packet indicator) and the connection should be dropped.
    bool Feed(std::span<const std::uint8_t> data);

private:
    // Header length and payload length of the packet being collected (0 = header incomplete).
    [[nodiscard]] std::size_t HeaderSize() const;
    [[nodiscard]] std::size_t PayloadSize() const;

    PacketFn m_onPacket;
    std::vector<std::uint8_t> m_packet;  // indicator + header + payload collected so far
};

// One H4 packet ready to send: indicator + packet.
std::vector<std::uint8_t> H4Frame(Hci::PacketType type, std::span<const std::uint8_t> packet);

}  // namespace WN::Bt
