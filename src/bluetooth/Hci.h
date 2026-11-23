// WSA-Next © 2026 Marek Wesołowski - wesmar
// HCI constants used by the bridge (Bluetooth Core Specification, Vol 4 Part E).
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace WN::Bt::Hci {

// H4 packet indicators.
enum class PacketType : std::uint8_t { Command = 1, Acl = 2, Sco = 3, Event = 4, Iso = 5 };

// Events.
inline constexpr std::uint8_t kEvtInquiryComplete = 0x01;
inline constexpr std::uint8_t kEvtCommandComplete = 0x0E;
inline constexpr std::uint8_t kEvtCommandStatus = 0x0F;
inline constexpr std::uint8_t kEvtLeMeta = 0x3E;
inline constexpr std::uint8_t kLeAdvertisingReport = 0x02;

// Status codes.
inline constexpr std::uint8_t kSuccess = 0x00;
inline constexpr std::uint8_t kUnknownCommand = 0x01;
inline constexpr std::uint8_t kInvalidParameters = 0x12;
inline constexpr std::uint8_t kCommandDisallowed = 0x0C;

// Legacy advertising report event types.
inline constexpr std::uint8_t kAdvInd = 0x00;
inline constexpr std::uint8_t kAdvDirectInd = 0x01;
inline constexpr std::uint8_t kAdvScanInd = 0x02;
inline constexpr std::uint8_t kAdvNonconnInd = 0x03;
inline constexpr std::uint8_t kScanRsp = 0x04;

struct OpcodeInfo {
    std::uint16_t opcode;
    std::uint8_t octet, bit;  // position in the Read Local Supported Commands mask
    const char* name;
};

// Every opcode RootCanal's PDL knows (Opcodes.inc), sorted by opcode.
std::span<const OpcodeInfo> AllOpcodes();
const OpcodeInfo* FindOpcode(std::uint16_t opcode);
std::string_view OpcodeName(std::uint16_t opcode);

}  // namespace WN::Bt::Hci
