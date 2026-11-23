// WSA-Next © 2026 Marek Wesołowski - wesmar
// What WSA-Next Setup (WSA-Next.msi) asks WSA-Next.exe to do. Windows Installer runs these as
// LocalSystem (deferred custom actions without impersonation), because its impersonated actions
// get the user's limited token when Setup was started without elevation, and the background host
// and the removal of WSA need administrator rights - a second UAC prompt would be the alternative.
//
//   WSA-Next.exe msi stop-hosts              stop the background host in every Windows session
//                                            (each one holds WSA-Next.exe open)
//   WSA-Next.exe msi uninstall <SID> [--all] run "WSA-Next.exe uninstall --yes --from-setup [--all]"
//                                            as the user with that SID, in that user's session,
//                                            with the user's full (elevated) token, and wait
#pragma once

namespace WN::SetupActions {

// argv[1] is "msi". Returns the process exit code.
int Run(int argc, wchar_t** argv);

}  // namespace WN::SetupActions
