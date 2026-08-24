#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <dwmapi.h>
#include "BroadcastSource.h"
#include "BroadcastSourceShared.h"

namespace broadcast
{

    // The app creates. It holds the mapping and view for the whole process lifetime.
    static HANDLE gMapping = nullptr;
    static shared::BroadcastSourceShared* gView = nullptr;

    static std::string NarrowUtf8(const std::wstring& wide)
    {
        if (wide.empty()) return std::string();

        int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    // Seqlock writer: bump generation odd, write the payload, and bump even.
    // Concurrent probe reader that observes an odd (or a changed) generation retries, so it never sees a torn value
    static void Publish(uint32_t kind, uint64_t id)
    {
        if (!gView)
            return;
        gView->generation++; // now odd: write in progress
        MemoryBarrier();
        gView->kind = kind;
        gView->id = id;
        gView->reserved = 0;
        gView->version = shared::kBroadcastSourceVersion;
        MemoryBarrier();
        gView->generation++; // now even: sorted
    }

    uint64_t PrimaryMonitor()
    {
        POINT origin = {0, 0};
        HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mon));
    }

    void Init()
    {
        if (gView) return;
        gMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(shared::BroadcastSourceShared), shared::kBroadcastSourceObject);
        if (!gMapping) return;

        gView = reinterpret_cast<shared::BroadcastSourceShared*>(MapViewOfFile(gMapping, FILE_MAP_WRITE, 0, 0, sizeof(shared::BroadcastSourceShared)));
        if (!gView)
        {
            CloseHandle(gMapping);
            gMapping = nullptr;
            return;
        }
        gView->generation = 0;

        // Default = primary monitor, use as fallback.
        Publish(shared::BroadcastSourceMonitor, PrimaryMonitor());
    }

    void Shutdown()
    {
        if (gView)
        {
            UnmapViewOfFile(gView);
            gView = nullptr;
        }
        if (gMapping)
        {
            CloseHandle(gMapping);
            gMapping = nullptr;
        }
    }

    void SetMonitor(uint64_t hmon)
    {
        Publish(shared::BroadcastSourceMonitor, hmon);
    }

    void SetApp(uint64_t hwnd)
    {
        Publish(shared::BroadcastSourceApp, hwnd);
    }

    std::vector<MonitorSource> EnumMonitorSources()
    {
        std::vector<MonitorSource> out;
        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL
            {
                auto* v = reinterpret_cast<std::vector<MonitorSource>*>(lp);
                MONITORINFOEXW mi;
                mi.cbSize = sizeof(mi);

                if (!GetMonitorInfoW(h, &mi)) return TRUE;

                bool primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                int width = mi.rcMonitor.right - mi.rcMonitor.left;
                int height = mi.rcMonitor.bottom - mi.rcMonitor.top;

                // szDevice is "\\.\DISPLAYn": the trailing number matches Windows Settings "Display n".
                std::wstring device = mi.szDevice;
                std::string number;
                for (wchar_t c : device)
                {
                    if (c >= L'0' && c <= L'9')
                    {
                        number.push_back(static_cast<char>(c));
                    }
                }

                MonitorSource m;
                m.handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
                m.primary = primary;
                m.label = "Display " + (number.empty() ? std::string("?") : number) + " - " + std::to_string(width) + "x" + std::to_string(height);
                if (primary)
                {
                    m.label += " (Primary)";
                }
                
                v->push_back(std::move(m));
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&out));
    
        return out;
    }

    std::vector<AppSource> EnumAppWindows()
    {
        std::vector<AppSource> out;
        EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL
            {
                auto* v = reinterpret_cast<std::vector<AppSource>*>(lp);
                if (!IsWindowVisible(h)) return TRUE;

                if (GetWindow(h, GW_OWNER) != nullptr) return TRUE; // dialogs / tool popups, not a top-level app window
                if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

                int len = GetWindowTextLengthW(h);
                if (len <= 0) return TRUE;

                // Skip cloaked UWP ghost windows (enumerable but not actually on screen).
                int cloaked = 0;
                if (DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) == S_OK && cloaked != 0)
                {
                    return TRUE;
                }

                std::wstring title(static_cast<size_t>(len) + 1, 0);
                GetWindowTextW(h, &title[0], len + 1);
                title.resize(static_cast<size_t>(len));

                AppSource a;
                a.hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
                a.title = NarrowUtf8(title);
                v->push_back(std::move(a));
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&out));
    
        return out;
    }

}
