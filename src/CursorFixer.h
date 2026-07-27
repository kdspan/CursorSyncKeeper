#pragma once
#include <windows.h>

class CursorFixer {
public:
    // Re-assert the user's persisted mouse-trail setting into the live
    // kernel state. Reading the value from the registry guarantees we
    // restore exactly what the user configured (0 = off, 1-7 = trails),
    // even if the display driver wiped the in-memory flag.
    static void Apply();

private:
    CursorFixer() = delete;
};
