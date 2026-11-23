// WSA-Next © 2026 Marek Wesołowski - wesmar
// WSA-Next - entry point. No arguments: graphical interface. Any argument: command line.
#include "app/Cli.h"
#include "app/SetupActions.h"
#include "host/Host.h"
#include "gui/MainWindow.h"
#include "support/Common.h"
#include "support/Config.h"
#include "support/Text.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <memory>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

struct ArgvDeleter {
    void operator()(wchar_t** argv) const noexcept { LocalFree(argv); }
};

class ComApartment {
public:
    ComApartment() noexcept : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ~ComApartment() { if (SUCCEEDED(m_hr)) CoUninitialize(); }

private:
    HRESULT m_hr;
};

// The GUI needs administrator rights (Program Files, developer mode, Windows features); the manifest
// stays asInvoker so the command line can report a missing elevation instead of a UAC prompt in a script.
int RelaunchElevated() {
    const std::wstring self = WN::ModulePath();
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = self.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return 0;
    if (GetLastError() != ERROR_CANCELLED)
        MessageBoxW(nullptr, WN::ErrorText(WN::Text::Get(IDS_ERR_ELEVATE), GetLastError()).c_str(), WN::Config::kAppTitle,
                    MB_OK | MB_ICONERROR);
    return 1;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int showCommand) {
    // Hardening: DLLs only from System32 (no planting next to the executable or in the working directory).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    // No loader message boxes, here or in the processes started from here (inherited): a failure is
    // reported through the exit code and the log.
    SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    int argc = 0;
    const std::unique_ptr<wchar_t*, ArgvDeleter> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    // The background host (Windows Hello and Bluetooth for Android; started by the window or at
    // logon): no console, no window. "bluetooth --background" is its name of the first test builds.
    if (argv && argc == 3 && _wcsicmp(argv.get()[2], L"--background") == 0 &&
        (_wcsicmp(argv.get()[1], L"host") == 0 || _wcsicmp(argv.get()[1], L"bluetooth") == 0)) {
        WN::Text::UseSaved();  // its icon and the Windows Hello dialogs speak the user's language
        return WN::Host::RunBackground();
    }
    // WSA-Next Setup's custom actions (LocalSystem, session 0): no console either.
    if (argv && argc > 2 && _wcsicmp(argv.get()[1], L"msi") == 0) return WN::SetupActions::Run(argc, argv.get());
    if (argv && argc > 1) return WN::Cli::Run(argc, argv.get());

    WN::Text::UseSaved();  // Windows language unless the user picked one
    if (!WN::IsElevated()) return RelaunchElevated();

    const ComApartment com;  // file dialogs, taskbar progress
    const INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WN::Ui::MainWindow window(instance);
    if (!window.Create(showCommand)) {
        MessageBoxW(nullptr, WN::Text::Get(IDS_ERR_WINDOW).c_str(), WN::Config::kAppTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    return window.Run();
}
