#include "HiddenWindow.h"
#include "CursorFixer.h"
#include <windows.h>
#include <dbt.h>   // DEV_BROADCAST_DEVICEINTERFACE_*, DBT_*
#include <cstring> // memcmp (GUID equality)
#include <string>
#include <vector>
#include <algorithm>

// Single-shot timer: replace any blocking Sleep() with an event-driven delay.
#define TIMER_ID_FIX   1
#define FIX_DELAY_MS   500   // wait for the display driver to finish resetting

// Device-interface GUIDs for display-class devices. We listen only to these so
// a USB-C monitor / docking-station display / USB GPU triggers a re-apply,
// while unrelated USB plugs (flash drive, mouse, keyboard) do not.
static const GUID kMonitorInterfaceGuid = {
    0xe6f07b5f, 0xee97, 0x4a90,
    {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};  // GUID_DEVINTERFACE_MONITOR
static const GUID kDisplayAdapterGuid = {
    0x5b45201d, 0xf2f2, 0x4f3b,
    {0x85, 0xbb, 0x30, 0xff, 0x1f, 0x95, 0x33, 0x99}};  // GUID_DEVINTERFACE_DISPLAY_ADAPTER

// Serialize the current monitor DEVICE-NAME set into a stable string (sorted,
// order-independent). Deliberately name-only: geometry / primary-flag jitter
// briefly while the GPU driver re-evaluates outputs during an unrelated USB
// plug/unplug, but the set of device names only changes when a monitor is
// REALLY added or removed -- which is exactly the event we care about.
static std::wstring CaptureTopology() {
    std::vector<std::wstring> parts;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM l) -> BOOL {
            auto* p = reinterpret_cast<std::vector<std::wstring>*>(l);
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi))
                p->push_back(mi.szDevice);
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
    // capture reflects the TRUE monitor set -- not a transient phantom state.
    // (The old design compared at event time, mid-jitter, which is why an HDD
    // insert flashed once and a removal flashed twice.)
    std::wstring cur = CaptureTopology();
    if (cur == m_lastTopology)
        return false;   // monitor set unchanged -> no fix, zero flash

    // A monitor was really added/removed: snapshot the new set so a second
    // broadcast of this same physical event compares equal and is dropped.
    m_lastTopology = std::move(cur);
    return true;
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
