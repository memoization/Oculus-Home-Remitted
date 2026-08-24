#define WIN32_LEAN_AND_MEAN
#include "Tray.h"
#include <shellapi.h>
#include "UI.h"

static const UINT WM_TRAY = WM_APP + 1;
enum
{
    ID_TRAY_RESTORE = 1001,
    ID_TRAY_QUIT = 1002
};

static WNDPROC g_origProc = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HWND g_hwnd = nullptr;

static void RestoreWindow(HWND h)
{
    ShowWindow(h, SW_SHOW);
    ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
    // The render loop clears its "closed" flag once it sees the window visible again
}

static void ShowTrayMenu(HWND h)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_RESTORE, L"Open Oculus Home Remitted");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
    SetForegroundWindow(h); // purpose: so the menu dismisses on outside-click
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
    DestroyMenu(menu);
}

static LRESULT CALLBACK TrayProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_TRAY)
    {
        if (l == WM_LBUTTONUP || l == WM_LBUTTONDBLCLK)
            RestoreWindow(h);
        else if (l == WM_RBUTTONUP)
            ShowTrayMenu(h);
        return 0;
    }
    if (msg == WM_COMMAND && HIWORD(w) == 0)
    {
        if (LOWORD(w) == ID_TRAY_RESTORE)
        {
            RestoreWindow(h);
            return 0;
        }
        if (LOWORD(w) == ID_TRAY_QUIT)
        {
            ui.keepAlive.store(false); // render loop exits, app quits (Remove() runs on the way out)
            return 0;
        }
    }

    return CallWindowProcW(g_origProc, h, msg, w, l);
}

namespace tray
{

void Install(HWND hwnd)
{
    if (!hwnd || g_hwnd)
        return;
    g_hwnd = hwnd;
    g_origProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(TrayProc)));

    HICON icon = LoadIconW(GetModuleHandleW(nullptr), L"GLFW_ICON");
    if (!icon)
    {
        icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = icon;
    wcscpy_s(g_nid.szTip, L"Oculus Home Remitted");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void Remove()
{
    if (!g_hwnd) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_origProc)
    {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origProc));
    }
        
    g_hwnd = nullptr;
    g_origProc = nullptr;
}

}
