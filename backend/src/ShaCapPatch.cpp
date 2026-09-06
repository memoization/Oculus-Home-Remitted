#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

// Some CPUs with the Intel SHA extensions crash Home2 at startup with an access violation around the exe offset 0x1e4a3.
// OpenSSL runs its sha1 stack which is not 16-byte aligned in these scenarios, so they fault on that stack.
// The scalar sha1 path uses only general registers, so it is immune to the misaligned stack.
// OpenSSL picks the sha1 variant at runtime from its cached capability vector OPENSSL_ia32cap_P, and the sha1 dispatcher tests SSSE3 first, taking the scalar path when SSSE3 is absent.
// So this clears the SSSE3 bit, similar to the "OPENSSL_ia32cap" system environment variable workaround.

namespace home2backend {

    bool InstallShaCapPatch()
    {
        BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        if (!base)
        {
            LogLine("shacap: no main module handle, skipped");
            return false;
        }

        // The sha1 dispatcher loads OPENSSL_ia32cap_P[0] here with relative mov r9d.
        // The seven signature bytes carry the exact rip displacement, so a matching build uses the vector location.
        const uintptr_t kRefRva = 0x1d156;
        const BYTE expect[7] = { 0x44, 0x8B, 0x0D, 0x73, 0x50, 0xCC, 0x03 }; // mov r9d [rip+0x03cc5073]
        if (memcmp(base + kRefRva, expect, sizeof(expect)) != 0)
        {
            BYTE* p = base + kRefRva;
            char msg[144];
            wsprintfA(msg, "shacap: signature mismatch at exe offset 0x%X (got %02X %02X %02X %02X %02X %02X %02X)",
                      (unsigned)kRefRva, p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
            LogLine(msg);
            return false;
        }

        // Resolve OPENSSL_ia32cap_P from the relative displacement in that instruction.
        // The displacement is the four bytes at offset 3, and rip points at the next instruction, kRefRva plus 7.
        int32_t disp = 0;
        memcpy(&disp, base + kRefRva + 3, sizeof(disp));
        uintptr_t capRva = kRefRva + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(disp));

        volatile uint32_t* p1 = reinterpret_cast<uint32_t*>(base + capRva + 4); // OPENSSL_ia32cap_P[1], carries SSSE3 at bit 9
        volatile uint32_t* p2 = reinterpret_cast<uint32_t*>(base + capRva + 8); // OPENSSL_ia32cap_P[2], carries the SHA extensions at bit 29

        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(base + capRva), 16, PAGE_READWRITE, &old))
        {
            LogLine("shacap: VirtualProtect failed on the capability vector, skipping patch");
            return false;
        }

        uint32_t before1 = *p1;
        uint32_t before2 = *p2;
        *p1 = before1 & ~0x00000200u;// clear SSSE3 so the sha1 dispatcher goes to the scalar path
        *p2 = before2 & ~0x20000000u;// clear the SHA extensions so shaext is never selected
        uint32_t after1 = *p1;
        uint32_t after2 = *p2;
        VirtualProtect(reinterpret_cast<LPVOID>(base + capRva), 16, old, &old);

        char msg[192];
        wsprintfA(msg, "shacap: OPENSSL_ia32cap_P at exe offset 0x%X and cleared SSSE3 and SHA bits. P1 %08X to %08X, P2 %08X to %08X",
                  (unsigned)capRva, before1, after1, before2, after2);
        LogLine(msg);
        return true;
    }

}
