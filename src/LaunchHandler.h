#pragma once
#include <atomic>
#include <thread>

// Handles the deployment of the home instance
class LaunchHandler
{
public:
    void Start();
    void Stop();
    void DoLaunchHome();
    void DoExitHome();

    bool LaunchPending() const { return launchPending_.load(); }
    bool ClosePending() const { return closePending_.load(); }

private:
    void Loop();
    bool OpenVrStart();// connect to the runtime as a background utility app
    void OpenVrStop();// release the OpenVR runtime connection
    void PollRuntimeEvents(); // drain OpenVR events and handle a runtime quit
    void HandleStuckQuit(unsigned long long now); // force Home out if it is stuck in transition to another app
    bool InVoid(bool ocDashActive); // true when no real scene app is in the foreground
    bool OpenVRHeadsetInUse() const;// true when the user is actually wearing the headset

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> launchPending_{ false };
    std::atomic<bool> closePending_{ false };
    std::atomic<int> cooldownMs_{ 10000 }; // Grace period while in the void before home launches

    const int kPollMs = 2000; // freq of the void check and the event drain
    const int kReconnectMs = 3000; // gap between runtime connect attempts while disconnected
    const int kStuckQuitMs = 1000; // how long the scene app may sit quitting before exiting Home for it

    bool autoLaunchedHome = false;
    bool openvrReady = false;
    bool armed = true; // whether a return to the void may auto-launch Home. Cleared while Home is up so quitting Home directly to the dashboard does not relaunch it, set again once a real VR app runs
    unsigned long long eligibleSinceTick_ = 0; // GetTickCount64 when the idle void wait started, 0 when not eligible
    unsigned long long lastConnectTick_ = 0; // last runtime connect attempt, throttles reconnects
    unsigned long long quittingSinceTick_ = 0; // GetTickCount64 when the scene app entered the quitting state, 0 otherwise
};

extern LaunchHandler g_launchHandler;
