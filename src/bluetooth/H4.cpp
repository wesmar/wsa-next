// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "bluetooth/H4.h"
#include <algorithm>

namespace WN::Bt {

std::size_t H4Reader::HeaderSize() const {
    switch (static_cast<Hci::PacketType>(m_packet[0])) {
    case Hci::PacketType::Command: return 3;  // opcode(2) length(1)
    case Hci::PacketType::Acl: return 4;      // handle+flags(2) length(2)
    case Hci::PacketType::Sco: return 3;      // handle+flags(2) length(1)
    case Hci::PacketType::Event: return 2;    // code(1) length(1)
    case Hci::PacketType::Iso: return 4;      // handle+flags(2) length(14 bits)
    default: return 0;
    }
}

std::size_t H4Reader::PayloadSize() const {
    const std::uint8_t* h = m_packet.data() + 1;
    switch (static_cast<Hci::PacketType>(m_packet[0])) {
    case Hci::PacketType::Command: return h[2];
    case Hci::PacketType::Acl: return static_cast<std::size_t>(h[2] | (h[3] << 8));
    case Hci::PacketType::Sco: return h[2];
    case Hci::PacketType::Event: return h[1];
    case Hci::PacketType::Iso: return static_cast<std::size_t>((h[2] | (h[3] << 8)) & 0x3FFF);
    default: return 0;
    }
}

bool H4Reader::Feed(std::span<const std::uint8_t> data) {
    while (!data.empty()) {
        if (m_packet.empty()) {
            const std::uint8_t type = data[0];
            if (type < 1 || type > 5) return false;
            m_packet.push_back(type);
            data = data.subspan(1);
            continue;
        }
        const std::size_t header = HeaderSize();
        std::size_t want = 1 + header;
        if (m_packet.size() >= want) want += PayloadSize();
        const std::size_t take = std::min(want - m_packet.size(), data.size());
        m_packet.insert(m_packet.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(take));
        data = data.subspan(take);
        if (m_packet.size() == 1 + header) {
            if (PayloadSize() != 0) continue;  // header complete, payload follows
        } else if (m_packet.size() < 1 + header || m_packet.size() < 1 + header + PayloadSize()) {
            continue;
        }
        const auto type = static_cast<Hci::PacketType>(m_packet[0]);
        m_onPacket(type, std::span<const std::uint8_t>(m_packet).subspan(1));
        m_packet.clear();
    }
    return true;
}

std::vector<std::uint8_t> H4Frame(Hci::PacketType type, std::span<const std::uint8_t> packet) {
    std::vector<std::uint8_t> frame;
    frame.reserve(packet.size() + 1);
    frame.push_back(static_cast<std::uint8_t>(type));
    frame.insert(frame.end(), packet.begin(), packet.end());
    return frame;
}

}  // namespace WN::Bt
