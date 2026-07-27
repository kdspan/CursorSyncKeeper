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

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    HWND m_hWnd;
};
