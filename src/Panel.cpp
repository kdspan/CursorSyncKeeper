#include <windows.h>
#include <string>
#include "resource.h"
#include "AdminOps.h"
#include "CursorFixer.h"

// ===========================================================================
// CursorSyncKeeperPanel  --  the CONTROL PANEL program
//
//   A small GUI that lets the user:
//     • 单次修复  - apply the software-mouse fix exactly once (one screen flash)
//     • 卸载      - remove the daemon, scheduled task, registry value and files
//
//   UNINSTALL deliberately does NOT run the fix (no GPU-driver reset, no black
//   screen). It only tears the software down. Every privileged teardown action
//   (stop daemon / remove task / remove registry / remove files) is provided by
//   the shared AdminOps module -- the lightweight daemon contains none of it.
//
//   Carries a requireAdministrator manifest because it deletes Program Files,
//   removes the HKLM value and deletes the scheduled task.
// ===========================================================================

// ---- operations ----------------------------------------------------------

// Single, explicit fix. Shows one screen flash (GPU driver reset) -- that is
// the point of the "fix now" button, so it is expected here.
static void DoSingleFix() {
    CursorFixer::Apply();
}

// Uninstall. NOTE: intentionally does NOT call CursorFixer::Apply(), so the GPU
// driver is never reset and the screen never goes black during uninstall.
static std::wstring DoUninstall() {
    AdminOps::StopDaemon();                          // kill the running daemon
    AdminOps::RemoveScheduledTask();                 // drop the logon auto-start
    CursorFixer::DisableSoftwareMouseRegistry();     // remove HKLM OverlayTestMode
    CursorFixer::RestoreMouseTrails();               // MouseTrails -1 -> 0
    const std::wstring dir = AdminOps::ReadInstallLocation();
    if (!dir.empty()) AdminOps::RemoveFiles(dir);    // delete the install directory
    AdminOps::RemoveARP();                           // remove the Programs entry
    AdminOps::RemoveStartMenuShortcuts();            // remove Start Menu folder
    AdminOps::RunWait(L"cmd /c rmdir /s /q \"" + AdminOps::LogDir() + L"\"");
    return L"卸载完成。\n"
           L"• 已停止守护进程\n"
           L"• 已移除登录自启计划任务\n"
           L"• 已移除注册表项 (OverlayTestMode)\n"
           L"• 已还原鼠标轨迹设置 (MouseTrails=0)\n"
           L"• 已删除开始菜单程序项\n"
           L"• 已删除程序文件与日志\n\n"
           L"（卸载过程不执行修复，因此不会出现屏幕黑闪。）";
}

// ---- dialog --------------------------------------------------------------

static INT_PTR CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Show the embedded program icon in the title bar / taskbar.
            if (HICON hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr),
                    MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0,
                    LR_DEFAULTSIZE | LR_SHARED)) {
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
                SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
            }
            SetWindowTextW(hwnd, L"CursorSyncKeeper 控制面板");
            SetDlgItemTextW(hwnd, IDC_P_TITLE, L"CursorSyncKeeper 控制面板");
            // Real (Chinese) button labels -- the .rc only holds English
            // placeholders because rc.exe is ANSI-only.
            SetDlgItemTextW(hwnd, IDC_P_FIX,       L"单次修复");
            SetDlgItemTextW(hwnd, IDC_P_UNINSTALL, L"卸载");
            SetDlgItemTextW(hwnd, IDC_P_EXIT,      L"退出");

            if (AdminOps::IsInstalled()) {
                const std::wstring dir = AdminOps::ReadInstallLocation();
                SetDlgItemTextW(hwnd, IDC_P_INFO,
                    (L"当前状态：已安装\n安装位置：" + (dir.empty() ? L"(未知)" : dir) +
                     L"\n守护进程：后台常驻 (事件驱动, CPU≈0%)\n\n"
                     L"可执行【单次修复】或【卸载】。").c_str());
                EnableWindow(GetDlgItem(hwnd, IDC_P_UNINSTALL), TRUE);
            } else {
                SetDlgItemTextW(hwnd, IDC_P_INFO,
                    L"当前状态：未安装。\n\n请先运行 CursorSyncKeeper_Setup.exe "
                    L"安装向导进行安装，\n之后再使用本控制面板进行修复或卸载。");
                // Fix is still allowed (it is a one-shot apply); uninstall is not.
                EnableWindow(GetDlgItem(hwnd, IDC_P_UNINSTALL), FALSE);
            }
            SetDlgItemTextW(hwnd, IDC_P_STATUS, L"");
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_P_FIX:
                    DoSingleFix();
                    SetDlgItemTextW(hwnd, IDC_P_STATUS,
                        L"已执行一次软件鼠标修复。\n"
                        L"（含约 1 秒屏幕黑闪，为显卡驱动重置的正常表现；"
                        L"若问题依旧可再次点击。）");
                    return TRUE;

                case IDC_P_UNINSTALL: {
                    if (!AdminOps::IsInstalled()) {
                        SetDlgItemTextW(hwnd, IDC_P_STATUS, L"尚未安装，无法卸载。");
                        return TRUE;
                    }
                    const int r = MessageBoxW(hwnd,
                        L"确定要卸载 CursorSyncKeeper 吗？\n\n"
                        L"该操作会停止守护进程、移除计划任务与注册表项，\n"
                        L"并删除程序文件（不会执行修复，不黑屏）。",
                        L"确认卸载", MB_YESNO | MB_ICONQUESTION);
                    if (r != IDYES) return TRUE;

                    const std::wstring res = DoUninstall();
                    SetDlgItemTextW(hwnd, IDC_P_STATUS, res.c_str());
                    EnableWindow(GetDlgItem(hwnd, IDC_P_FIX), FALSE);
                    EnableWindow(GetDlgItem(hwnd, IDC_P_UNINSTALL), FALSE);
                    return TRUE;
                }

                case IDC_P_EXIT:
                    EndDialog(hwnd, 0);
                    return TRUE;
            }
            return FALSE;

        case WM_CLOSE:
            EndDialog(hwnd, 0);
            return TRUE;
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    std::wstring args;
    if (lpCmdLine) {
        int len = MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, nullptr, 0);
        if (len > 0) {
            args.resize(len - 1);
            MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, &args[0], len);
        }
    }

    // Needs admin to delete Program Files / write HKLM. Re-launch elevated if the
    // embedded manifest did not already elevate us.
    if (!AdminOps::IsElevated()) {
        AdminOps::RelaunchElevated(args);
        return 0;
    }

    // Silent uninstall path (used by "Programs and Features" quiet uninstall).
    if (args.find(L"uninstall") != std::wstring::npos) {
        const std::wstring res = DoUninstall();
        MessageBoxW(nullptr, res.c_str(), L"CursorSyncKeeper",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_PANEL), nullptr, PanelProc, 0);
    return 0;
}
