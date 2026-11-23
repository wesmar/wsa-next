// WSA-Next © 2026 Marek Wesołowski - wesmar
// WSA-Next Setup (WSA-Next.msi). The same WSA-Next.exe is published in the setup package and in
// WSA-Next.zip; it finds out by itself which way it came:
//
//   * Setup installed: Windows Installer owns %ProgramFiles%\WSA-Next\WSA-Next.exe and its
//     language files. A copy run from elsewhere (the zip) leaves them alone, and removing WSA-Next
//     is handed to Windows Installer, which calls back "WSA-Next.exe uninstall" (app/SetupActions.h)
//     before it removes its own files.
//   * no Setup: WSA-Next copies itself into that folder for the background host, as it always did.
#pragma once
#include "support/Common.h"
#include <optional>
#include <string>

namespace WN::Install {

// The product code of the installed WSA-Next Setup, if any.
std::optional<std::wstring> SetupProduct();

// Starts the removal through Windows Installer (msiexec /x, a progress bar only) and returns at
// once: the caller ends, so that its executable is not in use when Windows Installer removes it.
// `everything`: also the Android apps and their data (Setup's REMOVEDATA).
Result<> RemoveThroughSetup(const std::wstring& product, bool everything);

}  // namespace WN::Install
