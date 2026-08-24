#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace home2backend {

    // ---------------------------------------------------------------------------
    // Defeat the game-exe OpenSSL 1.0.2 certificate verification in-process
    // so it accepts the loopback leaf (CN=graph.oculus.com signed by the CA).
    //
    // The graph client verifies with SSL_VERIFY_PEER via a custom app_verify_callback,
    // so neither X509_verify_cert nor the CTX-level set_verify hook is consulted at
    // connection time (the persistent SSL_CTX is configured once at SDK init, before
    // the hooks arm). The abort gate in ssl3_get_server_certificate fails the handshake
    // when verify_mode has SSL_VERIFY_PEER set and the cert result i is not positive,
    // so the timing-immune fix is per-connection: clear the verify_mode field on every
    // SSL_connect (fires at handshake time, after the hooks). ssl_st.verify_mode is at
    // offset 0x140 (from SSL_set_verify @ 0x20fe080: mov [rcx+0x140],edx).
    //
    // All RVAs are in the game exe's OpenSSL (imagebase 0x140000000). The hook is
    // scoped to that module only. Every hook self-checks first bytes before patching.
    // ---------------------------------------------------------------------------

    static const uintptr_t kOffVerifyMode      = 0x140;// ssl_st / ssl_ctx_st
    static const uintptr_t kRvaX509VerifyCert  = 0x20ea9f0;
    static const uintptr_t kRvaSslCtxSetVerify = 0x20fc510;
    static const uintptr_t kRvaSslSetVerify    = 0x20fe080;
    static const uintptr_t kRvaSslConnect      = 0x20fc6e0;

    typedef int (*X509VerifyCertFn)(void*);
    typedef void (*SetVerifyFn)(void*, int, void*);
    typedef int (*SslConnectFn)(void*);

    static X509VerifyCertFn GOrigVerify = nullptr;
    static SetVerifyFn GOrigCtxSetVerify = nullptr;
    static SetVerifyFn GOrigSslSetVerify = nullptr;
    static SslConnectFn GOrigSslConnect = nullptr;

    static volatile long GVerifyLogged = 0;
    static volatile long GCtxSetLogged = 0;
    static volatile long GSslSetLogged = 0;
    static volatile long GConnectLogged = 0;

    // X509_verify_cert forced to success, kept as a backstop and not consulted for a custom cb.
    static int DetourVerify(void*)
    {
        if (InterlockedIncrement(&GVerifyLogged) <= 3)
            LogLine("verify: *** X509_verify_cert forced 1 (accept) ***");
        return 1;
    }

    static void DetourCtxSetVerify(void* ctx, int mode, void* cb)
    {
        if (InterlockedIncrement(&GCtxSetLogged) <= 4)
            LogLine("verify: SSL_CTX_set_verify(mode=0x" + HexU(static_cast<unsigned>(mode)) + ") forced 0 (SSL_VERIFY_NONE)");
        GOrigCtxSetVerify(ctx, 0, cb);
    }

    static void DetourSslSetVerify(void* ssl, int mode, void* cb)
    {
        if (InterlockedIncrement(&GSslSetLogged) <= 4)
            LogLine("verify: SSL_set_verify(mode=0x" + HexU(static_cast<unsigned>(mode)) + ") forced 0 (SSL_VERIFY_NONE)");
        GOrigSslSetVerify(ssl, 0, cb);
    }

    // Primary, timing-immune: clear the verify_mode field before the handshake reads it.
    static int DetourSslConnect(void* ssl)
    {
        if (ssl)
            *reinterpret_cast<int*>(reinterpret_cast<char*>(ssl) + kOffVerifyMode) = 0;
        if (InterlockedIncrement(&GConnectLogged) <= 4)
            LogLine("verify: *** SSL_connect zeroed the verify_mode field (SSL_VERIFY_NONE) pre-handshake ***");
        return GOrigSslConnect(ssl);
    }

    static bool TryInstall(uintptr_t base, uintptr_t rva, const unsigned char* expect, size_t expectLen, void* detour, void** orig, const char* name)
    {
        // base plus rva is inside the game module's mapped .text (readable), no SEH needed.
        unsigned char* target = reinterpret_cast<unsigned char*>(base + rva);
        static const char* H = "0123456789abcdef";
        std::string dump;
        for (int i = 0; i < 8; ++i)
        {
            dump += H[(target[i] >> 4) & 0xf];
            dump += H[target[i] & 0xf];
            dump += ' ';
        }

        bool match = std::memcmp(target, expect, expectLen) == 0;
        LogLine(std::string("verify: ") + name + " @ RVA 0x" + HexU(rva) + " first bytes: " + dump + (match ? "(match)" : "(unexpected, not hooking)"));
        
        if (!match) return false;
        
        if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK)
        {
            LogLine(std::string("verify: ") + name + " hook FAILED");
            return false;
        }
        LogLine(std::string("verify: ") + name + " hook installed");
        return true;
    }

    bool InstallVerifyHook()
    {
        HMODULE game = GetModuleHandleW(nullptr);
        wchar_t path[MAX_PATH] = {0};
        GetModuleFileNameW(game, path, MAX_PATH);
        if (std::wstring(path).find(L"Home2-Win64-Shipping") == std::wstring::npos)
        {
            LogLine("verify: host is not Home2-Win64-Shipping.exe, skipping verify hooks");
            return false;
        }
        uintptr_t base = reinterpret_cast<uintptr_t>(game);

        static const unsigned char kX509[] = {0x40, 0x53, 0x57, 0x41, 0x55, 0x41, 0x56};
        static const unsigned char kCtxSet[] = {0x89, 0x91, 0x40, 0x01, 0x00, 0x00};
        static const unsigned char kSslSet[] = {0x89, 0x91, 0x40, 0x01, 0x00, 0x00};
        static const unsigned char kConnect[] = {0x40, 0x53, 0xb8, 0x20, 0x00, 0x00, 0x00};

        // Primary (per-connection, timing-immune).
        bool primary = TryInstall(base, kRvaSslConnect, kConnect, sizeof(kConnect),
                                  reinterpret_cast<void*>(&DetourSslConnect),
                                  reinterpret_cast<void**>(&GOrigSslConnect),
                                  "SSL_connect(verify_mode=0)");
        // Belt-and-suspenders.
        TryInstall(base, kRvaSslSetVerify, kSslSet, sizeof(kSslSet),
                   reinterpret_cast<void*>(&DetourSslSetVerify),
                   reinterpret_cast<void**>(&GOrigSslSetVerify), "SSL_set_verify");
        TryInstall(base, kRvaSslCtxSetVerify, kCtxSet, sizeof(kCtxSet),
                   reinterpret_cast<void*>(&DetourCtxSetVerify),
                   reinterpret_cast<void**>(&GOrigCtxSetVerify), "SSL_CTX_set_verify");
        TryInstall(base, kRvaX509VerifyCert, kX509, sizeof(kX509),
                   reinterpret_cast<void*>(&DetourVerify),
                   reinterpret_cast<void**>(&GOrigVerify), "X509_verify_cert");

        LogLine(std::string("hooks armed (primary SSL_connect ") + (primary ? "OK" : "FAILED") + "; ssl_st.verify_mode offset 0x140). SSL_connect clears verify_mode per connection so the custom app_verify_callback can no longer abort.");
        return primary;
    }

}
