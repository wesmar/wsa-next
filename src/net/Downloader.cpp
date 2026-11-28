// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "net/Downloader.h"
#include "support/Config.h"
#include "support/Handle.h"
#include "support/Sha256.h"
#include "support/Text.h"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cwctype>
#include <format>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace WN::Net {
namespace fs = std::filesystem;
namespace {

struct InternetCloser {
    void operator()(HINTERNET h) const noexcept { if (h) WinHttpCloseHandle(h); }
};
using Internet = std::unique_ptr<std::remove_pointer_t<HINTERNET>, InternetCloser>;

struct Http {
    Internet session, connection, request;
    DWORD status = 0;
};

bool IsLoopback(std::wstring host) {
    std::ranges::transform(host, host.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1";
}

// Sends one request. `from` > 0 asks for the bytes from that offset on (Range).
Result<Http> Open(const std::wstring& url, const wchar_t* verb, std::uint64_t from, DWORD connectMs, DWORD ioMs) {
    wchar_t host[256]{}, path[2048]{}, extra[2048]{};
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;     uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;      uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;   uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return Fail(ErrorText(Text::Get(IDS_CTX_INVALID_URL), GetLastError()));
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !(uc.nScheme == INTERNET_SCHEME_HTTP && IsLoopback(host))) return Fail(Text::Get(IDS_HTTPS_ONLY));
    const std::wstring target = std::wstring(path) + extra;

    Http http;
    const std::wstring agent = std::format(L"WSA-Next/{}", Config::kAppVersion);
    http.session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0));
    if (!http.session) return Fail(ErrorText(L"WinHttpOpen", GetLastError()));
    WinHttpSetTimeouts(http.session.get(), static_cast<int>(connectMs), static_cast<int>(connectMs), static_cast<int>(ioMs),
                       static_cast<int>(ioMs));
    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;  // GitHub's CDN speaks HTTP/2
    WinHttpSetOption(http.session.get(), WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof protocols);

    http.connection.reset(WinHttpConnect(http.session.get(), host, uc.nPort, 0));
    if (!http.connection) return Fail(ErrorText(Text::Get(IDS_CTX_CONNECT), GetLastError()));
    http.request.reset(WinHttpOpenRequest(http.connection.get(), verb, target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!http.request) return Fail(ErrorText(L"WinHttpOpenRequest", GetLastError()));
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(http.request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof redirectPolicy);

    const std::wstring range = from ? std::format(L"Range: bytes={}-\r\n", from) : std::wstring();
    if (!WinHttpSendRequest(http.request.get(), range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(),
                            range.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(http.request.get(), nullptr))
        return Fail(ErrorText(Text::Get(IDS_CTX_REQUEST), GetLastError()));
    DWORD size = sizeof http.status;
    if (!WinHttpQueryHeaders(http.request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &http.status, &size, WINHTTP_NO_HEADER_INDEX))
        return Fail(ErrorText(Text::Get(IDS_CTX_STATUS), GetLastError()));
    return http;
}

// Reads the response body in chunks; `sink` returns false to stop with an error already set.
Result<> ReadBody(HINTERNET request, std::stop_token stop, const std::function<Result<>(const char*, DWORD)>& sink) {
    std::vector<char> buffer(1u << 20);
    for (;;) {
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) return Fail(ErrorText(Text::Get(IDS_CTX_DOWNLOADING), GetLastError()));
        if (!available) return {};
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read))
            return Fail(ErrorText(Text::Get(IDS_CTX_DOWNLOADING), GetLastError()));
        if (!read) return {};
        if (auto r = sink(buffer.data(), read); !r) return r;
    }
}

// Hashes the bytes already in `file` (a resumed download continues the same digest).
Result<> HashExisting(const fs::path& file, std::uint64_t bytes, Sha256& hash, std::stop_token stop) {
    FileHandle in(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!in) return Fail(ErrorText(Text::Format(IDS_CTX_OPEN, file.wstring()), GetLastError()));
    std::vector<char> buffer(1u << 20);
    while (bytes) {
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(bytes, buffer.size()));
        if (!ReadFile(in.get(), buffer.data(), want, &read, nullptr) || read != want)
            return Fail(ErrorText(Text::Format(IDS_CTX_READ, file.wstring()), GetLastError()));
        hash.Update(buffer.data(), read);
        bytes -= read;
    }
    return {};
}

// One attempt: continues `part` from its current size.
Result<> Attempt(const std::wstring& url, const fs::path& part, std::uint64_t expectedSize, std::unique_ptr<Sha256>& hash, std::uint64_t& done,
                 const ProgressFn& progress, std::stop_token stop) {
    auto http = Open(url, L"GET", done, 15000, 60000);
    if (!http) return Fail(http.error());
    if (done && http->status == 200) {  // server ignored the range: start over
        done = 0;
        hash = std::make_unique<Sha256>();
    } else if (done && http->status == 416) {  // nothing left to send
        return {};
    } else if (http->status != (done ? 206u : 200u)) {
        return Fail(Text::Format(IDS_HTTP_STATUS, http->status));
    }

    FileHandle file(CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return Fail(ErrorText(Text::Format(IDS_CTX_CREATE, part.wstring()), GetLastError()));
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(done);
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN) || !SetEndOfFile(file.get()))
        return Fail(ErrorText(Text::Format(IDS_CTX_WRITE, part.wstring()), GetLastError()));

    progress(done, expectedSize);
    return ReadBody(http->request.get(), stop, [&](const char* data, DWORD n) -> Result<> {
        if (done + n > expectedSize) return Fail(Text::Get(IDS_DOWNLOAD_TOO_LARGE));
        DWORD written = 0;
        if (!WriteFile(file.get(), data, n, &written, nullptr) || written != n)
            return Fail(ErrorText(Text::Format(IDS_CTX_WRITE, part.wstring()), GetLastError()));
        hash->Update(data, n);
        done += n;
        progress(done, expectedSize);
        return {};
    });
}

}  // namespace

Result<std::string> FetchText(const std::wstring& url, std::size_t limit, std::stop_token stop, DWORD connectMs, DWORD ioMs) {
    auto http = Open(url, L"GET", 0, connectMs, ioMs);
    if (!http) return Fail(http.error());
    if (http->status != 200) return Fail(Text::Format(IDS_HTTP_STATUS_URL, http->status, url));
    std::string body;
    auto r = ReadBody(http->request.get(), stop, [&](const char* data, DWORD n) -> Result<> {
        if (body.size() + n > limit) return Fail(Text::Get(IDS_DOWNLOAD_TOO_LARGE));
        body.append(data, n);
        return {};
    });
    if (!r) return Fail(r.error());
    return body;
}

Result<> Download(const std::wstring& url, const fs::path& part, std::uint64_t expectedSize, const std::wstring& expectedSha256,
                  int attempts, const ProgressFn& progress, const LogFn& log, std::stop_token stop) {
    std::error_code ec;
    fs::create_directories(part.parent_path(), ec);
    auto hash = std::make_unique<Sha256>();
    if (!hash->ok()) return Fail(Text::Get(IDS_SHA_UNAVAILABLE));

    std::uint64_t done = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (ec || done > expectedSize) done = 0;
    if (done) {
        log(Text::Format(IDS_LOG_RESUME, FormatBytes(done), FormatBytes(expectedSize)));
        if (auto r = HashExisting(part, done, *hash, stop); !r) return r;
    }

    Result<> last;
    for (int attempt = 1; attempt <= std::max(attempts, 1); ++attempt) {
        if (done == expectedSize) { last = {}; break; }
        last = Attempt(url, part, expectedSize, hash, done, progress, stop);
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        if (last && done == expectedSize) break;
        if (last) last = Fail(Text::Get(IDS_INCOMPLETE));
        if (attempt < attempts) {
            log(Text::Format(IDS_LOG_RETRY, attempt, attempts, last.error()));
            for (int i = 0; i < 20 && !stop.stop_requested(); ++i) Sleep(100 * attempt);
        }
    }
    if (!last) return last;

    const std::wstring actual = hash->Finish();
    if (actual != expectedSha256) {
        fs::remove(part, ec);  // a damaged file must not be resumed
        return Fail(Text::Format(IDS_SHA_MISMATCH, expectedSha256, actual));
    }
    return {};
}

Result<std::wstring> HashFile(const fs::path& file, const ProgressFn& progress, std::stop_token stop) {
    std::error_code ec;
    const std::uint64_t size = fs::file_size(file, ec);
    if (ec) return Fail(ErrorText(Text::Format(IDS_CTX_OPEN, file.wstring()), static_cast<unsigned long>(ec.value())));
    Sha256 hash;
    if (!hash.ok()) return Fail(Text::Get(IDS_SHA_UNAVAILABLE));
    FileHandle in(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!in) return Fail(ErrorText(Text::Format(IDS_CTX_OPEN, file.wstring()), GetLastError()));
    std::vector<char> buffer(1u << 20);
    std::uint64_t done = 0;
    for (;;) {
        if (stop.stop_requested()) return Fail(Text::Get(IDS_CANCELED));
        DWORD read = 0;
        if (!ReadFile(in.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            return Fail(ErrorText(Text::Format(IDS_CTX_READ, file.wstring()), GetLastError()));
        if (!read) break;
        hash.Update(buffer.data(), read);
        done += read;
        progress(done, size);
    }
    return hash.Finish();
}

}  // namespace WN::Net
