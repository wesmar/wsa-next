// WSA-Next © 2026 Marek Wesołowski - wesmar
// Minimal JSON reader and writer for the release manifest and the install state (RFC 8259,
// UTF-8). Numbers keep their text, so 64-bit sizes are read exactly.
#pragma once
#include "support/Common.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace WN::Json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::wstring, Value, std::less<>>;

class Value {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(bool b) : m_kind(Kind::Bool), m_bool(b) {}
    Value(std::wstring s) : m_kind(Kind::String), m_text(std::move(s)) {}
    Value(const wchar_t* s) : Value(std::wstring(s)) {}
    Value(std::uint64_t n) : m_kind(Kind::Number), m_text(std::to_wstring(n)) {}
    Value(Array a) : m_kind(Kind::Array), m_array(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : m_kind(Kind::Object), m_object(std::make_shared<Object>(std::move(o))) {}
    static Value Number(std::wstring text) {
        Value v;
        v.m_kind = Kind::Number;
        v.m_text = std::move(text);
        return v;
    }

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] bool IsObject() const noexcept { return m_kind == Kind::Object; }
    [[nodiscard]] bool IsArray() const noexcept { return m_kind == Kind::Array; }

    // Typed access: nullopt when the value has another type.
    [[nodiscard]] std::optional<std::wstring> String() const;
    [[nodiscard]] std::optional<std::uint64_t> UInt() const;
    [[nodiscard]] std::optional<bool> Bool() const;
    [[nodiscard]] const Array& Items() const;   // empty unless an array
    [[nodiscard]] const Object& Members() const;  // empty unless an object
    // Object member, or a null value.
    [[nodiscard]] const Value& operator[](std::wstring_view key) const;

    // Helpers for required members (the message names the missing key).
    [[nodiscard]] Result<std::wstring> RequireString(std::wstring_view key) const;
    [[nodiscard]] Result<std::uint64_t> RequireUInt(std::wstring_view key) const;
    [[nodiscard]] std::wstring StringOr(std::wstring_view key, std::wstring fallback = {}) const;

private:
    Kind m_kind = Kind::Null;
    bool m_bool = false;
    std::wstring m_text;  // string value or number text
    std::shared_ptr<Array> m_array;
    std::shared_ptr<Object> m_object;
};

// Parses UTF-8 JSON text.
Result<Value> Parse(std::string_view utf8);
// Pretty-printed UTF-8 JSON (two-space indent, members in key order).
std::string Write(const Value& value);

}  // namespace WN::Json
