#pragma once

// ---- Shared application icon (embedded into all three programs) ----
#define IDI_APP_ICON     100

// ---- Self-extracting payloads (embedded inside CursorSyncKeeper_Setup.exe) ----
// So a single Setup.exe is enough to install everything; it does NOT need the
// daemon / control panel files to be present alongside it.
#define IDR_DAEMON_BIN   400
#define IDR_PANEL_BIN    401

// ---- Wizard installer (CursorSyncKeeper_Setup.exe) ----
#define IDD_WIZARD       200
#define IDC_WIZ_TITLE    201
#define IDC_W0_TEXT      202
#define IDC_W1_PROMPT    203
#define IDC_PATH         204
#define IDC_BROWSE       205
#define IDC_W2_TEXT      206
#define IDC_W3_TEXT      207
#define IDC_BACK         208
#define IDC_NEXT         209
#define IDC_CANCEL       210

// ---- Control panel (CursorSyncKeeperPanel.exe) ----
#define IDD_PANEL        300
#define IDC_P_TITLE      301
#define IDC_P_INFO       302
#define IDC_P_FIX        303
#define IDC_P_UNINSTALL  304
#define IDC_P_EXIT       305
#define IDC_P_STATUS     306
