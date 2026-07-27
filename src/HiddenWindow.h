#pragma once
#include <windows.h>

class HiddenWindow {
public:
    HiddenWindow();
    ~HiddenWindow();

    // Create the invisible message-only window.
    bool Create();

    // Block on the message pump until WM_QUIT.
    void RunMessageLoop();

    // Tear down the window.
    void Destroy();

    // Record that a fix just ran. Any display-change trigger arriving within
    // COOLDOWN_MS afterwards is suppressed, because it is the aftershock of our
    // own ResetGraphicsDriver() (which broadcasts WM_DISPLAYCHANGE), not a
    // genuine external display change. Without this the fix re-triggers itself
    // forever (screen flashes in a loop until the process is killed).
    void MarkApplied();

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hWnd = nullptr;

    // Tick of the last fix. Triggers within COOLDOWN_MS after it are ignored
    // (they are the aftermath of our own driver reset).
    DWORD m_lastApplyTick = 0;

    static constexpr DWORD COOLDOWN_MS = 4000;
};
