#include "HiddenWindow.h"
#include "CursorFixer.h"
#include <windows.h>
#include <dbt.h>      // DEV_BROADCAST_DEVICEINTERFACE_*, DBT_*
#include <shellapi.h> // SHQueryUserNotificationState
#include <cstring>    // memcmp (GUID equality)
#include <string>
#include <vector>
#include <algorithm>

// Single-shot timer: replace any blocking Sleep() with an event-driven delay.
#define TIMER_ID_FIX      1
#define FIX_DELAY_MS      500   // wait for the display driver to finish resetting

// Sentinel-verification timers (borderless/windowed games -- no display-mode
// change, so the topology path never fires for them).
#define TIMER_ID_VERIFY   2     // one-shot, armed by foreground / setting events
#define VERIFY_DELAY_MS   1200  // let the game finish its own cursor teardown
#define TIMER_ID_WATCHDOG 3     // periodic last-resort check (one cheap syscall)

// Watchdog period adapts to foreground state. While a likely-fullscreen game
// owns the foreground we poll FAST (a game can reset MouseTrails within a
// frame, so 3 s is the longest we tolerate being on the hardware path); on the
// normal desktop the 15 s tick is plenty and costs essentially nothing.
#define WATCHDOG_MS_NORMAL 15000
#define WATCHDOG_MS_FAST   3000

// Window handle the out-of-context WinEvent hook posts to.
#define WM_APP_FOREGROUND (WM_APP + 1)
static HWND g_hookWnd = nullptr;

// Device-interface GUIDs for display-class devices. We listen only to these so
// a USB-C monitor / docking-station display / USB GPU triggers a re-apply,
// while unrelated USB plugs (flash drive, mouse, keyboard) do not.
static const GUID kMonitorInterfaceGuid = {
    0xe6f07b5f, 0xee97, 0x4a90,
    {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};  // GUID_DEVINTERFACE_MONITOR
static const GUID kDisplayAdapterGuid = {
    0x5b45201d, 0xf2f2, 0x4f3b,
    {0x85, 0xbb, 0x30, 0xff, 0x1f, 0x95, 0x33, 0x99}};  // GUID_DEVINTERFACE_DISPLAY_ADAPTER

// Serialize the FULL current display topology (device name + geometry +
// primary flag) into a stable, sorted, order-independent string.
//
// Why full topology and not name-only: a game exiting exclusive fullscreen
// changes the RESOLUTION of an existing monitor -- the device-name set stays
// identical, so a name-only compare drops exactly the event this tool exists
// for. Geometry must be part of the snapshot.
//
// Why this is still immune to mobile-HDD jitter: the comparison is DEFERRED
// to WM_TIMER, FIX_DELAY_MS after the last broadcast. HDD-induced GPU jitter
// is transient -- by the time the timer fires the topology has returned to
// the snapshot value, compares equal, and is dropped (zero flash). A game
// exit or monitor add/remove is a PERSISTENT change and compares different.
static std::wstring CaptureTopology() {
    std::vector<std::wstring> parts;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM l) -> BOOL {
            auto* p = reinterpret_cast<std::vector<std::wstring>*>(l);
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi)) {
                wchar_t buf[256];
                swprintf_s(buf, L"%s[%ld,%ld,%ld,%ld,%lu]",
                           mi.szDevice,
                           mi.rcMonitor.left,  mi.rcMonitor.top,
                           mi.rcMonitor.right, mi.rcMonitor.bottom,
                           static_cast<unsigned long>(mi.dwFlags & MONITORINFOF_PRIMARY));
                p->push_back(buf);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&parts));
    std::sort(parts.begin(), parts.end());
    std::wstring out;
    for (const auto& s : parts) out += s + L";";
    return out;
}

HiddenWindow::HiddenWindow() : m_hWnd(nullptr) {}
HiddenWindow::~HiddenWindow() { Destroy(); }

bool HiddenWindow::Create() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = L"CursorSyncKeeperClass";

    if (!RegisterClassW(&wc)) return false;

    // WS_POPUP + 1x1 at (0,0), never shown -> not in Alt+Tab, not visible.
    // It still receives broadcast messages (WM_DISPLAYCHANGE, etc.).
    m_hWnd = CreateWindowExW(
        0,
        L"CursorSyncKeeperClass",
        L"CursorSyncKeeper",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (m_hWnd) {
        // Associate this instance so WndProc can reach the debounce state.
        SetWindowLongPtrW(m_hWnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        m_lastApplyTick = GetTickCount();

        // Register for display-interface device notifications. Arrival/removal
        // of a *display* device (e.g. plugging a USB-C monitor or a docking
        // station's video output) changes the display topology and flips the
        // system back to the hardware cursor, so it must re-trigger a fix.
        DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

        filter.dbcc_classguid = kMonitorInterfaceGuid;
        m_hDevNotify = RegisterDeviceNotificationW(
            m_hWnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

        filter.dbcc_classguid = kDisplayAdapterGuid;
        m_hDevNotifyAdapter = RegisterDeviceNotificationW(
            m_hWnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        // Registration failure is non-fatal: WM_DISPLAYCHANGE still covers the
        // common hot-plug case, so we do not abort startup on failure.
    }

    if (m_hWnd) {
        // Foreground-switch hook: the trigger for BORDERLESS games, which
        // reset the mouse setting without ever changing the display mode.
        // WINEVENT_OUTOFCONTEXT -> callback runs in OUR thread via the message
        // loop; no DLL injection into other processes.
        g_hookWnd = m_hWnd;
        m_hWinEventHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

        // Last-resort watchdog: one cheap SPI query per tick, catches games
        // that reset the cursor silently (no broadcast, no foreground change).
        // Starts on the NORMAL period; it self-rebases to FAST when a game is
        // detected (foreground switch or first tick).
        SetTimer(m_hWnd, TIMER_ID_WATCHDOG, WATCHDOG_MS_NORMAL, nullptr);
    }

    // Snapshot the current display topology so the first real change (not a
    // USB-storage jitter) is what triggers the first fix.
    m_lastTopology = CaptureTopology();

    return (m_hWnd != nullptr);
}

void HiddenWindow::RunMessageLoop() {
    MSG msg;
    // 99.9% of the time the thread is suspended inside GetMessage -> 0% CPU.
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void HiddenWindow::Destroy() {
    if (m_hWinEventHook) {
        UnhookWinEvent(m_hWinEventHook);
        m_hWinEventHook = nullptr;
        g_hookWnd = nullptr;
    }
    if (m_hWnd) KillTimer(m_hWnd, TIMER_ID_WATCHDOG);
    if (m_hDevNotifyAdapter) {
        UnregisterDeviceNotification(m_hDevNotifyAdapter);
        m_hDevNotifyAdapter = nullptr;
    }
    if (m_hDevNotify) {
        UnregisterDeviceNotification(m_hDevNotify);
        m_hDevNotify = nullptr;
    }
    if (m_hWnd) { DestroyWindow(m_hWnd); m_hWnd = nullptr; }
}

bool HiddenWindow::CooldownElapsed() const {
    // Ignore anything within COOLDOWN_MS of the last fix (the aftershock of
    // our own ResetGraphicsDriver(), belt-and-suspenders).
    DWORD now = GetTickCount();
    return static_cast<LONG>(now - m_lastApplyTick) >=
           static_cast<LONG>(COOLDOWN_MS);
}

bool HiddenWindow::ShouldApplyNow() {
    // Called from the timer, AFTER the FIX_DELAY_MS settle window. Any GPU
    // jitter caused by a mobile-HDD plug/unplug has died down by now, so this
    // capture reflects the TRUE topology -- not a transient phantom state.
    std::wstring cur = CaptureTopology();
    if (cur == m_lastTopology)
        return false;   // topology unchanged (HDD jitter settled) -> no flash

    // Persistent change (game exit resolution switch / monitor add/remove):
    // snapshot the new topology so a second broadcast of this same physical
    // event compares equal and is dropped.
    m_lastTopology = std::move(cur);
    return true;
}

// Heuristic for "a borderless / fullscreen GAME likely owns the foreground".
// SHQueryUserNotificationState does NOT flag borderless-fullscreen games (they
// are just a normal maximized-borderless window), so we detect them ourselves:
// a visible, non-toolwindow top-level window whose rect covers the ENTIRE
// primary monitor and carries no caption / sizing border (WS_POPUP) is the
// classic borderless-game signature. Exclusive-fullscreen D3D apps are caught
// too (they also cover the screen), which is fine -- we WANT the fast poll for
// them as well.
static bool IsLikelyFullscreenForegroundWindow() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg))
        return false;
    if (GetWindowLongPtrW(fg, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return false;   // taskbar-like / overlay windows are not games

    HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi))
        return false;

    RECT wr;
    GetWindowRect(fg, &wr);
    // Covers the full primary monitor...
    if (wr.left   <= mi.rcMonitor.left  && wr.top    <= mi.rcMonitor.top &&
        wr.right  >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom) {
        // ...and has no title bar / border -> borderless-game signature.
        const LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
        if ((style & (WS_CAPTION | WS_THICKFRAME)) == 0)
            return true;
    }
    return false;
}

void HiddenWindow::VerifyCursorSentinel() {
    // (1) Keep OverlayTestMode (MPO off) alive without a driver reset. This is
    //     the driver-level software-cursor guarantee; rewriting the registry is
    //     cheap and never flashes the screen.
    CursorFixer::EnsureOverlayTestModeAlive();

    // (2) Runtime sentinel intact -> nothing was hijacked, zero work.
    if (CursorFixer::TrailsSentinelActive())
        return;

    // (3) MouseTrails was reset to the hardware path -- by a borderless,
    //     windowed OR still-running fullscreen game. Re-assert the -1 sentinel
    //     cheaply (no GPU reset -> NO screen flash). We deliberately NO LONGER
    //     back off while a game is foreground: that back-off was the very reason
    //     the secondary monitor lost the software cursor during gaming. Both
    //     OverlayTestMode and MouseTrails=-1 are invisible, so there is no
    //     visual artifact and no tug-of-war a game can "win" for long -- the
    //     watchdog re-asserts within WATCHDOG_MS_FAST seconds.
    CursorFixer::ReassertSoftwareCursor();
}

void CALLBACK HiddenWindow::WinEventProc(HWINEVENTHOOK, DWORD event, HWND,
                                         LONG, LONG, DWORD, DWORD) {
    // Out-of-context: this runs on our own thread inside the message loop.
    // Defer via the coalescing one-shot timer instead of acting inline, so a
    // burst of foreground flips (alt-tab storm) costs one verification.
    if (event == EVENT_SYSTEM_FOREGROUND && g_hookWnd) {
        // Only a fullscreen / borderless GAME gaining the foreground is a real
        // risk of having reset the mouse path. Ordinary foreground switches
        // (Telegram, browser, explorer at logon, ...) never touch MouseTrails,
        // so we deliberately do NOT schedule a repair for them -- otherwise the
        // daemon would "repair" on every app switch while the cursor was never
        // actually broken (and a benign app like Telegram auto-starting at boot
        // would spuriously appear to trigger a fix). The watchdog (15 s) still
        // catches any genuine drift.
        const bool game = IsLikelyFullscreenForegroundWindow();
        if (game)
            SetTimer(g_hookWnd, TIMER_ID_VERIFY, VERIFY_DELAY_MS, nullptr);
        // Keep the watchdog adaptive either way: FAST while a game owns the
        // foreground, NORMAL otherwise (so we return to the slow poll the
        // instant the game exits).
        KillTimer(g_hookWnd, TIMER_ID_WATCHDOG);
        SetTimer(g_hookWnd, TIMER_ID_WATCHDOG,
                 game ? WATCHDOG_MS_FAST : WATCHDOG_MS_NORMAL, nullptr);
    }
}

// ===================== Pure event-driven, no polling =====================
LRESULT CALLBACK HiddenWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HiddenWindow* self =
        reinterpret_cast<HiddenWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
        // --- Triggers: anything that can reset the display/GPU context ---
        case WM_DISPLAYCHANGE:    // resolution / orientation / monitor hot-plug (game exit)
        case WM_POWERBROADCAST:   // wake from sleep (PBT_APMRESUME*)
            // At event time we only check the cooldown and (re)arm the settle
            // timer. NO topology decision is made here -- the system may be
            // mid-jitter (mobile-HDD plug). Re-arming on every broadcast means
            // a burst of triggers coalesces into ONE deferred judgement, made
            // in WM_TIMER after the jitter has settled.
            if (self && self->CooldownElapsed())
                SetTimer(hWnd, TIMER_ID_FIX, FIX_DELAY_MS, nullptr);
            return 0;

        case WM_DEVICECHANGE:     // display-class USB device (monitor / dock / USB GPU)
            // We registered device notifications for the monitor and display-
            // adapter interfaces, so lParam now carries the concrete arrival /
            // removal of a *display* device. Only those are even considered; a
            // flash drive / mouse / keyboard plug is ignored here. (If
            // registration failed, wParam is the generic DBT_DEVNODES_CHANGED
            // with lParam == NULL, which we also ignore.)
            if (self && (wParam == DBT_DEVICEARRIVAL ||
                         wParam == DBT_DEVICEREMOVECOMPLETE) && lParam) {
                auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
                if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                    auto* di =
                        reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(lParam);
                    if (std::memcmp(&di->dbcc_classguid, &kMonitorInterfaceGuid,
                                    sizeof(GUID)) == 0 ||
                        std::memcmp(&di->dbcc_classguid, &kDisplayAdapterGuid,
                                    sizeof(GUID)) == 0) {
                        // Same deferred pattern: just (re)arm the settle timer;
                        // the monitor-set comparison happens in WM_TIMER.
                        if (self->CooldownElapsed())
                            SetTimer(hWnd, TIMER_ID_FIX, FIX_DELAY_MS, nullptr);
                    }
                }
            }
            return 0;

        case WM_SETTINGCHANGE:
            // A game (or anything) called SystemParametersInfo with
            // SPIF_SENDCHANGE -- possibly resetting MouseTrails. Coalesce into
            // one deferred sentinel check. Our own re-assert also broadcasts
            // this, but then the sentinel is intact and the check is a no-op,
            // so there is no loop.
            if (self)
                SetTimer(hWnd, TIMER_ID_VERIFY, VERIFY_DELAY_MS, nullptr);
            return 0;

        case WM_APP_FOREGROUND:
            // (Reserved path if the hook is ever switched to PostMessage.)
            if (self)
                SetTimer(hWnd, TIMER_ID_VERIFY, VERIFY_DELAY_MS, nullptr);
            return 0;

        case WM_QUERYENDSESSION:
        case WM_ENDSESSION:
            // Last chance to force MouseTrails back to -1 (no driver reset, no
            // flash). Windows saves per-user settings (HKCU\...\MouseTrails) when
            // it tears down the session, so if the runtime value happened to be 0
            // at that instant -- a process reset it in the final seconds, or the
            // daemon was not running -- Windows would persist 0 and the mouse
            // would boot onto the hardware path. Re-asserting here guarantees the
            // software-cursor sentinel is what gets saved. Harmless if the
            // shutdown is later cancelled (mouse simply stays on the software
            // path).
            if (self) CursorFixer::ReassertSoftwareCursor();
            return DefWindowProcW(hWnd, msg, wParam, lParam);

        case WM_TIMER:
            if (wParam == TIMER_ID_FIX) {
                KillTimer(hWnd, TIMER_ID_FIX);
                // Jitter has settled: NOW decide. If the monitor device-name
                // set is unchanged (mobile HDD / flash drive / mouse), this
                // returns false and we do nothing -- zero screen flash.
                if (self && self->ShouldApplyNow()) {
                    // Re-assert the user's mouse-trail setting after the reset.
                    CursorFixer::Apply();
                    // Refresh the cooldown so the WM_DISPLAYCHANGE aftershock
                    // that Apply() just caused is suppressed (breaks the loop).
                    self->m_lastApplyTick = GetTickCount();
                }
            } else if (wParam == TIMER_ID_VERIFY) {
                // One-shot: armed by foreground switches / setting broadcasts.
                KillTimer(hWnd, TIMER_ID_VERIFY);
                VerifyCursorSentinel();
            } else if (wParam == TIMER_ID_WATCHDOG) {
                // Periodic sentinel check (silent cursor hijack with no events).
                // Cheap: one SPI query + a guarded registry read when idle.
                VerifyCursorSentinel();
                // Re-base the period so it tracks the foreground state: FAST
                // while a game is up, NORMAL on the desktop. Self-rescheduling
                // via KillTimer+SetTimer avoids drift.
                KillTimer(hWnd, TIMER_ID_WATCHDOG);
                SetTimer(hWnd, TIMER_ID_WATCHDOG,
                         IsLikelyFullscreenForegroundWindow() ? WATCHDOG_MS_FAST
                                                              : WATCHDOG_MS_NORMAL,
                         nullptr);
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void HiddenWindow::MarkApplied() {
    m_lastApplyTick = GetTickCount();
}
