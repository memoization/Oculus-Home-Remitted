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
    typedef OvrpResult(*HasInputFocusFn)(OvrpBool*);
    typedef OvrpResult(*HasSystemOverlayFn)(OvrpBool*);

    static ShouldQuitFn GOrigShouldQuit = nullptr;
    static ShouldQuit2Fn GOrigShouldQuit2 = nullptr;
    static HasVrFocusFn GOrigHasVrFocus = nullptr;
    static HasVrFocus2Fn GOrigHasVrFocus2 = nullptr;
    static HasInputFocusFn GOrigHasInputFocus = nullptr;
    static HasSystemOverlayFn GOrigHasSystemOverlay = nullptr;

    static volatile long GQuitLogged = 0;
    static volatile long GFocusLogged = 0;
    static volatile long GInputFocusLogged = 0;
    static volatile long GOverlayLogged = 0;
    static volatile long GHadRealVrFocus = 0; // latched once the runtime has genuinely granted the app VR focus

    // A real quit request only counts once the app has actually held VR focus
    static void MaybeHonorQuit(bool realShouldQuit)
    {
        if (realShouldQuit && GHadRealVrFocus)
        {
            LogLine("ovr: quit requested after the app held focus, exiting the process");
            ExitProcess(0);
        }
    }

    static OvrpBool DetourShouldQuit()
    {
        OvrpBool real = GOrigShouldQuit ? GOrigShouldQuit() : 0; // the runtime's real answer, also preserves side effects
        MaybeHonorQuit(real != 0);
        if (InterlockedIncrement(&GQuitLogged) <= 2)
            LogLine("ovr: ovrp_GetAppShouldQuit forced false (keep app alive)");
        return 0; // ovrpBool_False
    }

    static OvrpResult DetourShouldQuit2(OvrpBool* out)
    {
        OvrpBool real = 0;
        if (GOrigShouldQuit2)
        {
            GOrigShouldQuit2(out);
            if (out) real = *out;
        }
        MaybeHonorQuit(real != 0);
        if (out)
            *out = 0;
        if (InterlockedIncrement(&GQuitLogged) <= 2)
            LogLine("ovr: ovrp_GetAppShouldQuit2 forced false (keep app alive)");
        return 0; //ovrpSuccess so UE trusts
    }

    static OvrpBool DetourHasVrFocus()
    {
        if (GOrigHasVrFocus && GOrigHasVrFocus())
            InterlockedExchange(&GHadRealVrFocus, 1); // the app truly has focus now, so later quits are user-driven
        if (InterlockedIncrement(&GFocusLogged) <= 2)
            LogLine("ovr: ovrp_GetAppHasVrFocus forced true");
        return 1; // ovrpBool_True
    }

    static OvrpResult DetourHasVrFocus2(OvrpBool* out)
    {
        if (GOrigHasVrFocus2)
        {
            GOrigHasVrFocus2(out);
            if (out && *out)
                InterlockedExchange(&GHadRealVrFocus, 1);
        }
        if (out)
            *out = 1;
        if (InterlockedIncrement(&GFocusLogged) <= 2)
            LogLine("ovr: ovrp_GetAppHasVrFocus2 forced true");
        return 0;
    }

    static OvrpResult DetourHasInputFocus(OvrpBool* out)
    {
        if (GOrigHasInputFocus)
            GOrigHasInputFocus(out); // preserve any runtime side effects
        if (out)
            *out = 1; // ovrpBool_True
        if (InterlockedIncrement(&GInputFocusLogged) <= 2)
            LogLine("ovr: ovrp_GetAppHasInputFocus forced true (keep controllers live)");
        return 0; // ovrpSuccess
    }

    // A present system overlay can suppress input. Report none so input is never held off for it.
    static OvrpResult DetourHasSystemOverlayPresent(OvrpBool* out)
    {
        if (GOrigHasSystemOverlay)
            GOrigHasSystemOverlay(out);
        if (out)
            *out = 0; // ovrpBool_False, no overlay present
        if (InterlockedIncrement(&GOverlayLogged) <= 2)
            LogLine("ovr: ovrp_GetAppHasSystemOverlayPresent forced false");
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
        bool underRevive = IsUnderRevive();

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

        // Keep hand and controller input alive regardless of the dashboard, but only under Revive, which is where the input focus can break
        if (underRevive)
        {
            HookByName(ovrp, "ovrp_GetAppHasInputFocus",
                       reinterpret_cast<void*>(&DetourHasInputFocus),
                       reinterpret_cast<void**>(&GOrigHasInputFocus));
            HookByName(ovrp, "ovrp_GetAppHasSystemOverlayPresent",
                       reinterpret_cast<void*>(&DetourHasSystemOverlayPresent),
                       reinterpret_cast<void**>(&GOrigHasSystemOverlay));
        }

        LogLine(std::string("ovr: OVRPlugin runtime hooks installed (ShouldQuit forced false, VrFocus forced true") + (underRevive ? ", InputFocus forced true and SystemOverlay forced absent for Revive" : ", InputFocus left native") + "). ShouldQuit " + ((q1 || q2) ? "hooked" : "not hooked, app may still quit"));
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
