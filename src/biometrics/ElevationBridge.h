// WSA-Next © 2026 Marek Wesołowski - wesmar
// Elevation bridge: Android's elevation daemon in WSA <-> Hyper-V socket ("WSEV") <-> Windows Hello.
//
// Root for Android apps without Magisk and without changing a byte of the signed images: the
// daemon is part of the official system_ext image, and every request is confirmed on Windows with
// Windows Hello (fingerprint, face or PIN). Off by default; the background host runs this bridge
// only while the user has turned the feature on.
#pragma once
#include <stop_token>

namespace WN::Bio::Elevation {

// Connects to the daemon whenever WSA runs and decides its requests until `stop` is requested.
// Every decision is written to the bridge log (bluetooth.log), granted or not. `suForApps`: the
// daemon also offers root apps the name they look for, su (a link to 0, gone when the bridge is).
void RunBridge(std::stop_token stop, bool suForApps);

}  // namespace WN::Bio::Elevation
