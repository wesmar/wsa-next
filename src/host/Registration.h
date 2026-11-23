// WSA-Next © 2026 Marek Wesołowski - wesmar
// Google Play registration of a new installation (Google Play variant). Google knows a new
// installation as an uncertified device until its Google Services Framework ID is entered on
// Google's page. Android's notifications reach Windows, but a click on one does not come back to
// Android for an app without a launcher activity, so the notice is Windows' own:
//
//   WsaNextRegistration (Play services checked in)  --/dev/socket/wsa_elevate "RGWN"-->
//   wsa-elevated (reads the ID)  --vsock "WSRG": "RGID", length, digits-->  this host
//
// The host's notification-area icon shows a notification; a click on it (or its menu item)
// copies the ID to the clipboard and opens Google's page in the user's browser, unelevated. After
// that click a digest of the ID is remembered (not the ID), and that ID is not offered again.
// Reply to Android: 'S' shown, 'O' done before, 'N' no icon to show it (Android then shows its
// own notification with the ID).
#pragma once
#include <cstdint>
#include <stop_token>
#include <string>

namespace WN::Host {

inline constexpr std::uint32_t kRegistrationPort = 0x57535247;  // "WSRG" (device/wsa/wsa_x86_64/elevate)

// Serves Android's registration helper until `stop`. Runs in the multithreaded apartment.
void RunRegistration(std::stop_token stop);

// The ID Android last offered in this host's lifetime; empty when none.
std::wstring PendingRegistrationId();

// Copies the pending ID to the clipboard (`owner`: a window of this thread) and opens Google's
// page in the user's browser; remembers the ID as done. False without a pending ID.
bool RegisterWithGoogle(void* owner);

}  // namespace WN::Host
