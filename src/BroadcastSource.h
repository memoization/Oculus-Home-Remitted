#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Session-only backend broadcast-source channel. It enumerates the system's monitors & top-level app windows and publishes the user's pick into a CreateFileMapping-backed shared block that the injected probe reads live per broadcast.
// The selection is never persisted to a save file, so every launch defaults to monitor 1.
namespace broadcast
{
    struct MonitorSource
    {
        uint64_t handle; // HMONITOR value this is the id sent to the backend
        std::string label; // e.g., "Display 2 - 1920x1080 (Primary)"
        bool primary;
    };

    struct AppSource
    {
        uint64_t hwnd; //HWND value, this is the id sent to the backend
        std::string title;
    };

    // Creates the shared block and write the primary-monitor default.
    void Init();
    // Unmap and close the shared block.
    void Shutdown();

    void SetMonitor(uint64_t hmon);// publish {Monitor, hmon} via the seqlock writer
    void SetApp(uint64_t hwnd); // publish {App, hwnd} via the seqlock writer

    // NB: named EnumMonitorSources to dodge winspool.h's `#define EnumMonitors EnumMonitorsW`
    std::vector<MonitorSource> EnumMonitorSources();
    std::vector<AppSource> EnumAppWindows();

    uint64_t PrimaryMonitor(); // HMONITOR value of the primary monitor
}
