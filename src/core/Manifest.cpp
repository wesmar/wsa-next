// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "core/Manifest.h"
#include "core/Release.h"
#include "install/Unpack.h"
#include "net/Downloader.h"
#include "support/Json.h"
#include "support/Text.h"
#include <algorithm>
#include <cwctype>
#include <format>

namespace WN::Core {
namespace {

bool IsHex64(const std::wstring& s) {
    return s.size() == 64 && std::ranges::all_of(s, [](wchar_t c) { return std::iswxdigit(c) != 0; });
}

// A target must stay inside the install folder.
bool SafeTarget(const std::wstring& path) {
    if (path.empty() || path.find(L':') != std::wstring::npos || path.front() == L'\\' || path.front() == L'/') return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find_first_of(L"\\/", start);
        if (end == std::wstring::npos) end = path.size();
        const std::wstring_view element(path.data() + start, end - start);
        // "." and empty elements too: a disk or file target names a file (an archive may be ".").
        if (element.empty() || element == L"." || element == L".." || Install::ReservedName(element) || element.find_first_of(L"*?\"<>|") != std::wstring_view::npos) return false;
        start = end + 1;
    }
    return true;
}

}  // namespace

const Component* Manifest::Find(std::wstring_view id) const {
    const auto it = std::ranges::find_if(components, [&](const Component& c) { return c.id == id; });
    return it != components.end() ? &*it : nullptr;
}

std::wstring ResolveUrl(const std::wstring& base, const std::wstring& relative) {
    if (relative.find(L"://") != std::wstring::npos) return relative;
    std::wstring dir = base.substr(0, base.find_first_of(L"?#"));
    const std::size_t slash = dir.rfind(L'/');
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    if (!relative.empty() && relative.front() == L'/') {  // host-relative
        const std::size_t scheme = dir.find(L"://");
        const std::size_t host = scheme == std::wstring::npos ? std::wstring::npos : dir.find(L'/', scheme + 3);
        return (host == std::wstring::npos ? dir : dir.substr(0, host)) + relative;
    }
    return dir + relative;
}

Result<Manifest> ParseManifest(std::string_view utf8, const std::wstring& url) {
    auto json = Json::Parse(utf8);
    if (!json) return Fail(Text::Format(IDS_MANIFEST_INVALID, json.error()));
    const Json::Value& root = *json;
    if (!root.IsObject()) return Fail(Text::Format(IDS_MANIFEST_INVALID, L"object expected"));
    if (root[L"format"].UInt().value_or(0) != 1) return Fail(Text::Get(IDS_MANIFEST_FORMAT));

    Manifest m;
    m.source = url;
    m.name = root.StringOr(L"name", L"WSA-Next");
    auto version = root.RequireString(L"version");
    if (!version) return Fail(Text::Format(IDS_MANIFEST_INVALID, version.error()));
    m.version = std::move(*version);
    m.android = root.StringOr(L"android");
    m.variant = root.StringOr(L"variant");
    m.minWindowsBuild = static_cast<std::uint32_t>(root[L"minWindowsBuild"].UInt().value_or(0));

    for (const Json::Value& item : root[L"components"].Items()) {
        Component c;
        auto field = [&](std::wstring_view key) -> Result<std::wstring> { return item.RequireString(key); };
        auto id = field(L"id");
        auto kind = field(L"kind");
        auto target = field(L"target");
        auto link = field(L"url");
        auto sha = field(L"packedSha256");
        auto packed = item.RequireUInt(L"packedSize");
        for (const auto* r : {&id, &kind, &target, &link, &sha})
            if (!*r) return Fail(Text::Format(IDS_MANIFEST_INVALID, r->error()));
        if (!packed) return Fail(Text::Format(IDS_MANIFEST_INVALID, packed.error()));

        c.id = std::move(*id);
        if (*kind == L"disk") c.kind = ComponentKind::Disk;
        else if (*kind == L"file") c.kind = ComponentKind::File;
        else if (*kind == L"archive") c.kind = ComponentKind::Archive;
        else return Fail(Text::Format(IDS_MANIFEST_INVALID, std::format(L"unknown kind \"{}\" of \"{}\"", *kind, c.id)));
        c.target = std::move(*target);
        if (!SafeTarget(c.target) && !(c.kind == ComponentKind::Archive && c.target == L"."))
            return Fail(Text::Format(IDS_MANIFEST_INVALID, std::format(L"unsafe target \"{}\"", c.target)));
        c.url = ResolveUrl(url, *link);
        c.packedSha256 = std::move(*sha);
        std::ranges::transform(c.packedSha256, c.packedSha256.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (!IsHex64(c.packedSha256)) return Fail(Text::Format(IDS_MANIFEST_INVALID, std::format(L"bad packedSha256 of \"{}\"", c.id)));
        c.packedSize = *packed;
        c.sha256 = item.StringOr(L"sha256");
        std::ranges::transform(c.sha256, c.sha256.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (!c.sha256.empty() && !IsHex64(c.sha256)) return Fail(Text::Format(IDS_MANIFEST_INVALID, std::format(L"bad sha256 of \"{}\"", c.id)));
        c.size = item[L"size"].UInt().value_or(0);
        c.version = item.StringOr(L"version", m.version);
        c.optional = item[L"optional"].Bool().value_or(false);
        if (m.Find(c.id)) return Fail(Text::Format(IDS_MANIFEST_INVALID, std::format(L"component \"{}\" listed twice", c.id)));
        m.components.push_back(std::move(c));
    }
    if (m.components.empty()) return Fail(Text::Format(IDS_MANIFEST_INVALID, L"no components"));

    // WSA-Next Setup: an entry that is not complete is left out, the release itself stays usable.
    if (const Json::Value& app = root[L"app"]; app.IsObject()) {
        AppRelease a;
        a.version = app.StringOr(L"version");
        a.url = app.StringOr(L"url");
        a.size = app[L"size"].UInt().value_or(0);
        a.sha256 = app.StringOr(L"sha256");
        std::ranges::transform(a.sha256, a.sha256.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        a.minimum = app.StringOr(L"minimum");
        if (!a.version.empty() && !a.url.empty() && a.size && IsHex64(a.sha256)) {
            a.url = ResolveUrl(url, a.url);
            m.app = std::move(a);
        }
    }
    return m;
}

Result<Manifest> LoadManifest(const std::wstring& url, std::stop_token stop, unsigned long connectMs, unsigned long ioMs) {
    auto body = Net::FetchText(url, 1u << 20, stop, connectMs, ioMs);
    if (!body) return Fail(body.error());
    auto manifest = ParseManifest(*body, url);
    if (!manifest) return manifest;
    manifest->raw = std::move(*body);
    // The signature: absent or unreachable means an unofficial release, not a failure.
    if (auto catalog = Net::FetchText(ResolveUrl(url, Release::kCatalogAsset), 1u << 20, stop, connectMs, ioMs)) {
        manifest->catalog = std::move(*catalog);
        manifest->official = Release::Vouches(manifest->catalog, manifest->raw);
    }
    return manifest;
}

}  // namespace WN::Core
