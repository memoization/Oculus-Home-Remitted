#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>

// Raise the UGC glb upload size limit the game enforces on the client. There are two separate patches and both hardcode values in MB.
//
// glTF asset validator at RVA 0x7C2163. It holds the limit as two hardcoded values
//     0x7C2163  B8 00 00 F0 00       mov eax, 0xf00000      15 MB default limit
//     0x7C2168  BA 00 00 D0 02       mov edx, 0x2d00000     > 45 MB limit used when the asset carries a dds texture
//     0x7C216D  45 84 C0             test r8b, r8b
//     0x7C2170  0F 45 C2             cmovne eax, edx
//     0x7C2173  39 86 B0 03 00 00    cmp [rsi+0x3b0], eax
//     0x7C2179  setl dil
//     0x7C217F  mov byte [rsi+0x3b5], 3
//
// the Zstandard plugin decompress at RVA 0x59FE91 is a check that refuses any zstd whose declared content size is 45 MB or more.
//     0x59FE83  mov rsi, [rbp-9]
//     0x59FE91  48 81 FE 00 00 D0 02  cmp rsi, 0x2d00000   the content size against the 45 MB ceiling
//     0x59FE98  jb ...
//
// maxBytes for every patch is max_ugc_file_size read from the served world_login template, so the adjustment now configurable in the json.

namespace home2backend
{
    static const unsigned long long kStockBytes = 15728640ull; // fallback if template read fails

    // glTF validator with the two hardcoded byte limits.
    static const BYTE kExpect1[10] =
    {
        0xB8, 0x00, 0x00, 0xF0, 0x00, // mov eax, 0xf00000
        0xBA, 0x00, 0x00, 0xD0, 0x02  // mov edx, 0x2d00000
    };
    static const DWORD kRva1 = 0x7C2163;

    // the Zstandard plugin "decompress-too-large"
    static const BYTE kExpect2[7] =
    {
        0x48, 0x81, 0xFE, 0x00, 0x00, 0xD0, 0x02 // cmp rsi, 0x2d00000
    };
    static const DWORD kRva2 = 0x59FE91;

    // Read max_ugc_file_size in bytes from store\templates\world_login.json
    static unsigned long long readMaxUgcFileSize(const std::wstring& selfDir)
    {
        std::wstring path = selfDir + L"\\store\\templates\\world_login.json";

        std::ifstream f(path, std::ios::binary);
        if (!f) return kStockBytes;

        std::ostringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();

        size_t key = text.find("\"max_ugc_file_size\"");
        if (key == std::string::npos) return kStockBytes;

        size_t colon = text.find(':', key);
        if (colon == std::string::npos) return kStockBytes;

        unsigned long long value = strtoull(text.c_str() + colon + 1, nullptr, 10);
        if (value == 0) return kStockBytes;

        return value;
    }

    // Flip a run of code bytes with the page made writable
    static bool writeCode(BYTE* at, const BYTE* bytes, size_t n)
    {
        DWORD old = 0;
        if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        {
            return false;
        }

        memcpy(at, bytes, n);
        VirtualProtect(at, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, n);
        return true;
    }

    // overwrite both hardcoded limit with maxBytes so the dds path and the normal path share the same limit
    static bool patchGLTFValidator(BYTE* base, unsigned long long maxBytes)
    {
        BYTE* sig = base + kRva1;
        if (memcmp(sig, kExpect1, sizeof(kExpect1)) != 0)
        {
            char got[64];
            wsprintfA(got, "%02X %02X %02X %02X %02X", sig[0], sig[1], sig[2], sig[3], sig[4]);
            LogLine(std::string("ugcsize: GLTF validator signature mismatch @exe+0x7C2163 (got ") + got + "), skipping");
            return false;
        }

        BYTE patch[10];
        memcpy(patch, kExpect1, sizeof(kExpect1)); // keeping the two mov opcodes: 0xB8 and 0xBA

        // mov eax, imm32 at patch[0], its immediate is patch[1..4]
        patch[1] = static_cast<BYTE>(maxBytes & 0xFF);
        patch[2] = static_cast<BYTE>((maxBytes >> 8) & 0xFF);
        patch[3] = static_cast<BYTE>((maxBytes >> 16) & 0xFF);
        patch[4] = static_cast<BYTE>((maxBytes >> 24) & 0xFF);

        // mov edx, imm32 at patch[5], its immediate is patch[6..9]
        patch[6] = static_cast<BYTE>(maxBytes & 0xFF);
        patch[7] = static_cast<BYTE>((maxBytes >> 8) & 0xFF);
        patch[8] = static_cast<BYTE>((maxBytes >> 16) & 0xFF);
        patch[9] = static_cast<BYTE>((maxBytes >> 24) & 0xFF);

        if (!writeCode(sig, patch, sizeof(patch)))
        {
            LogLine("ugcsize: GLTF validator VirtualProtect failed, skipping");
            return false;
        }

        LogLine("ugcsize: GLTF validator @exe+0x7C2163 now caps at " + std::to_string(maxBytes) + " bytes");
        return true;
    }

    // Patch the zstd decompress size limit so a larger stored upload can decompress when entering a home
    static bool patchZstd(BYTE* base, unsigned long long maxBytes)
    {
        BYTE* sig = base + kRva2;
        if (memcmp(sig, kExpect2, sizeof(kExpect2)) != 0)
        {
            char got[64];
            wsprintfA(got, "%02X %02X %02X %02X %02X %02X %02X", sig[0], sig[1], sig[2], sig[3], sig[4], sig[5], sig[6]);
            LogLine(std::string("ugcsize: Zstd signature mismatch @exe+0x59FE91 (got ") + got + "), skipping");
            return false;
        }

        BYTE patch[7];
        memcpy(patch, kExpect2, sizeof(kExpect2)); // keep cmp rsi 48 81 FE prefix

        // the imm32 is sign extended to 64 bit and maxBytes stays positive after the 0x7FFFFFFF clamp
        patch[3] = static_cast<BYTE>(maxBytes & 0xFF);
        patch[4] = static_cast<BYTE>((maxBytes >> 8) & 0xFF);
        patch[5] = static_cast<BYTE>((maxBytes >> 16) & 0xFF);
        patch[6] = static_cast<BYTE>((maxBytes >> 24) & 0xFF);

        if (!writeCode(sig, patch, sizeof(patch)))
        {
            LogLine("ugcsize: Zstd VirtualProtect failed, skip");
            return false;
        }

        LogLine("ugcsize: Zstd @exe+0x59FE91 zstd limit now " + std::to_string(maxBytes) + " bytes");
        return true;
    }

    bool InstallUgcSizePatch(const std::wstring& selfDir)
    {
        LogLine("ugcsize: patching UGC size limits");

        BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        if (!base)
        {
            LogLine("ugcsize: no main module handle, skipping");
            return false;
        }

        // The game compares are signed against a 32 bit file size so clamp to a positive signed 32 bit.
        unsigned long long maxBytes = readMaxUgcFileSize(selfDir);
        if (maxBytes > 0x7FFFFFFFull) maxBytes = 0x7FFFFFFFull;

        LogLine("ugcsize: template max_ugc_file_size is " + std::to_string(maxBytes) + " bytes: " + std::to_string(maxBytes / (1024 * 1024)) + " MB");

        bool result1 = patchGLTFValidator(base, maxBytes);
        bool result2 = patchZstd(base, maxBytes);

        return result1 && result2;
    }
}
