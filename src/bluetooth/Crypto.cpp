// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "bluetooth/Crypto.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace WN::Bt {

Block SecurityE(const Block& key, const Block& plaintext) {
    // AES works on the most significant octet first; Bluetooth sends the least significant first.
    Block k, data, out{};
    std::reverse_copy(key.begin(), key.end(), k.begin());
    std::reverse_copy(plaintext.begin(), plaintext.end(), data.begin());
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    ULONG written = 0;
    const bool ok =
        BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                                         sizeof(BCRYPT_CHAIN_MODE_ECB), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, k.data(), 16, 0)) &&
        BCRYPT_SUCCESS(BCryptEncrypt(handle, data.data(), 16, nullptr, nullptr, 0, out.data(), 16, &written, 0));
    if (handle) BCryptDestroyKey(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) throw std::runtime_error("AES-128 unavailable");
    Block result;
    std::reverse_copy(out.begin(), out.end(), result.begin());
    return result;
}

Block C1(const Block& k, const Block& r, std::span<const std::uint8_t, 7> preq, std::span<const std::uint8_t, 7> pres, std::uint8_t iat,
         std::uint64_t ia, std::uint8_t rat, std::uint64_t ra) {
    // p1 = pres || preq || rat' || iat' and p2 = padding || ia || ra, least significant octet first.
    Block p1{}, p2{};
    p1[0] = iat;
    p1[1] = rat;
    std::copy(preq.begin(), preq.end(), p1.begin() + 2);
    std::copy(pres.begin(), pres.end(), p1.begin() + 9);
    for (int i = 0; i < 6; ++i) {
        p2[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(ra >> (8 * i));
        p2[static_cast<std::size_t>(6 + i)] = static_cast<std::uint8_t>(ia >> (8 * i));
    }
    Block x;
    for (std::size_t i = 0; i < 16; ++i) x[i] = static_cast<std::uint8_t>(r[i] ^ p1[i]);
    x = SecurityE(k, x);
    for (std::size_t i = 0; i < 16; ++i) x[i] = static_cast<std::uint8_t>(x[i] ^ p2[i]);
    return SecurityE(k, x);
}

Block S1(const Block& k, const Block& r1, const Block& r2) {
    // r' = r1' || r2': the low halves, r1's in the most significant position.
    Block r{};
    std::copy_n(r2.begin(), 8, r.begin());
    std::copy_n(r1.begin(), 8, r.begin() + 8);
    return SecurityE(k, r);
}

void RandomBytes(std::span<std::uint8_t> out) {
    // Never hand out what happened to be in the buffer: no randomness, no answer.
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("BCryptGenRandom failed");
}

}  // namespace WN::Bt
