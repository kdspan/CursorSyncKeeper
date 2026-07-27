cmake_minimum_required(VERSION 3.10)
project(CursorSyncKeeper)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WIN32: build a GUI subsystem executable (no console window)
add_executable(CursorSyncKeeper WIN32
    src/main.cpp
    src/HiddenWindow.cpp
    src/CursorFixer.cpp
)

# user32  : window / message pump / SystemParametersInfo / SendInput
# kernel32: GetModuleFileName / registry helpers / CreateProcess
# advapi32: RegOpenKeyEx / RegSetValueEx (HKLM write) / CheckTokenMembership
# shell32 : ShellExecuteEx (UAC elevation)
target_link_libraries(CursorSyncKeeper user32 kernel32 advapi32 shell32)

if(MSVC)
    # Smaller, release-optimized binary; strip unnecessary stuff
    target_link_options(CursorSyncKeeper PRIVATE /ENTRY:WinMainCRTStartup)
    set_target_properties(CursorSyncKeeper PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF")
endif()
