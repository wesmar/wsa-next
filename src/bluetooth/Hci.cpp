// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "bluetooth/Hci.h"
#include <algorithm>
#include <array>

namespace WN::Bt::Hci {
namespace {

constexpr OpcodeInfo kOpcodes[] = {
#include "bluetooth/Opcodes.inc"
};

}  // namespace

std::span<const OpcodeInfo> AllOpcodes() { return kOpcodes; }

const OpcodeInfo* FindOpcode(std::uint16_t opcode) {
    const auto it = std::ranges::find(kOpcodes, opcode, &OpcodeInfo::opcode);
    return it == std::end(kOpcodes) ? nullptr : &*it;
}

std::string_view OpcodeName(std::uint16_t opcode) {
    if (const OpcodeInfo* info = FindOpcode(opcode)) return info->name;
    return (opcode >> 10) == 0x3F ? "VENDOR_SPECIFIC" : "?";
}

}  // namespace WN::Bt::Hci
