// WSA-Next © 2026 Marek Wesołowski - wesmar
#include <Windows.h>
#include <ShlObj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shellapi.h>
#include <wrl/client.h>
#include "support/Browser.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

namespace WN {
namespace {
using Microsoft::WRL::ComPtr;

// The shell's own ShellExecute, reached through the desktop window's view: Explorer starts the
// program, with its rights.
bool OpenThroughExplorer(const wchar_t* url) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool opened = false;
    {
        ComPtr<IShellWindows> windows;
        ComPtr<IDispatch> desktop;
        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IDispatch> background;
        ComPtr<IShellFolderViewDual> folderView;
        ComPtr<IDispatch> application;
        ComPtr<IShellDispatch2> shell;
        VARIANT empty{};
        long hwnd = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.GetAddressOf()))) &&
            windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, desktop.GetAddressOf()) == S_OK && desktop &&
            SUCCEEDED(desktop.As(&provider)) &&
            SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.GetAddressOf()))) &&
            SUCCEEDED(browser->QueryActiveShellView(view.GetAddressOf())) &&
            SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(background.GetAddressOf()))) &&
            SUCCEEDED(background.As(&folderView)) && SUCCEEDED(folderView->get_Application(application.GetAddressOf())) &&
            SUCCEEDED(application.As(&shell))) {
            BSTR file = SysAllocString(url);
            VARIANT none{}, show{};
            show.vt = VT_I4;
            show.lVal = SW_SHOWNORMAL;
#pragma push_macro("ShellExecute")
#undef ShellExecute  // the COM method, not shellapi.h's ShellExecuteW
            opened = file && SUCCEEDED(shell->ShellExecute(file, none, none, none, show));
#pragma pop_macro("ShellExecute")
            SysFreeString(file);
        }
    }  // the interfaces are released before COM is
    if (SUCCEEDED(com)) CoUninitialize();
    return opened;
}

}  // namespace

void OpenInBrowser(const wchar_t* url) {
    AllowSetForegroundWindow(ASFW_ANY);  // Explorer's browser window may come to the front
    if (!OpenThroughExplorer(url)) ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace WN
