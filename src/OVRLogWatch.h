#pragma once
#include <atomic>
#include <thread>
#include <string>
#include <map>
#include <windows.h>

// HACK HACK HACK
// Tells whether an VR app is running on the Meta Link software, where OpenVR cannot be used.
// It tails the newest Service_*.txt log under %LOCALAPPDATA%\Oculus.
// This is the Elmer's Glue solution to keeping tabs on OVR app activity. Very subjected to breaking in the future.
class OVRLogWatch
{
public:
    void Start();
    void Stop();

    // True when at least one tracked Oculus VR app process is still alive.
    bool AppRunning() const { return appRunning_.load(); }

    // True once a service log has been found and is being tailed.
    bool Ready() const { return ready_.load(); }

private:
    struct TrackedApp
    {
        std::string image; // the process exe base name which is resolved from the live process for clear logging
        HANDLE process; // a handle to the launched process
    };

    void Loop();
    std::wstring FindLatestLog() const; // the newest Service_*.txt, or empty
    bool OpenLog(const std::wstring& path); // open, reset state, replay from the start
    void CloseLog();
    void DrainNewLines();// read appended bytes and parse whole lines
    void ConsumeLine(const std::string& line);
    void PruneDeadApps(); // drop any tracked pid whose process has exited
    void UpdateRunningState(); // recompute appRunning_ and log a transition
    void Track(DWORD pid, const std::string& expectedExe, unsigned long long lineTimeFt); // begin tracking a launched app by pid, confirming the live process really is that exe and the same instance the line named
    void UntrackAll(); // stop tracking every app and release the handles

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> appRunning_{ false };
    std::atomic<bool> ready_{ false };

    std::wstring logPath_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::string partial_; // trailing incomplete line carried to the next read
    std::map<DWORD, TrackedApp> apps_; // pid to the app tracked under it
    unsigned short logYear_ = 0; // the year from the log file name, since log lines carry only day and month
    unsigned long long lastScanTick_ = 0; // last time a newer service log was checked
};

extern OVRLogWatch g_ovrLogWatch;
