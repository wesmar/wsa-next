// WSA-Next © 2026 Marek Wesołowski - wesmar
// Telephony bridge: the PC's modem for Android's RIL in WSA (vsock port "WSRL"). Android gets the
// SIM, the network and SMS; without a mobile broadband modem the bridge does not run and Android
// shows no SIM, as a phone without one. Runs in the background host (src/host).
#pragma once
#include <stop_token>

namespace WN::Tel {

// Serves Android's RIL until `stop`; returns at once when this PC has no modem.
void RunBridge(std::stop_token stop);

}  // namespace WN::Tel
