// WSA-Next © 2026 Marek Wesołowski - wesmar
// MMS for Android through the PC's modem. Android has no cellular network: its MmsService
// (android/patches/packages/services/Mms) hands each HTTP exchange to the wsa-mms daemon
// (device/wsa/wsa_x86_64/telephony), which the host reaches on vsock port "WSMM". The first line
// names the APN and the server; the host opens that APN on the modem as a connection of its own
// (ConnectivityManager.AcquireConnectionAsync: a second packet context, the PC's internet stays
// as it is), connects to the server through it, answers "OK" and passes the bytes through. The
// APN is open only while an exchange runs.
#pragma once
#include <stop_token>

namespace WN::Tel {

// Serves Android's MMS exchanges until `stop`. Runs in the multithreaded apartment.
void RunMmsRelay(std::stop_token stop);

}  // namespace WN::Tel
