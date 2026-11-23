// WSA-Next © 2026 Marek Wesołowski - wesmar
// Bluetooth security functions on Windows CNG (Core Vol 3 Part H 2.2): values are 128-bit and,
// as on the wire, least significant octet first.
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace WN::Bt {

using Block = std::array<std::uint8_t, 16>;

// e(key, plaintext): AES-128 (also HCI LE Encrypt).
Block SecurityE(const Block& key, const Block& plaintext);

// LE legacy pairing: confirm value c1 and short term key s1.
//   preq / pres: the 7 octets of the Pairing Request / Response PDUs (opcode included)
//   ia / ra: initiator / responder addresses, iat / rat: their types (0 public, 1 random)
Block C1(const Block& k, const Block& r, std::span<const std::uint8_t, 7> preq, std::span<const std::uint8_t, 7> pres, std::uint8_t iat,
         std::uint64_t ia, std::uint8_t rat, std::uint64_t ra);
Block S1(const Block& k, const Block& r1, const Block& r2);

// Cryptographically random bytes.
void RandomBytes(std::span<std::uint8_t> out);

}  // namespace WN::Bt
