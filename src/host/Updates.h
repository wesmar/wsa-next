// WSA-Next © 2026 Marek Wesołowski - wesmar
// The background host's look for updates (core/Update.h): no timer, no polling. It waits for
// Windows to report the network (once at the start, then on every change of connectivity) and
// reads the release manifest only when the last check is old enough and the PC has Internet access
// (any network, the mobile modem too). A failure is silent; the next change of the network, or the
// next logon, tries again.
#pragma once
#include <stop_token>

namespace WN::Host {

void RunUpdateCheck(std::stop_token stop);

}  // namespace WN::Host
