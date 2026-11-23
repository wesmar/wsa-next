// WSA-Next © 2026 Marek Wesołowski - wesmar
// WSA-Next's publisher signature. An official WSA-Next.exe carries a valid Authenticode signature
// by the publisher; the binary's bytes change when it is signed, so a build from the sources is
// recognizable as such - it works, but without the features that only official builds offer
// (Bluetooth and Windows Hello for Android). Official releases of the Android images carry a
// catalog signed the same way (see core/Release.h).
#pragma once

namespace WN::Signature {

enum class Origin {
    Official,       // signed by the publisher, signature intact (timestamped: valid after expiry too)
    Unsigned,       // a build from the sources
    OtherSigner,    // a valid signature of someone else
    Invalid,        // a signature that does not match the file (modified after signing)
};

// Who signed a file (an executable, a catalog): WinVerifyTrust, no network access. `handle`
// (optional): the file opened by the caller, so that exactly those bytes are checked.
[[nodiscard]] Origin CheckFile(const wchar_t* path, void* handle = nullptr);

// This WSA-Next.exe, checked once per process.
[[nodiscard]] Origin Check();
[[nodiscard]] inline bool Official() { return Check() == Origin::Official; }

// The features reserved for official builds (Bluetooth and Windows Hello for Android). Debug
// builds (WSA-Next_debug.exe, development only) have them unsigned.
[[nodiscard]] inline bool OfficialFeatures() {
#ifdef _DEBUG
    return true;
#else
    return Official();
#endif
}

}  // namespace WN::Signature
