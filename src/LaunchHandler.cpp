#include "LaunchHandler.h"
#include <windows.h>
#include <openvr.h>
#include <string>
#include <filesystem>
#include "Watcher.h"
#include "Injector.h"
#include "Prefs.h"
#include "HomeLogger.h"
#include "UI.h"
#include "OVRLogWatch.h"

LaunchHandler g_launchHandler;

namespace
{
    struct EnumWindowContext
    {
        DWORD pid;
        bool sentClose;
    };

    // Case insensitive substring test
    bool ContainsNoCase(const char* haystack, const char* needle)
    {
        std::string h(haystack ? haystack : "");
        std::string n(needle);
        for (char& c : h) c = (char)std::tolower((unsigned char)c);
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        return h.find(n) != std::string::npos;
    }

    bool OculusDashRunning()
    {
        return injector::FindProcessId(L"OculusDash.exe");
    }

    // Post WM_CLOSE to each visible top-level window owned by the target pid
    BOOL CALLBACK CloseWindowForPid(HWND hwnd, LPARAM lParam)
    {
        auto* context = reinterpret_cast<EnumWindowContext*>(lParam);
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);

        if (windowPid == context->pid && IsWindowVisible(hwnd))
        {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            context->sentClose = true;
        }
        return TRUE;
    }

    // Ask the home process to close by posting WM_CLOSE to its window, wait up to waitMs for a clean exit, then fall back to TerminateProcess only if it is still alive
    bool RequestCloseThenKill(DWORD pid, DWORD waitMs)
    {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!process)
        {
            return false;
        }

        EnumWindowContext context{ pid, false };
        EnumWindows(CloseWindowForPid, reinterpret_cast<LPARAM>(&context));

        if (context.sentClose && WaitForSingleObject(process, waitMs) == WAIT_OBJECT_0)
        {
            CloseHandle(process);
            return true;
        }

        // No window took the close, so force it down as a last resort.
        BOOL terminated = TerminateProcess(process, 0);
        if (terminated)
        {
            WaitForSingleObject(process, waitMs);
        }
        CloseHandle(process);
        return terminated == TRUE;
    }
}

void LaunchHandler::Start()
{
    if (running_.load())
    {
        return;
    }

    running_.store(true);
    thread_ = std::thread(&LaunchHandler::Loop, this);
    homeLogger.write() << "AutoLaunch: watching the VR runtime for an idle dashboard." << std::endl;
}

void LaunchHandler::Stop()
{
    running_.store(false);
    if (thread_.joinable())
    {
        thread_.join();
    }
}

// Read a short string property of the active HMD. "" if unavailable.
static std::string HmdString(vr::ETrackedDeviceProperty prop)
{
    if (!vr::VRSystem()) return {};
    char buf[128] = {};
    vr::ETrackedPropertyError err = vr::TrackedProp_Success;
    vr::VRSystem()->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, prop, buf, sizeof(buf), &err);
    return err == vr::TrackedProp_Success ? std::string(buf) : std::string();
}

bool HmdIsOculus()
{
    if (!vr::VRSystem() || !vr::VRSystem()->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd)) return false;

    std::string tracking = HmdString(vr::Prop_TrackingSystemName_String); // "oculus" on the Oculus driver
    std::string maker = HmdString(vr::Prop_ManufacturerName_String); // "Oculus" / "Meta"
    return ContainsNoCase(tracking.c_str(), "oculus") || ContainsNoCase(maker.c_str(), "oculus") || ContainsNoCase(maker.c_str(), "meta");
}

void LaunchHandler::DoExitHome()
{
    DWORD pid = injector::FindProcessId(prefs.configuredHomeProcessW.c_str());
    if (pid == 0)
    {
        return;
    }

    launchPending_.store(false);
    closePending_.store(true);
    homeLogger.write() << "Exit Home: requesting clean close of Home (pid " << pid << ") ..." << std::endl;

    // The watcher notices the process is gone and arms again
    std::thread([this, pid]()
        {
            bool closed = RequestCloseThenKill(pid, 6000);
            if (closed)
            {
                homeLogger.write() << "Exit Home: Home closed (pid " << pid << ")." << std::endl;
                closePending_.store(false);
            }
            else
            {
                // Could not open or kill the process
                closePending_.store(false);
                homeLogger.write() << "Exit Home: could not close Home (pid " << pid << ")." << std::endl;
            }
    }).detach();
}

void LaunchHandler::DoLaunchHome()
{
    // Can't run with Steam barring Revive
    if ((!ui.launchWithRevive || ui.reviveInjectorPath.empty()) && openvrReady && !HmdIsOculus())
    {
        if (!autoLaunchedHome)
        {
            ui.GetEnv()->noticeMessage = "Unable to launch Oculus Home: Revive is not configured.\nTo run with SteamVR, setup Revive in the \"Settings\" page.";
            ui.GetEnv()->nextPopup = "Notice";
        }
        return;
    }

    DWORD pid = injector::FindProcessId(prefs.configuredHomeProcessW.c_str());
    if (pid != 0)
    {
        return;
    }

    if (!std::filesystem::exists(ui.home2ExePath))
    {
        if (!autoLaunchedHome)
        {
            ui.GetEnv()->noticeMessage = "Unable to launch Oculus Home: The set executable does not exist.";
            ui.GetEnv()->nextPopup = "Notice";
        }
        return;
    }

    // Just launch Home executable normally. Launching from explorer or shortcut can also work
    if (ui.home2ExePath.empty())
    {
        ui.GetEnv() -> noticeMessage = "Set the home's executable first using \"Set Executable\", then retry \"Launch Home\".";
        ui.GetEnv() -> nextPopup = "Notice";
        return;
    }

    std::wstring exeW = prefs.Widen(ui.home2ExePath);
    std::wstring dir = exeW;
    std::wstring appContext = exeW;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        dir = dir.substr(0, slash);
    }

    std::wstring cmd = L"\"" + exeW + L"\"" + L" -windowed -HideAllWindows -UNATTENDED";

    // Add Revive injector as the main app targeting home shipping
    if (ui.launchWithRevive && !ui.reviveInjectorPath.empty() && !OculusDashRunning())
    {
        // Check if the injector exists
        if (!std::filesystem::exists(ui.reviveInjectorPath))
        {
            if (!autoLaunchedHome)
            {
                ui.GetEnv()->noticeMessage = "Unable to launch Oculus Home: Revive injector does not exist.";
                ui.GetEnv()->nextPopup = "Notice";
            }
            return;
        }

        cmd = L"\"" + prefs.Widen(ui.reviveInjectorPath) + L"\"" + L" " + cmd;
        appContext = prefs.Widen(ui.reviveInjectorPath);
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(appContext.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        launchPending_.store(true);
        homeLogger.write() << "Launched Home! Injection happens automatically" << std::endl;
    }
    else
    {
        homeLogger.write() << "Launch failed (error " << GetLastError() << ")." << std::endl;
        ui.GetEnv() -> noticeMessage = "Could not launch the Home2 executable. Check the path in Set Executable.";
        ui.GetEnv() -> nextPopup = "Notice";
    }
}

bool LaunchHandler::OpenVrStart()
{
    if (openvrReady)
    {
        return true;
    }

    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    openvrReady = (err == vr::VRInitError_None);

    if (!openvrReady)
    {
        vr::VR_Shutdown(); // release any partial init
    }
    else
    {
        armed = true; // a fresh runtime session may auto-launch Home once, like the old startup behavior
        homeLogger.write() << "AutoLaunch: connected to the VR runtime." << std::endl;
    }
    return openvrReady;
}

void LaunchHandler::OpenVrStop()
{
    if (!openvrReady)
    {
        return;
    }

    vr::VR_Shutdown();
    openvrReady = false;
    eligibleSinceTick_ = 0;
}

void LaunchHandler::PollRuntimeEvents()
{
    if (!openvrReady || !vr::VRSystem())
    {
        return;
    }

    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof(ev)))
    {
        if (ev.eventType == vr::VREvent_Quit)
        {
            // SteamVR is shutting down. Drop the connection so it does not close this process, but the loop reconnects if it returns
            homeLogger.write() << "AutoLaunch: VR runtime is quitting, releasing the connection." << std::endl;
            OpenVrStop();

            DoExitHome(); // Close the home process as well to not leave it running dormant
            break;
        }
    }
}

bool LaunchHandler::OpenVRHeadsetInUse() const
{
    if (!openvrReady || !vr::VRSystem())
    {
        return false;
    }

    // Only when the user is actively wearing the headset, so Home never launches with no HMD
    vr::EDeviceActivityLevel level = vr::VRSystem()->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
    return level >= 0;
}

bool LaunchHandler::InVoid(bool ocDashActive)
{
    // Handle Oculus Dash
    if (ocDashActive && g_ovrLogWatch.Ready() && !g_ovrLogWatch.AppRunning())
    {
        return true;
    }

    // Check for a running SteamVR scene process
    if (openvrReady && !ocDashActive)
    {
        uint32_t scenePid = vr::VRApplications()->GetCurrentSceneProcessId();
        if (scenePid == 0)
        {
            return true; // nothing owns the scene, so the user is in the plain dashboard void
        }
        else
        {
            // A scene app is running. SteamVR Home is its own environment app and still counts as the void to launch from.
            char key[vr::k_unMaxApplicationKeyLength] = {};
            if (vr::VRApplications()->GetApplicationKeyByProcessId(scenePid, key, sizeof(key)) == vr::VRApplicationError_None)
            {
                // SteamVR Home counts
                if (ContainsNoCase(key, "steamvr_environments"))
                {
                    return true;
                }

                return false;
            }
        }
    }

    return false;
}

void LaunchHandler::HandleStuckQuit(unsigned long long now)
{
    // Launching a VR app from Home makes SteamVR tell Home, the current scene app, to quit, and the new app waits on Home's process to go.
    if (!openvrReady || !vr::VRApplications() || !g_homeWatcher.HomeRunning() || vr::VRApplications()->GetSceneApplicationState() != vr::EVRSceneApplicationState_Quitting)
    {
        quittingSinceTick_ = 0;
        return;
    }

    if (quittingSinceTick_ == 0)
    {
        quittingSinceTick_ = now;
    }
    else if (now - quittingSinceTick_ >= kStuckQuitMs)
    {
        homeLogger.write() << "AutoLaunch: scene app stuck quitting, closing Home so the app transition can finish." << std::endl;
        DoExitHome();
        quittingSinceTick_ = now; // re-arm so another close only comes after a further wait if Home is somehow still up
    }
}

void LaunchHandler::Loop()
{
    while (running_.load())
    {
        unsigned long long now = GetTickCount64();

        // Keep a live connection to the OpenVR runtime.
        if (!openvrReady && now - lastConnectTick_ >= kReconnectMs)
        {
            lastConnectTick_ = now;
            OpenVrStart();
        }

        if (openvrReady)
        {
            PollRuntimeEvents(); // may drop the connection on a runtime quit
            HandleStuckQuit(now); // close Home if it is undergoing a scene-app transition to another app
        }

        bool eligible = false;
        bool homeUp = g_homeWatcher.HomeRunning();
        bool ocDashActive = OculusDashRunning();

        if (openvrReady || ocDashActive)
        {
            // In dashboard presence with SteamVR or Oculus
            bool inVoid = InVoid(ocDashActive);
            if (homeUp)
            {
                armed = false;
            }
            else if (!inVoid)
            {
                armed = true; // a real scene app is in focus which is not Home and not the dashboard void
            }

            if (armed && !homeUp && inVoid && (ocDashActive || OpenVRHeadsetInUse()))
            {
                eligible = ui.autoLaunchEnabled && !ui.home2ExePath.empty();
            }
        }
        else
        {
            armed = true; // neither runtime present, re-arm

            // Neither platform is active. Close a auto-launched home session
            if (autoLaunchedHome)
            {
                autoLaunchedHome = false;
                DoExitHome();
            }
        }

        if (!homeUp)
        {
            closePending_.store(false);
            autoLaunchedHome = false;
        }
        else
        {
            launchPending_.store(false);
        }

        if (eligible)
        {
            if (eligibleSinceTick_ == 0)
            {
                eligibleSinceTick_ = now;
            }
            else if (now - eligibleSinceTick_ >= (unsigned long long)cooldownMs_.load())
            {
                autoLaunchedHome = true;
                eligibleSinceTick_ = now; // restart the wait so a stalled launch retries only after another cooldown
                homeLogger.write() << "AutoLaunch: headset idle in the void, requesting a Home launch." << std::endl;

                DoLaunchHome();
            }
        }
        else
        {
            eligibleSinceTick_ = 0;
        }

        Sleep(kPollMs);
    }

    OpenVrStop();
}
