#pragma once
#include <atomic>
#include <thread>
#include <string>
#include <windows.h>

// Resident background injector. While the frontend is alive, a poll thread watches for "Home2-Win64-Shipping.exe" and injects home2backend.dll into any new instance, then re-arms when that process exits.
// It catches Home running whether it was started by the app's Launch Home button or started directly.
class HomeWatcher
{
public:
    void Start(const std::wstring& dllPath); // dllPath = the home2backend.dll to inject
    void Stop();

    bool HomeRunning() const { return homeRunning_.load(); }
    bool Injected() const { return injectedPid_.load() != 0; }

private:
    void Loop();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::wstring dllPath_;
    std::atomic<DWORD> injectedPid_{0};
    std::atomic<bool> homeRunning_{false};
};

extern HomeWatcher g_homeWatcher;
