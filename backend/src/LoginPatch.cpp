#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>

// Force the OculusWorlds login-complete handler onto its success path.
//
// On a direct/third-party launch the platform login can't truly complete before the
// game's synchronous GameInstance::Init login check, since the async completion is delivered a
// tick too late. So LoginManager::OnLoginComplete is invoked with bWasSuccessful false
// while state is 3 (WaitingLogin): it logs "Oculus platform login failed", sets state to 2
// (Ready), and falls back to the white void or default. The real async success arrives later
// but is discarded ("Received OnLoginComplete while not in WaitingLogin state").
//
// Decompiled handler at Home2-Win64-Shipping.exe RVA 0x3B6340:
//     0x3B635C  84 D2      test dl,dl            with dl the bWasSuccessful flag
//     0x3B635E  75 5E      jne  0x3B63BE         success path, proceed from state 3 to 4
//     ...       failure path, log "platform login failed", state 2, fallback
//     0x3B63BE  success path, when state is 3 it calls the login-complete event, sets state 4, stamps.
//                            It does not touch the async user object, the id comes from the
//                             ovr_GetLoggedInUserID spoof, then reaches WaitingWorldsLogin (served)
//
//Flip the jne (75) to an unconditional jmp (EB) so the handler always takes the
// success path. Guarded by a 4-byte signature so we only ever patch this exact build.

namespace home2backend {
    
    bool InstallLoginPatch() // magic sauce
    {
        BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        if (!base)
        {
            LogLine("loginpatch: no main module handle, skipped");
            return false;
        }
        BYTE* sig = base + 0x3B635C;
        const BYTE expect[4] = {0x84, 0xD2, 0x75, 0x5E}; // test dl,dl then jne +0x5E
        if (memcmp(sig, expect, 4) != 0)
        {
            char got[32];
            wsprintfA(got, "%02X %02X %02X %02X", sig[0], sig[1], sig[2], sig[3]);
            LogLine(std::string("loginpatch: signature mismatch @exe+0x3B635C (got ") + got + ", expected 84 D2 75 5E), exe build differs, skip (login unchanged)");
            return false;
        }

        BYTE* patch = base + 0x3B635E; // the 'jne' opcode byte
        DWORD old = 0;
        if (!VirtualProtect(patch, 1, PAGE_EXECUTE_READWRITE, &old))
        {
            LogLine("loginpatch: VirtualProtect failed, skip");
            return false;
        }

        *patch = 0xEB; //jne to jmp, always take the login-success path
        VirtualProtect(patch, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), patch, 1);

        LogLine("loginpatch: OnLoginComplete jne became jmp @exe+0x3B635E, platform login forced to the success path (state 3 to 4, proceeds to WaitingWorldsLogin).");
        return true;
    }

}
