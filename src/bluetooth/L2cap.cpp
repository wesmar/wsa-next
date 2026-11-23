// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "bluetooth/L2cap.h"
#include <algorithm>

namespace WN::Bt {
namespace {

constexpr std::uint8_t kPbFirstNonFlushable = 0b00, kPbContinuation = 0b01, kPbFirstFlushable = 0b10;

}  // namespace

std::uint16_t AclHandle(std::span<const std::uint8_t> acl) {
    if (acl.size() < 4) return 0xFFFF;
    return static_cast<std::uint16_t>((acl[0] | (acl[1] << 8)) & 0x0FFF);
}

std::optional<L2capFrame> AclReassembler::Feed(std::span<const std::uint8_t> acl) {
    if (acl.size() < 4) return std::nullopt;
    const std::uint16_t header = static_cast<std::uint16_t>(acl[0] | (acl[1] << 8));
    const std::uint8_t pb = static_cast<std::uint8_t>((header >> 12) & 0x3);
    const std::size_t length = static_cast<std::size_t>(acl[2] | (acl[3] << 8));
    const auto data = acl.subspan(4, std::min(length, acl.size() - 4));

    if (pb == kPbFirstNonFlushable || pb == kPbFirstFlushable) {
        m_pending.assign(data.begin(), data.end());
        m_handle = static_cast<std::uint16_t>(header & 0x0FFF);
        m_expected = 0;
    } else if (pb == kPbContinuation && !m_pending.empty()) {
        m_pending.insert(m_pending.end(), data.begin(), data.end());
    } else {
        return std::nullopt;
    }
    if (m_expected == 0) {
        if (m_pending.size() < 4) return std::nullopt;
        m_expected = 4 + static_cast<std::size_t>(m_pending[0] | (m_pending[1] << 8));
    }
    if (m_pending.size() < m_expected) return std::nullopt;

    L2capFrame frame;
    frame.handle = m_handle;
    frame.cid = static_cast<std::uint16_t>(m_pending[2] | (m_pending[3] << 8));
    frame.payload.assign(m_pending.begin() + 4, m_pending.begin() + static_cast<std::ptrdiff_t>(m_expected));
    m_pending.clear();
    m_expected = 0;
    return frame;
}

std::vector<std::vector<std::uint8_t>> AclPackets(std::uint16_t handle, std::uint16_t cid, std::span<const std::uint8_t> payload,
                                                  std::size_t maxData) {
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 4);
    frame.push_back(static_cast<std::uint8_t>(payload.size()));
    frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    frame.push_back(static_cast<std::uint8_t>(cid));
    frame.push_back(static_cast<std::uint8_t>(cid >> 8));
    frame.insert(frame.end(), payload.begin(), payload.end());

    std::vector<std::vector<std::uint8_t>> packets;
    for (std::size_t offset = 0; offset < frame.size(); offset += maxData) {
        const std::size_t take = std::min(maxData, frame.size() - offset);
        const std::uint8_t pb = offset == 0 ? kPbFirstFlushable : kPbContinuation;
        const std::uint16_t header = static_cast<std::uint16_t>((handle & 0x0FFF) | (pb << 12));
        std::vector<std::uint8_t> packet{static_cast<std::uint8_t>(header), static_cast<std::uint8_t>(header >> 8),
                                         static_cast<std::uint8_t>(take), static_cast<std::uint8_t>(take >> 8)};
        packet.insert(packet.end(), frame.begin() + static_cast<std::ptrdiff_t>(offset),
                      frame.begin() + static_cast<std::ptrdiff_t>(offset + take));
        packets.push_back(std::move(packet));
    }
    return packets;
}

}  // namespace WN::Bt
