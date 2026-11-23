// WSA-Next © 2026 Marek Wesołowski - wesmar
// HTTP downloads (WinHTTP). HTTPS everywhere; plain HTTP only to this computer (a test server on
// localhost). https -> http redirects are refused.
#pragma once
#include "support/Common.h"
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>

namespace WN::Net {

// Small text resource (the manifest), at most `limit` bytes, returned as received (UTF-8).
Result<std::string> FetchText(const std::wstring& url, std::size_t limit, std::stop_token stop);

// Downloads `url` into `part`, resuming a partial file with an HTTP range request, retrying up to
// `attempts` times after network errors. The finished file must have `expectedSize` bytes and the
// SHA-256 `expectedSha256` (lower-case hex); otherwise it is deleted and an error returned.
// `progress` gets bytes of the whole file (resumed bytes included).
Result<> Download(const std::wstring& url, const std::filesystem::path& part, std::uint64_t expectedSize,
                  const std::wstring& expectedSha256, int attempts, const ProgressFn& progress, const LogFn& log,
                  std::stop_token stop);

// SHA-256 of a file (lower-case hex), with progress.
Result<std::wstring> HashFile(const std::filesystem::path& file, const ProgressFn& progress, std::stop_token stop);

}  // namespace WN::Net
