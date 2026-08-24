#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

#include <mutex>
#include <string>

// OAF IPC capture
//
// The game's OAF client is Home2\Plugins\OculusWorldPlugin\ThirdParty\OafIpc\x64\
// OafIpc.dll. It talks to OVRServer via a named-pipe "OculusIPC002 Hello" handshake and
// then a shared-memory channel (MapViewOfFile). So instead of the transport, capture at the OafIpc API
// boundary, where the messages are plain UTF8 strings (JSON), using the DLL's own debug
// logging:
//   - OafIpc.dll exports OafIpc_SetEnableDebugLogging(bool), which sets a global flag.
//   - When the flag is set, OafClient emits, via OutputDebugStringA:
//        OafClient::toOaf   "<request json>"      (game to OVRServer)
//        OafClient::toClient "<reply json>"       (OVRServer to game)
//     plus OafClient::Connect ok / failed! etc.
// Enable the flag as soon as OafIpc.dll loads, and hook OutputDebugStringA to record
// every "OafClient" line to a private oaf_ipc.log.
//
// Purpose: capture the full request/reply catalog from a working (dashboard-launch) session,
// so replay the success replies in the offline session by hooking OafIpc_GetReply and substituting the recorded reply for the matching request.

namespace home2backend {

    static HANDLE GOafLog = INVALID_HANDLE_VALUE;
    static std::mutex GOafMutex;
    static long GOafLines = 0;
    static const long kMaxOafLines = 8000;

    static void OafWrite(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(GOafMutex);
        if (GOafLog == INVALID_HANDLE_VALUE || GOafLines > kMaxOafLines)
            return;
        ++GOafLines;
        std::string line = s + "\r\n";
        DWORD w = 0;
        WriteFile(GOafLog, line.data(), static_cast<DWORD>(line.size()), &w, nullptr);
    }

    // OutputDebugStringA capture (the OafClient debug lines) 
    typedef void(WINAPI* OutputDebugStringAFn)(LPCSTR);
    static OutputDebugStringAFn GOrigOutputDebugStringA = nullptr;

    static void WINAPI DetourOutputDebugStringA(LPCSTR text)
    {
        if (text && strstr(text, "OafClient"))
        {
            // Trim a single trailing CR/LF the DLL appends (OafWrite adds its own).
            std::string s(text);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            OafWrite(s);
        }
        if (GOrigOutputDebugStringA)
            GOrigOutputDebugStringA(text);
    }

    //  Enable OafIpc debug logging the instant its DLL is loaded
    // The get_signature / home_ready exchange (sequenceId 0/1) fires immediately after OafIpc_Connect, so a polling waiter loses the race and misses it. Instead enable the debug flag SYNCHRONOUSLY from a LoadLibrary hook, before the plugin's first call.
    typedef bool(*SetEnableDebugLoggingFn)(bool);

    static volatile long GOafLoggingArmed = 0; // 0 = not yet enabled, 1 = enabled

    static void TryEnableOafLogging(const char* via)
    {
        if (GOafLoggingArmed) return;

        HMODULE mod = GetModuleHandleW(L"OafIpc.dll");
        if (!mod)
            return;
        if (InterlockedCompareExchange(&GOafLoggingArmed, 1, 0) != 0)
            return; // another thread won the race
        auto fn = reinterpret_cast<SetEnableDebugLoggingFn>(
            GetProcAddress(mod, "OafIpc_SetEnableDebugLogging"));
        if (fn)
        {
            fn(true);
            OafWrite(std::string("=== OafIpc_SetEnableDebugLogging(true) enabled via ") + via +
                     "; capturing OafClient::toOaf / toClient (incl. get_signature seq 0/1) ===");
            LogLine(std::string("oaf: OafIpc.dll debug logging ENABLED via ") + via);
        }
        else
        {
            OafWrite("=== OafIpc.dll loaded but OafIpc_SetEnableDebugLogging not found ===");
            LogLine("oaf: OafIpc.dll loaded but SetEnableDebugLogging export missing");
        }
        // Arm the reply rewriter in the same breath (this runs after the loader lock is released, so MinHook thread-freezing is safe) so it catches seq 1-3 (home_ready / get_worlds_location / get_demo_settings) before they fire 90ms later.
        InstallOafRewriteHooksNow(mod);
    }

    typedef HMODULE(WINAPI* LoadLibraryWFn)(LPCWSTR);
    typedef HMODULE(WINAPI* LoadLibraryExWFn)(LPCWSTR, HANDLE, DWORD);
    static LoadLibraryWFn GOrigLoadLibraryW = nullptr;
    static LoadLibraryExWFn GOrigLoadLibraryExW = nullptr;

    static HMODULE WINAPI DetourLoadLibraryW(LPCWSTR name)
    {
        HMODULE h = GOrigLoadLibraryW(name);
        TryEnableOafLogging("LoadLibraryW");
        return h;
    }

    static HMODULE WINAPI DetourLoadLibraryExW(LPCWSTR name, HANDLE f, DWORD flags)
    {
        HMODULE h = GOrigLoadLibraryExW(name, f, flags);
        // Skip DONT_RESOLVE/AS_DATAFILE loads (module isn't runnable / logging state N/A).
        if (!(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)))
            TryEnableOafLogging("LoadLibraryExW");
        return h;
    }

    // ANSI variants. the OculusWorldPlugin can load OafIpc.dll via LoadLibraryA/ExA, which the W-only hooks miss then only the slow waiter catches it.
    typedef HMODULE(WINAPI* LoadLibraryAFn)(LPCSTR);
    typedef HMODULE(WINAPI* LoadLibraryExAFn)(LPCSTR, HANDLE, DWORD);
    static LoadLibraryAFn GOrigLoadLibraryA = nullptr;
    static LoadLibraryExAFn GOrigLoadLibraryExA = nullptr;

    static HMODULE WINAPI DetourLoadLibraryA(LPCSTR name)
    {
        HMODULE h = GOrigLoadLibraryA(name);
        TryEnableOafLogging("LoadLibraryA");
        return h;
    }

    static HMODULE WINAPI DetourLoadLibraryExA(LPCSTR name, HANDLE f, DWORD flags)
    {
        HMODULE h = GOrigLoadLibraryExA(name, f, flags);
        if (!(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)))
            TryEnableOafLogging("LoadLibraryExA");
        return h;
    }

    // Derive <Home2 root>\Plugins\OculusWorldPlugin\ThirdParty\OafIpc\x64\OafIpc.dll from the game exe (…\Home2\Binaries\Win64\Home2-Win64-Shipping.exe): strip the exe name, Win64, and Binaries to reach the Home2 root, then append the fixed plugin sub-path.
    static std::wstring DeriveOafIpcPath()
    {
        wchar_t exe[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return L"";

        std::wstring p(exe);
        for (int i = 0; i < 3; ++i)
        {
            size_t slash = p.find_last_of(L"\\/");
            if (slash == std::wstring::npos) return L"";

            p = p.substr(0, slash);
        }
        return p + L"\\Plugins\\OculusWorldPlugin\\ThirdParty\\OafIpc\\x64\\OafIpc.dll";
    }

    // Load OafIpc.dll  arm the OAF hooks before the game's plugin loads it,
    // so home_ready, get_worlds_location, and get_demo_settings can never fire ahead of
    // the rewrite hooks. That timing race leaves get_demo_settings unpatched, the game's
    // DemoModeFetchEvent times out, LoginFailed follows, and it drops to the offline void. These require the replies to be
    // loaded first via InstallOafRewrite. If the DLL isnt at the derived path, simply fall
    // back to the LoadLibrary hooks and waiter, so this is strictly additive.
    void PreloadOafIpc()
    {
        if (GetModuleHandleW(L"OafIpc.dll"))
        {
            TryEnableOafLogging("preload(already-mapped)");
            return;
        }

        std::wstring path = DeriveOafIpcPath();
        if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            LogLine("oaf: preload skipped, OafIpc.dll not found at derived path (" + NarrowUtf8(path) + "), relying on LoadLibrary hooks and waiter");
            return;
        }

        HMODULE mod = LoadLibraryW(path.c_str());
        if (!mod)
        {
            LogLine("oaf: preload LoadLibraryW failed (err " + std::to_string(GetLastError()) + ") for " + NarrowUtf8(path) + ", relying on LoadLibrary hooks and waiter");
            return;
        }

        LogLine("oaf: preloaded OafIpc.dll from " + NarrowUtf8(path) + ", arming OAF hooks now");
        TryEnableOafLogging("preload");
    }

    static DWORD WINAPI OafEnableWaiter(LPVOID)
    {
        // Backup path: if OafIpc.dll was already mapped before the hooks installed, the LoadLibrary hook won't fire for it poll as a fallback.
        for (int i = 0; i < 8000 && !GOafLoggingArmed; ++i)// ~200s max at 25ms
        {
            TryEnableOafLogging("waiter");
            if (GOafLoggingArmed)
                return 0;
            Sleep(25);
        }
        return 0;
    }

    static bool Hook(HMODULE mod, const char* name, void* detour, void** orig)
    {
        void* t = reinterpret_cast<void*>(GetProcAddress(mod, name));
        if (!t)
        {
            LogLine(std::string("oaf: export not found: ") + name);
            return false;
        }
        if (MH_CreateHook(t, detour, orig) != MH_OK || MH_EnableHook(t) != MH_OK)
        {
            LogLine(std::string("oaf: hook failed: ") + name);
            return false;
        }
        return true;
    }

    bool InstallOafCapture(const std::wstring& dir)
    {
        GOafLog = CreateFileW((dir + L"\\oaf_ipc.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        OafWrite("=== OAF IPC capture. PRIVATE: toOaf/toClient carry auth material. ===");

        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        bool ok = false;
        if (k32)
        {
            ok = Hook(k32, "OutputDebugStringA", reinterpret_cast<void*>(&DetourOutputDebugStringA), reinterpret_cast<void**>(&GOrigOutputDebugStringA));
            
            // Enable OafIpc debug logging the instant OafIpc.dll maps (before its first OafIpc_Connect/Send), so catch get_signature/home_ready at seq 0/1.
            Hook(k32, "LoadLibraryW", reinterpret_cast<void*>(&DetourLoadLibraryW),
                 reinterpret_cast<void**>(&GOrigLoadLibraryW));
            Hook(k32, "LoadLibraryExW", reinterpret_cast<void*>(&DetourLoadLibraryExW),
                 reinterpret_cast<void**>(&GOrigLoadLibraryExW));
            Hook(k32, "LoadLibraryA", reinterpret_cast<void*>(&DetourLoadLibraryA),
                 reinterpret_cast<void**>(&GOrigLoadLibraryA));
            Hook(k32, "LoadLibraryExA", reinterpret_cast<void*>(&DetourLoadLibraryExA),
                 reinterpret_cast<void**>(&GOrigLoadLibraryExA));
        }

        HANDLE t = CreateThread(nullptr, 0, OafEnableWaiter, nullptr, 0, nullptr);
        if (t) CloseHandle(t);

        LogLine(std::string("oaf: OAF capture installed (OutputDebugStringA hook ") + (ok ? "ok" : "failed") + ") into oaf_ipc.log, waiting for OafIpc.dll to enable its debug logging. Capturing OafClient::toOaf (requests) and toClient (replies).");
        return ok;
    }

}
