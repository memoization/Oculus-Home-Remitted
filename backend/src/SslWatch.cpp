#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace home2backend {

    // Home 2's OpenSSL 1.0.2 boundary
    static const uintptr_t kRvaSslWrite = 0x20fe110;
    static const uintptr_t kRvaSslRead = 0x20fda20;

    typedef int(*SslWriteFn)(void*, const void*, int);
    typedef int(*SslReadFn)(void*, void*, int);

    static SslWriteFn GOrigWrite = nullptr;
    static SslReadFn GOrigRead = nullptr;
    static volatile long GReadLogged = 0;

    static bool LooksLikeRequest(const char* d, int n)
    {
        return (n >= 5 && std::memcmp(d, "POST ", 5) == 0) ||
               (n >= 4 && std::memcmp(d, "GET ", 4) == 0) ||
               (n >= 4 && std::memcmp(d, "PUT ", 4) == 0);
    }

    // If the game's OpenSSL 1.0.2 client SSL_write fires with an HTTP request, its handshake to the loopback listener succeeded, meaning it accepted the self-signed cert, so verify-peer is effectively off
    static int DetourSslWrite(void* ssl, const void* buf, int num)
    {
        if (buf && num > 0)
        {
            const char* d = static_cast<const char*>(buf);
            if (LooksLikeRequest(d, num))
            {
                std::string line = FirstLine(d, num);
                bool graphql = line.find("/graphql") != std::string::npos;
                LogLine(std::string("SSL_write: *** GAME SENT REQUEST") + (graphql ? " (/graphql)" : "") + ": " + line + " ***");
                GGameSentRequestPlain = true;
            }
        }
        return GOrigWrite(ssl, buf, num);
    }

    static int DetourSslRead(void* ssl, void* buf, int num)
    {
        int r = GOrigRead(ssl, buf, num);

        if (r > 0 && InterlockedIncrement(&GReadLogged) <= 3) LogLine("SSL_read: returned " + std::to_string(r) + "B");
        return r;
    }

    bool InstallSslWatch()
    {
        HMODULE game = GetModuleHandleW(nullptr);
        wchar_t path[MAX_PATH] = {0};
        GetModuleFileNameW(game, path, MAX_PATH);
        if (std::wstring(path).find(L"Home2-Win64-Shipping") == std::wstring::npos)
        {
            LogLine("ssl_watch: host is not Home2-Win64-Shipping.exe, skipping SSL hooks");
            return false;
        }

        uintptr_t base = reinterpret_cast<uintptr_t>(game);
        void* writeAddr = reinterpret_cast<void*>(base + kRvaSslWrite);
        void* readAddr = reinterpret_cast<void*>(base + kRvaSslRead);
        bool ok = true;
    
        if (MH_CreateHook(writeAddr, reinterpret_cast<void*>(&DetourSslWrite), reinterpret_cast<void**>(&GOrigWrite)) != MH_OK || MH_EnableHook(writeAddr) != MH_OK)
        {
            LogLine("ssl_watch: SSL_write hook FAILED");
            ok = false;
        }

        if (MH_CreateHook(readAddr, reinterpret_cast<void*>(&DetourSslRead), reinterpret_cast<void**>(&GOrigRead)) != MH_OK || MH_EnableHook(readAddr) != MH_OK)
        {
            LogLine("ssl_watch: SSL_read hook FAILED");
            ok = false;
        }

        if (ok)
        {
            LogLine("ssl_watch: SSL_write/SSL_read log-only hooks installed");
        }
        
        return ok;
    }

}
