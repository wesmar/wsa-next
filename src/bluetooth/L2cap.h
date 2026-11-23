// WSA-Next © 2026 Marek Wesołowski - wesmar
// L2CAP over HCI ACL for LE links: reassembly of what the host sends, fragmentation of what the
// bridge answers (Bluetooth Core Vol 3 Part A; LE uses the basic frame format only).
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace WN::Bt {

// Fixed LE channels.
inline constexpr std::uint16_t kCidAtt = 0x0004;
inline constexpr std::uint16_t kCidLeSignaling = 0x0005;
inline constexpr std::uint16_t kCidSmp = 0x0006;

struct L2capFrame {
    std::uint16_t handle = 0;
    std::uint16_t cid = 0;
    std::vector<std::uint8_t> payload;
};

// Collects the ACL fragments of one connection into L2CAP frames.
class AclReassembler {
public:
    // One HCI ACL packet (header included). Returns a frame when it completes one; malformed
    // input drops the partial frame.
    std::optional<L2capFrame> Feed(std::span<const std::uint8_t> acl);

private:
    std::vector<std::uint8_t> m_pending;  // L2CAP header + payload collected so far
    std::size_t m_expected = 0;           // L2CAP header + payload length of the frame in progress
    std::uint16_t m_handle = 0;
};

// HCI ACL packets (header included) carrying one L2CAP frame, each at most `maxData` bytes of data.
std::vector<std::vector<std::uint8_t>> AclPackets(std::uint16_t handle, std::uint16_t cid, std::span<const std::uint8_t> payload,
                                                  std::size_t maxData);

// Connection handle of an ACL packet (0xFFFF when too short).
std::uint16_t AclHandle(std::span<const std::uint8_t> acl);

}  // namespace WN::Bt
