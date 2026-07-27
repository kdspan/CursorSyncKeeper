#include "CursorFixer.h"
#include <windows.h>
#include <cstdlib>

void CursorFixer::Apply() {
    // 1) Read the user's persisted setting from the registry.
    //    HKCU\Control Panel\Mouse\MouseTrails  -> REG_SZ, e.g. "0".."7"
    int trails = 0;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Mouse", 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[16] = {0};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"MouseTrails", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS &&
            type == REG_SZ) {
            trails = static_cast<int>(wcstol(buf, nullptr, 10));
        }
        RegCloseKey(hKey);
    }

    // Clamp to the documented valid range for SPI_SETMOUSETRAILS.
    if (trails < 0) trails = 0;
    if (trails > 7) trails = 7;

    // 2) Force the kernel to re-load the mouse-trail flag.
    //    SPIF_SENDCHANGE broadcasts the change so all apps pick it up,
    //    without touching the registry (it already holds the right value).
    SystemParametersInfoW(
        SPI_SETMOUSETRAILS,
        static_cast<UINT>(trails),
        nullptr,
        SPIF_SENDCHANGE
    );
}
