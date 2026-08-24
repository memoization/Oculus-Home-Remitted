#pragma once
#include <windows.h>

// System-tray presence so the app stays open and the injector watcher keeps running when the window is closed to the tray. Subclasses the GLFW window proc to handle tray clicks and a Restore/Quit menu.
namespace tray
{
    void Install(HWND hwnd); // add the tray icon and subclass the window proc
    void Remove(); // remove the icon and restore the original proc
}
