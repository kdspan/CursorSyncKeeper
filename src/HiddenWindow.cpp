#include "HiddenWindow.h"
#include "CursorFixer.h"
#include <windows.h>

// Single-shot timer: replace any blocking Sleep() with an event-driven delay.
#define TIMER_ID_FIX   1
#define FIX_DELAY_MS   500   // wait for the display driver to finish resetting

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
    if (m_hWnd) { DestroyWindow(m_hWnd); m_hWnd = nullptr; }
}

// ===================== Pure event-driven, no polling =====================
LRESULT CALLBACK HiddenWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        // --- Triggers: anything that can reset the display/GPU context ---
        case WM_DISPLAYCHANGE:    // resolution / orientation / monitor hot-plug (game exit)
        case WM_DEVICECHANGE:     // GPU / display driver reset
        case WM_POWERBROADCAST:   // wake from sleep (PBT_APMRESUME*)
            // The driver is still resetting the context right now.
            // Schedule a single delayed fix instead of blocking the pump.
            SetTimer(hWnd, TIMER_ID_FIX, FIX_DELAY_MS, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_ID_FIX) {
                KillTimer(hWnd, TIMER_ID_FIX);
                // Re-assert the user's mouse-trail setting after the reset.
                CursorFixer::Apply();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
