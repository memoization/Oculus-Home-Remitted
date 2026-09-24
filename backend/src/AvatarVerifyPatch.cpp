#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

// Defeat certificate verification inside libovravatar.dll's own linked OpenSSL 1.1.0d.
//
// The Avatar SDK fetches each avatar asset over its separate OpenSSL, not the game exe's 1.0.2 that 
// verify hook already covers. Offline those fetches are redirected to the loopback, but libovravatar
// rejects the CA-signed leaf with unknown_ca, so LoadAssetTask fails and no mesh loads. This module
// forces its X509_verify_cert to return success, so the handshake completes and libcurl's own
// post-handshake SSL_get_verify_result check also passes.
//
// libovravatar.dll lives in the Oculus runtime, outside the Home install, so a fixed RVA would rot on a
// runtime update. Instead the target is found at run time by a byte signature, ASLR immune and
// re-derivable if a rebuild shifts it. The signature was taken from X509_verify_cert's prologue, which
// build_chain reaches, and the chkstk call's rel32 is wildcarded since it can move with the load base.
//
//   48 89 5C 24 08      mov  [rsp+8], rbx
//   57                  push rdi
//   B8 30 00 00 00      mov  eax, 0x30
//   E8 ?? ?? ?? ??      call __chkstk            rel32 wildcarded
//   48 2B E0            sub  rsp, rax
//   48 83 79 08 00      cmp  qword ptr [rcx+8], 0    the ctx->cert null check
// The first six bytes become  B8 01 00 00 00 C3  (mov eax, 1 ; ret), so the call returns 1 before it
// touches the stack it was about to allocate.

namespace home2backend {

    // true = the byte must match, false = wildcard (the chkstk rel32).
    static const BYTE kSig[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
        0x00, 0x00, 0x00, 0x00,
        0x48, 0x2B, 0xE0, 0x48, 0x83, 0x79, 0x08, 0x00
    };
    static const bool kMask[] = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        0, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 1, 1
    };

    static volatile long GAvatarVerifyDone = 0;

    // Resolve the module's .text section bounds from its PE headers so the scan stays inside code.
    static bool TextSection(HMODULE mod, BYTE** outBase, size_t* outSize)
    {
        BYTE* base = reinterpret_cast<BYTE*>(mod);
        IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

        IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            if (memcmp(sec[i].Name, ".text", 5) == 0)
            {
                *outBase = base + sec[i].VirtualAddress;
                *outSize = sec[i].Misc.VirtualSize;
                return true;
            }
        }
        return false;
    }

    static BYTE* FindSignature(BYTE* start, size_t size)
    {
        const size_t n = sizeof(kSig);
        if (size < n) return nullptr;

        for (size_t i = 0; i + n <= size; ++i)
        {
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
            {
                if (kMask[j] && start[i + j] != kSig[j]) { ok = false; break; }
            }
            if (ok) return start + i;
        }
        return nullptr;
    }

    static bool PatchReturnTrue(BYTE* fn)
    {
        const BYTE patch[6] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 }; // mov eax,1 ; ret
        DWORD old = 0;
        if (!VirtualProtect(fn, sizeof(patch), PAGE_EXECUTE_READWRITE, &old))
            return false;

        memcpy(fn, patch, sizeof(patch));
        VirtualProtect(fn, sizeof(patch), old, &old);
        FlushInstructionCache(GetCurrentProcess(), fn, sizeof(patch));
        return true;
    }

    static void ApplyNow(HMODULE mod)
    {
        BYTE* textBase = nullptr;
        size_t textSize = 0;
        if (!TextSection(mod, &textBase, &textSize))
        {
            LogLine("avatarverify: could not read libovravatar.dll .text, avatar assets will not load offline");
            return;
        }

        BYTE* fn = FindSignature(textBase, textSize);
        if (!fn)
        {
            LogLine("avatarverify: X509_verify_cert signature not found in libovravatar.dll (runtime build differs), avatar assets will not load offline");
            return;
        }

        // Count a second match so a non-unique signature is caught rather than silently patching the wrong site.
        BYTE* second = FindSignature(fn + 1, textSize - static_cast<size_t>(fn + 1 - textBase));
        if (second)
        {
            LogLine("avatarverify: signature matched more than once in libovravatar.dll, ambiguous, skipped");
            return;
        }

        uintptr_t rva = static_cast<uintptr_t>(fn - reinterpret_cast<BYTE*>(mod));
        char rvaHex[24] = { 0 };
        _snprintf_s(rvaHex, sizeof(rvaHex), _TRUNCATE, "0x%llx", static_cast<unsigned long long>(rva));

        if (PatchReturnTrue(fn))
            LogLine(std::string("avatarverify: libovravatar X509_verify_cert forced to return 1 @+") + rvaHex + ", the avatar asset TLS now trusts the loopback");
        else
            LogLine(std::string("avatarverify: VirtualProtect failed on libovravatar X509_verify_cert @+") + rvaHex);
    }

    static DWORD WINAPI Waiter(LPVOID)
    {
        // libovravatar.dll maps when the Avatar SDK initializes, a few seconds after injection and well before the first avatar asset fetch, so a light poll arms the patch in time.
        for (int i = 0; i < 2400 && !GAvatarVerifyDone; ++i) // up to ~120s at 50ms
        {
            HMODULE mod = GetModuleHandleW(L"libovravatar.dll");
            if (mod)
            {
                LogLine("avatarverify: libovravatar.dll detected, applying X509_verify_cert patch");
                ApplyNow(mod);
                InterlockedExchange(&GAvatarVerifyDone, 1);
                return 0;
            }
            Sleep(50);
        }

        if (!GAvatarVerifyDone)
            LogLine("avatarverify: libovravatar.dll never appeared within 120s, avatar TLS defeat not applied");
        return 0;
    }

    bool InstallAvatarVerifyPatch()
    {
        HANDLE t = CreateThread(nullptr, 0, Waiter, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
        return true;
    }

}
