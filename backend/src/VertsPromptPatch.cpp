#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>

// Suppress the "Multiplayer session connection failed. Please contact customer support." dialog.
//
// Offline the Verts realtime client can never reach a server, so OvrVertsManager transitions to the
// disconnected/error status with reason ConnectionFailed. The status listener (Home2-Win64-Shipping.exe
// sub_1407CA860, reached from the OvrVertsManager status delegate) builds an error message from the
// disconnect reason and queues it as a prompt via an array-add (sub_14075E8B0) into the prompt queue at
// OvrVertsManager+0x228. After the welcome flow ends, that queue is drained and ErrorDialog_BP.ShowError shows
// the popup.
//
// NOP the single prompt-add call so nothing is queued. The disconnect teardown and the post-disconnect UI
// state handling both still run, and every other prompt is untouched since only this one call site changes.
//
// Decompiled at RVA 0x7CB015:
//     48 8B 8E 28 02 00 00   mov  rcx, [rsi+0x228]   the prompt queue
//     E8 8F 38 F9 FF         call sub_14075E8B0      queue the built error prompt   becomes 5 NOPs
// The call rel32 shifts with the load base along with its target, so these bytes are stable across ASLR.

namespace home2backend {

    bool InstallVertsPromptPatch()
    {
        BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        if (!base)
        {
            LogLine("vertsprompt: no main module handle, skipped");
            return false;
        }

        BYTE* sig = base + 0x7CB015;
        const BYTE expect[12] = { 0x48, 0x8B, 0x8E, 0x28, 0x02, 0x00, 0x00, 0xE8, 0x8F, 0x38, 0xF9, 0xFF };
        if (memcmp(sig, expect, sizeof(expect)) != 0)
        {
            LogLine(std::string("vertsprompt: signature mismatch @exe+0x7CB015, exe build differs, skipped"));
            return false;
        }

        BYTE* patch = base + 0x7CB01C; // the 5 byte call sub_14075E8B0
        DWORD old = 0;
        if (!VirtualProtect(patch, 5, PAGE_EXECUTE_READWRITE, &old))
        {
            LogLine("vertsprompt: VirtualProtect failed, skip");
            return false;
        }

        memset(patch, 0x90, 5); // NOP the prompt add so the Verts disconnect never queues a dialog
        VirtualProtect(patch, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), patch, 5);

        LogLine("vertsprompt: prompt-add NOPed @exe+0x7CB01C");
        return true;
    }

}
