// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "install/Setup.h"
#include "support/Config.h"
#include "support/Text.h"
#include <Windows.h>
#include <Msi.h>
#include <format>

#pragma comment(lib, "msi.lib")

namespace WN::Install {

std::optional<std::wstring> SetupProduct() {
    wchar_t product[39]{};  // a GUID in braces
    for (DWORD i = 0; MsiEnumRelatedProductsW(Config::kSetupUpgradeCode, 0, i, product) == ERROR_SUCCESS; ++i) {
        const INSTALLSTATE state = MsiQueryProductStateW(product);
        if (state == INSTALLSTATE_DEFAULT || state == INSTALLSTATE_LOCAL) return std::wstring(product);
    }
    return std::nullopt;
}

Result<> RemoveThroughSetup(const std::wstring& product, bool everything) {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring msiexec = std::format(L"{}\\msiexec.exe", system);
    std::wstring command = std::format(L"\"{}\" /x {} /passive{}", msiexec, product, everything ? L" REMOVEDATA=1" : L"");
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(msiexec.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, system, &si, &pi))
        return Fail(ErrorText(Text::Get(IDS_CTX_SETUP), GetLastError()));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

}  // namespace WN::Install
