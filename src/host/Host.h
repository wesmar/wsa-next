// WSA-Next © 2026 Marek Wesołowski - wesmar
// The Windows side of WSA-Next's Android integration: one background process of the user's
// session, "WSA-Next.exe host --background", running
//   the Windows Hello bridge   always: Android's fingerprint sensor (src/biometrics); it answers
//                              "not available" while Windows Hello is not set up
//   the Bluetooth bridge       while Bluetooth for Android is on (setting "Bluetooth", src/bluetooth)
//   the elevation bridge       while root for Android apps is on (setting "Elevation"): every
//                              request confirmed with Windows Hello (src/biometrics)
//   the look for updates       a newer WSA-Next or Android release (host/Updates.h): a dot on the
//                              notification-area icon
// One instance per session (named mutex). The window and the command line control it through
// named events (stop, reload settings); the Task Scheduler task "WSA-Next" starts it at logon with
// the highest privileges (both bridges need administrator rights). Closing the window does not
// stop it: Android keeps its Bluetooth and fingerprint.
#pragma once
#include "support/Common.h"
#include <cstdint>

namespace WN::Host {

inline constexpr wchar_t kAutostartTask[] = L"WSA-Next";

// Body of "host --background". Returns at once when another instance already runs.
int RunBackground();

[[nodiscard]] bool Running();
Result<> Start();    // starts it unless it runs (the caller is elevated, so is the child)
void Stop();         // asks it to stop and waits until it has
Result<> Restart();  // from the installed WSA-Next.exe (after an update of it)
void Reload();       // the running host reads its settings again (and its icon redraws)

// The WSA-Next.exe the host and its logon task run: the installed copy in %ProgramFiles%\WSA-Next
// (only administrators may change it) when there is one, otherwise this executable.
[[nodiscard]] std::wstring Executable();

// Bluetooth for Android: the setting, applied to the running host (started if needed).
[[nodiscard]] bool BluetoothEnabled();
Result<> SetBluetoothEnabled(bool enabled);

// Root for Android apps, each request confirmed with Windows Hello (setting "Elevation", off by
// default): applied to the running host like Bluetooth.
[[nodiscard]] bool ElevationEnabled();
Result<> SetElevationEnabled(bool enabled);

// "su for root apps" (setting "ElevationSu", off): while root is on, Android also offers apps the
// name su (a link to 0) - root apps work, and root checks of other apps see it too.
[[nodiscard]] bool ElevationSuEnabled();
Result<> SetElevationSuEnabled(bool enabled);

// How long a Windows Hello verification is remembered for Android apps that ask for a fingerprint
// (setting "HelloRemember", in minutes; 0 = ask every time, the default; kHelloUntilLocked = until
// Windows is locked). Locking Windows, signing out or disconnecting always forgets it. Read by the
// Windows Hello bridge for every request; root for Android apps (the elevation bridge) always asks.
inline constexpr std::uint32_t kHelloUntilLocked = 0xFFFFFFFF;
[[nodiscard]] std::uint32_t HelloRememberMinutes();
void SetHelloRememberMinutes(std::uint32_t minutes);

// The host's icon in the notification area (setting "Tray", on by default; src/host/Tray.h).
[[nodiscard]] bool TrayEnabled();
Result<> SetTrayEnabled(bool enabled);

// Start with Windows: the logon task for the current user.
[[nodiscard]] bool AutostartEnabled();
Result<> SetAutostart(bool enable);

}  // namespace WN::Host
