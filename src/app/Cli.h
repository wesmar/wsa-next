// WSA-Next © 2026 Marek Wesołowski - wesmar
// Command-line mode: any argument on the command line selects it. Always English (scripts parse it).
#pragma once
#include <string>

namespace WN::Cli {

// Exit codes (documented in --help).
enum Exit : int { kOk = 0, kFailed = 1, kUsage = 2, kUnavailable = 3, kCanceled = 4, kRestart = 5 };

int Run(int argc, wchar_t** argv);

// The --help text (also shown by Help > Command line in the GUI).
std::wstring HelpText();

// Manifest URL for a --source value: local, mirror, github or a URL.
std::wstring SourceUrl(const std::wstring& source);

}  // namespace WN::Cli
