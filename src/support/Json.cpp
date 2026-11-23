// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "support/Json.h"
#include <cwctype>
#include <format>

namespace WN::Json {
namespace {

const Value kNull;
const Array kNoItems;
const Object kNoMembers;

class Parser {
public:
    explicit Parser(std::wstring text) : m_s(std::move(text)) {}

    Result<Value> Document() {
        auto v = ParseValue(0);
        if (!v) return v;
        SkipSpace();
        if (m_pos != m_s.size()) return Error(L"unexpected text after the value");
        return v;
    }

private:
    static constexpr int kMaxDepth = 64;

    std::unexpected<std::wstring> Error(std::wstring_view what) const {
        // Line and column for the message (the manifest is edited by hand).
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < m_pos && i < m_s.size(); ++i) {
            if (m_s[i] == L'\n') { ++line; column = 1; }
            else ++column;
        }
        return Fail(std::format(L"JSON: {} (line {}, column {})", what, line, column));
    }

    void SkipSpace() {
        while (m_pos < m_s.size() && (m_s[m_pos] == L' ' || m_s[m_pos] == L'\t' || m_s[m_pos] == L'\n' || m_s[m_pos] == L'\r'))
            ++m_pos;
    }

    bool Consume(std::wstring_view word) {
        if (m_s.compare(m_pos, word.size(), word) != 0) return false;
        m_pos += word.size();
        return true;
    }

    Result<Value> ParseValue(int depth) {
        if (depth > kMaxDepth) return Error(L"nested too deeply");
        SkipSpace();
        if (m_pos >= m_s.size()) return Error(L"value expected");
        const wchar_t c = m_s[m_pos];
        if (c == L'{') return ParseObject(depth);
        if (c == L'[') return ParseArray(depth);
        if (c == L'"') {
            auto s = ParseString();
            if (!s) return std::unexpected(s.error());
            return Value(std::move(*s));
        }
        if (Consume(L"true")) return Value(true);
        if (Consume(L"false")) return Value(false);
        if (Consume(L"null")) return Value();
        if (c == L'-' || std::iswdigit(c)) return ParseNumber();
        return Error(L"value expected");
    }

    Result<Value> ParseNumber() {
        const std::size_t start = m_pos;
        if (m_s[m_pos] == L'-') ++m_pos;
        auto digits = [&] {
            const std::size_t from = m_pos;
            while (m_pos < m_s.size() && std::iswdigit(m_s[m_pos])) ++m_pos;
            return m_pos > from;
        };
        if (!digits()) return Error(L"digit expected");
        if (m_pos < m_s.size() && m_s[m_pos] == L'.') {
            ++m_pos;
            if (!digits()) return Error(L"digit expected");
        }
        if (m_pos < m_s.size() && (m_s[m_pos] == L'e' || m_s[m_pos] == L'E')) {
            ++m_pos;
            if (m_pos < m_s.size() && (m_s[m_pos] == L'+' || m_s[m_pos] == L'-')) ++m_pos;
            if (!digits()) return Error(L"digit expected");
        }
        return Value::Number(m_s.substr(start, m_pos - start));
    }

    Result<std::wstring> ParseString() {
        ++m_pos;  // opening quote
        std::wstring out;
        while (m_pos < m_s.size()) {
            const wchar_t c = m_s[m_pos++];
            if (c == L'"') return out;
            if (c < 0x20) return Error(L"control character in a string");
            if (c != L'\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_s.size()) break;
            const wchar_t e = m_s[m_pos++];
            switch (e) {
            case L'"': out.push_back(L'"'); break;
            case L'\\': out.push_back(L'\\'); break;
            case L'/': out.push_back(L'/'); break;
            case L'b': out.push_back(L'\b'); break;
            case L'f': out.push_back(L'\f'); break;
            case L'n': out.push_back(L'\n'); break;
            case L'r': out.push_back(L'\r'); break;
            case L't': out.push_back(L'\t'); break;
            case L'u': {
                if (m_pos + 4 > m_s.size()) return Error(L"incomplete \\u escape");
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const wchar_t h = m_s[m_pos++];
                    code <<= 4;
                    if (h >= L'0' && h <= L'9') code |= static_cast<unsigned>(h - L'0');
                    else if (h >= L'a' && h <= L'f') code |= static_cast<unsigned>(h - L'a' + 10);
                    else if (h >= L'A' && h <= L'F') code |= static_cast<unsigned>(h - L'A' + 10);
                    else return Error(L"bad \\u escape");
                }
                out.push_back(static_cast<wchar_t>(code));  // UTF-16 code unit; surrogate pairs come as two
                break;
            }
            default: return Error(L"bad escape");
            }
        }
        return Error(L"unterminated string");
    }

    Result<Value> ParseArray(int depth) {
        ++m_pos;
        Array items;
        SkipSpace();
        if (m_pos < m_s.size() && m_s[m_pos] == L']') { ++m_pos; return Value(std::move(items)); }
        for (;;) {
            auto v = ParseValue(depth + 1);
            if (!v) return v;
            items.push_back(std::move(*v));
            SkipSpace();
            if (m_pos < m_s.size() && m_s[m_pos] == L',') { ++m_pos; continue; }
            if (m_pos < m_s.size() && m_s[m_pos] == L']') { ++m_pos; return Value(std::move(items)); }
            return Error(L"',' or ']' expected");
        }
    }

    Result<Value> ParseObject(int depth) {
        ++m_pos;
        Object members;
        SkipSpace();
        if (m_pos < m_s.size() && m_s[m_pos] == L'}') { ++m_pos; return Value(std::move(members)); }
        for (;;) {
            SkipSpace();
            if (m_pos >= m_s.size() || m_s[m_pos] != L'"') return Error(L"member name expected");
            auto key = ParseString();
            if (!key) return std::unexpected(key.error());
            SkipSpace();
            if (m_pos >= m_s.size() || m_s[m_pos] != L':') return Error(L"':' expected");
            ++m_pos;
            auto v = ParseValue(depth + 1);
            if (!v) return v;
            members.insert_or_assign(std::move(*key), std::move(*v));
            SkipSpace();
            if (m_pos < m_s.size() && m_s[m_pos] == L',') { ++m_pos; continue; }
            if (m_pos < m_s.size() && m_s[m_pos] == L'}') { ++m_pos; return Value(std::move(members)); }
            return Error(L"',' or '}' expected");
        }
    }

    std::wstring m_s;
    std::size_t m_pos = 0;
};

void Escape(std::wstring& out, const std::wstring& s) {
    out.push_back(L'"');
    for (const wchar_t c : s) {
        switch (c) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (c < 0x20) out += std::format(L"\\u{:04x}", static_cast<unsigned>(c));
            else out.push_back(c);
        }
    }
    out.push_back(L'"');
}

void WriteValue(std::wstring& out, const Value& v, int indent) {
    const std::wstring pad(static_cast<std::size_t>(indent + 2), L' '), close(static_cast<std::size_t>(indent), L' ');
    switch (v.kind()) {
    case Value::Kind::Null: out += L"null"; break;
    case Value::Kind::Bool: out += *v.Bool() ? L"true" : L"false"; break;
    case Value::Kind::Number:
    case Value::Kind::String: {
        if (v.kind() == Value::Kind::String) Escape(out, *v.String());
        else out += v.UInt() ? std::to_wstring(*v.UInt()) : L"0";
        break;
    }
    case Value::Kind::Array: {
        if (v.Items().empty()) { out += L"[]"; break; }
        out += L"[\n";
        bool first = true;
        for (const Value& item : v.Items()) {
            if (!first) out += L",\n";
            first = false;
            out += pad;
            WriteValue(out, item, indent + 2);
        }
        out += L"\n" + close + L"]";
        break;
    }
    case Value::Kind::Object: {
        if (v.Members().empty()) { out += L"{}"; break; }
        out += L"{\n";
        bool first = true;
        for (const auto& [key, item] : v.Members()) {
            if (!first) out += L",\n";
            first = false;
            out += pad;
            Escape(out, key);
            out += L": ";
            WriteValue(out, item, indent + 2);
        }
        out += L"\n" + close + L"}";
        break;
    }
    }
}

}  // namespace

std::optional<std::wstring> Value::String() const {
    if (m_kind != Kind::String) return std::nullopt;
    return m_text;
}

std::optional<std::uint64_t> Value::UInt() const {
    if (m_kind != Kind::Number || m_text.empty() || m_text[0] == L'-') return std::nullopt;
    std::uint64_t n = 0;
    for (const wchar_t c : m_text) {
        if (c < L'0' || c > L'9') return std::nullopt;  // fractions and exponents are not sizes
        const std::uint64_t digit = static_cast<std::uint64_t>(c - L'0');
        if (n > (UINT64_MAX - digit) / 10) return std::nullopt;
        n = n * 10 + digit;
    }
    return n;
}

std::optional<bool> Value::Bool() const {
    if (m_kind != Kind::Bool) return std::nullopt;
    return m_bool;
}

const Array& Value::Items() const { return m_array ? *m_array : kNoItems; }
const Object& Value::Members() const { return m_object ? *m_object : kNoMembers; }

const Value& Value::operator[](std::wstring_view key) const {
    if (!m_object) return kNull;
    const auto it = m_object->find(key);
    return it != m_object->end() ? it->second : kNull;
}

Result<std::wstring> Value::RequireString(std::wstring_view key) const {
    if (auto s = (*this)[key].String()) return std::move(*s);
    return Fail(std::format(L"\"{}\" (text) is missing", key));
}

Result<std::uint64_t> Value::RequireUInt(std::wstring_view key) const {
    if (auto n = (*this)[key].UInt()) return *n;
    return Fail(std::format(L"\"{}\" (number) is missing", key));
}

std::wstring Value::StringOr(std::wstring_view key, std::wstring fallback) const {
    auto s = (*this)[key].String();
    return s ? std::move(*s) : std::move(fallback);
}

Result<Value> Parse(std::string_view utf8) {
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF && static_cast<unsigned char>(utf8[1]) == 0xBB &&
        static_cast<unsigned char>(utf8[2]) == 0xBF)
        utf8.remove_prefix(3);  // BOM written by some editors
    return Parser(FromUtf8(utf8)).Document();
}

std::string Write(const Value& value) {
    std::wstring out;
    WriteValue(out, value, 0);
    out += L"\n";
    return ToUtf8(out);
}

}  // namespace WN::Json
