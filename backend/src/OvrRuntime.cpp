#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

#include <string>

// Direct launch survival. When Home2 is launched without OVRServer's app-launch flow such as a direct exe launch, 
// the VR runtime answers ovrp_GetAppShouldQuit=true and never grants VR focus.
// UE4's FOculusHMD calls FPlatformMisc::RequestExit and the app quits before login (seen in the game log: "went out of VR focus" then RequestExit(0)).
// Force "ShouldQuit" to false and "HasVrFocus" to true, both named exports of OVRPlugin.dll, so the app stays alive and keeps rendering.
// OVRPlugin.dll loads a few seconds after the DLL during UE HMD init, so a tiny waiter thread installs these the instant it appears, well before the first fatal ShouldQuit poll.
namespace home2backend {

    typedef int OvrpBool; // ovrpBool_False = 0, ovrpBool_True = 1
    typedef int OvrpResult; // ovrpSuccess = 0

    typedef OvrpBool(*ShouldQuitFn)();
    typedef OvrpResult(*ShouldQuit2Fn)(OvrpBool*);
    typedef OvrpBool(*HasVrFocusFn)();
    typedef OvrpResult(*HasVrFocus2Fn)(OvrpBool*);

    static ShouldQuitFn GOrigShouldQuit = nullptr;
    static ShouldQuit2Fn GOrigShouldQuit2 = nullptr;
    static HasVrFocusFn GOrigHasVrFocus = nullptr;
    static HasVrFocus2Fn GOrigHasVrFocus2 = nullptr;

    static volatile long GQuitLogged = 0;
    static volatile long GFocusLogged = 0;

    static OvrpBool DetourShouldQuit()
    {
        if (GOrigShouldQuit)
            GOrigShouldQuit(); // preserve any runtime side effects
        if (InterlockedIncrement(&GQuitLogged) <= 2)
            LogLine("ovr: ovrp_GetAppShouldQuit forced false (keep app alive)");
        return 0; // ovrpBool_False
    }

    static OvrpResult DetourShouldQuit2(OvrpBool* out)
    {
        if (GOrigShouldQuit2)
            GOrigShouldQuit2(out);
        if (out)
            *out = 0;
        if (InterlockedIncrement(&GQuitLogged) <= 2)
            LogLine("ovr: ovrp_GetAppShouldQuit2 forced false (keep app alive)");
        return 0; //ovrpSuccess so UE trusts
    }

    static OvrpBool DetourHasVrFocus()
    {
        if (GOrigHasVrFocus)
            GOrigHasVrFocus();
        if (InterlockedIncrement(&GFocusLogged) <= 2)
            LogLine("ovr: ovrp_GetAppHasVrFocus forced true");
        return 1; // ovrpBool_True
    }

    static OvrpResult DetourHasVrFocus2(OvrpBool* out)
    {
        if (GOrigHasVrFocus2)
            GOrigHasVrFocus2(out);
        if (out)
            *out = 1;
        if (InterlockedIncrement(&GFocusLogged) <= 2)
            LogLine("ovr: ovrp_GetAppHasVrFocus2 forced true");
        return 0;
    }

    static bool HookByName(HMODULE mod, const char* name, void* detour, void** orig)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
        if (!target)
        {
            LogLine(std::string("ovr: export not found: ") + name);
            return false;
        }

        if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK)
        {
            LogLine(std::string("ovr: hook failed: ") + name);
            return false;
        }
        return true;
    }

    static void InstallNow(HMODULE ovrp)
    {
        bool q1 = HookByName(ovrp, "ovrp_GetAppShouldQuit",
                             reinterpret_cast<void*>(&DetourShouldQuit),
                             reinterpret_cast<void**>(&GOrigShouldQuit));
        bool q2 = HookByName(ovrp, "ovrp_GetAppShouldQuit2",
                             reinterpret_cast<void*>(&DetourShouldQuit2),
                             reinterpret_cast<void**>(&GOrigShouldQuit2));
        HookByName(ovrp, "ovrp_GetAppHasVrFocus",
                   reinterpret_cast<void*>(&DetourHasVrFocus),
                   reinterpret_cast<void**>(&GOrigHasVrFocus));
        HookByName(ovrp, "ovrp_GetAppHasVrFocus2",
                   reinterpret_cast<void*>(&DetourHasVrFocus2),
                   reinterpret_cast<void**>(&GOrigHasVrFocus2));
        LogLine(std::string("ovr: OVRPlugin runtime hooks installed (ShouldQuit forced false, VrFocus forced true). ShouldQuit ") + ((q1 || q2) ? "hooked" : "not hooked, app may still quit"));
    }

    static DWORD WINAPI Waiter(LPVOID)
    {
        // OVRPlugin.dll is loaded by UE's HMD init a few seconds in
        for (int i = 0; i < 900; ++i) // up to 90s
        {
            HMODULE ovrp = GetModuleHandleW(L"OVRPlugin.dll");
            if (ovrp)
            {
                LogLine("ovr: OVRPlugin.dll detected, installing ShouldQuit/VrFocus hooks");
                InstallNow(ovrp);
                return 0;
            }
            Sleep(100);
        }

        LogLine("ovr: OVRPlugin.dll never appeared within 90s, ShouldQuit hooks not installed (direct launch will still quit)");
        return 0;
    }

    bool InstallOvrRuntimeHooks()
    {
        HANDLE t = CreateThread(nullptr, 0, Waiter, nullptr, 0, nullptr);
        if (t)
        {
            CloseHandle(t);
        }

        return true;
    }

}
