// WSA-Next © 2026 Marek Wesołowski - wesmar
// Opening a web page from WSA-Next, which runs elevated (the window and the background host):
// the page opens with the desktop's rights, through Explorer, so the browser never runs as
// administrator. Explorer may be missing (a shell replacement): then the page opens directly.
#pragma once

namespace WN {

// Opens `url` in the user's default browser. The caller may pass on its right to bring a window
// to the front (a click it just handled) - the browser is started by Explorer, not by it.
void OpenInBrowser(const wchar_t* url);

}  // namespace WN
