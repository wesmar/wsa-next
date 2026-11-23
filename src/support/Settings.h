// WSA-Next © 2026 Marek Wesołowski - wesmar
// Per-user settings in HKCU\Software\KVC\WSA-Next (shared by the GUI and the command line).
#pragma once
#include <Windows.h>
#include <string>
#include <string_view>

namespace WN::Settings {

std::wstring Read(const wchar_t* name, std::wstring_view fallback = {});
void Write(const wchar_t* name, const std::wstring& value);
DWORD ReadDword(const wchar_t* name, DWORD fallback);
void WriteDword(const wchar_t* name, DWORD value);
bool Exists(const wchar_t* name);

}  // namespace WN::Settings
