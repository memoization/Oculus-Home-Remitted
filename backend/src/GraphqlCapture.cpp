#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

#include <cstdint>
#include <cstring>
#include <string>

// GraphQL / HTTPS plaintext capture
//
// The game talks to graph.oculus.com (gatekeepers, Worlds login, world list, inventory, item
// defs, world-travel) over UE's libcurl and statically linked OpenSSL 1.0.2.
// Capture plaintext at the OpenSSL boundary.
//
// Hook points (static analysis of this exact exe build, shared with SslWatch):
//   SSL_write(SSL* s, const void* buf, int num) @ RVA 0x20fe110  outgoing request, buf holds the bytes on entry
//   SSL_read (SSL* s, void* buf, int num)       @ RVA 0x20fda20  incoming response, buf holds the bytes after the call, length is the return value
//
// Output: appends length-prefixed records to graphql_raw.bin beside the DLL, keyed by the SSL*
// pointer so the offline parser reassembles each keep-alive connection into request/response pairs.
// One record is 'S'(1) dir(1) ssl(8) len(4 little-endian) data(len), written in a single WriteFile
// so records from concurrent threads never interleave.
//
// PRIVATE: graphql_raw.bin can carry the user's live access_token. Keep it private, never share it,
// and this module never logs request or response bodies; only call counts and byte totals.

namespace home2backend {

    // Game OpenSSL 1.0.2 boundary (same RVAs as SslWatch).
    static const uintptr_t kRvaGqlSslWrite = 0x20fe110;
    static const uintptr_t kRvaGqlSslRead  = 0x20fda20;

    typedef int(*GqlSslWriteFn)(void*, const void*, int);
    typedef int(*GqlSslReadFn)(void*, void*, int);

    static GqlSslWriteFn GOrigGqlWrite = nullptr;
    static GqlSslReadFn  GOrigGqlRead  = nullptr;

    static HANDLE GGqlFile = INVALID_HANDLE_VALUE;
    static volatile long   GGqlWrites = 0;
    static volatile long   GGqlReads  = 0;
    static volatile LONG64 GGqlWriteBytes = 0;
    static volatile LONG64 GGqlReadBytes  = 0;

    // dir is 'W' (0x57) for a request or 'R' (0x52) for a response. FILE_APPEND_DATA makes each
    // WriteFile append atomically at EOF, so a single write per record keeps threads from tearing.
    static void GqlWriteRec(unsigned char dir, void* ssl, const void* data, int len)
    {
        if (GGqlFile == INVALID_HANDLE_VALUE || !data || len <= 0) return;

        const int total = 14 + len;
        std::string rec;
        rec.resize(static_cast<size_t>(total));
        char* p = &rec[0];
        p[0] = 0x53; // 'S'
        p[1] = static_cast<char>(dir);
        unsigned long long sp = reinterpret_cast<unsigned long long>(ssl);
        std::memcpy(p + 2, &sp, 8);
        unsigned int ul = static_cast<unsigned int>(len);
        std::memcpy(p + 10, &ul, 4);
        std::memcpy(p + 14, data, static_cast<size_t>(len));

        DWORD written = 0;
        WriteFile(GGqlFile, p, static_cast<DWORD>(total), &written, nullptr);
    }

    static int DetourGqlWrite(void* ssl, const void* buf, int num)
    {
        // Request bytes are in buf on entry, so capture before forwarding.
        if (buf && num > 0)
        {
            GqlWriteRec(0x57, ssl, buf, num);
            InterlockedIncrement(&GGqlWrites);
            InterlockedAdd64(&GGqlWriteBytes, num);
        }
        return GOrigGqlWrite(ssl, buf, num);
    }

    static int DetourGqlRead(void* ssl, void* buf, int num)
    {
        // Response bytes land in buf after the real read returns, and the return value is the length.
        int r = GOrigGqlRead(ssl, buf, num);
        if (buf && r > 0)
        {
            GqlWriteRec(0x52, ssl, buf, r);
            InterlockedIncrement(&GGqlReads);
            InterlockedAdd64(&GGqlReadBytes, r);
        }
        return r;
    }

    // Periodic progress so it's observable that the capture is alive. Counts and byte totals only.
    static DWORD WINAPI GqlProgressThread(LPVOID)
    {
        for (;;)
        {
            Sleep(15000);
            LogLine("graphql-capture: progress SSL_write " + std::to_string(GGqlWrites) +
                    " calls / " + std::to_string(static_cast<long long>(GGqlWriteBytes)) +
                    " B, SSL_read " + std::to_string(GGqlReads) +
                    " calls / " + std::to_string(static_cast<long long>(GGqlReadBytes)) + " B");
        }
    }

    bool InstallGraphqlCapture(const std::wstring& dir)
    {
        HMODULE game = GetModuleHandleW(nullptr);
        wchar_t path[MAX_PATH] = {0};
        GetModuleFileNameW(game, path, MAX_PATH);
        if (std::wstring(path).find(L"Home2-Win64-Shipping") == std::wstring::npos)
        {
            LogLine("graphql-capture: host is not Home2-Win64-Shipping.exe, skipping SSL capture");
            return false;
        }

        std::wstring outPath = dir + L"\\graphql_raw.bin";
        GGqlFile = CreateFileW(outPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (GGqlFile == INVALID_HANDLE_VALUE)
        {
            LogLine("graphql-capture: could not open graphql_raw.bin (err " + std::to_string(GetLastError()) + ")");
            return false;
        }

        uintptr_t base = reinterpret_cast<uintptr_t>(game);
        void* writeAddr = reinterpret_cast<void*>(base + kRvaGqlSslWrite);
        void* readAddr  = reinterpret_cast<void*>(base + kRvaGqlSslRead);
        bool ok = true;

        if (MH_CreateHook(writeAddr, reinterpret_cast<void*>(&DetourGqlWrite), reinterpret_cast<void**>(&GOrigGqlWrite)) != MH_OK || MH_EnableHook(writeAddr) != MH_OK)
        {
            LogLine("graphql-capture: SSL_write hook failed");
            ok = false;
        }

        if (MH_CreateHook(readAddr, reinterpret_cast<void*>(&DetourGqlRead), reinterpret_cast<void**>(&GOrigGqlRead)) != MH_OK || MH_EnableHook(readAddr) != MH_OK)
        {
            LogLine("graphql-capture: SSL_read hook failed");
            ok = false;
        }

        if (ok)
        {
            LogLine("graphql-capture: SSL_write/SSL_read capture installed, appending graphql_raw.bin beside the DLL (carries the access_token, keep private)");
            CreateThread(nullptr, 0, GqlProgressThread, nullptr, 0, nullptr);
        }
        return ok;
    }

}
