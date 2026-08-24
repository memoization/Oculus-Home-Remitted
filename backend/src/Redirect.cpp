#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "MinHook.h"

#include <cstring>
#include <string>
#include <mutex>
#include <set>

namespace home2backend {

    typedef int(WSAAPI* GetAddrInfoAFn)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
    typedef int(WSAAPI* GetAddrInfoWFn)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
    typedef int(WSAAPI* ConnectFn)(SOCKET, const sockaddr*, int);

    static GetAddrInfoAFn GOrigGetAddrInfoA = nullptr;
    static GetAddrInfoWFn GOrigGetAddrInfoW = nullptr;
    static ConnectFn GOrigConnect = nullptr;

    static const char* kTargetHostA = "graph.oculus.com";
    static const wchar_t* kTargetHostW = L"graph.oculus.com";

    extern "C" __declspec(dllimport) USHORT WINAPI RtlCaptureStackBackTrace(ULONG FramesToSkip, ULONG FramesToCapture, PVOID* BackTrace, PULONG BackTraceHash);

    // Resolve a code address to "module.dll+0xRVA" so it can see which module (game exe vs LibOVRPlatform64_1 vs OculusAppFramework) opens the graph.oculus.com TLS connection, which tells us which OpenSSL stack actually needs the verify hook.
    static std::string ModuleForAddr(void* addr)
    {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &mod) && mod)
        {
            wchar_t path[MAX_PATH] = {0};
            GetModuleFileNameW(mod, path, MAX_PATH);

            std::wstring w(path);
            size_t slash = w.find_last_of(L"\\/");
            std::string name = NarrowUtf8(slash == std::wstring::npos ? w : w.substr(slash + 1));
            uintptr_t off = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);

            return name + "+0x" + HexU(static_cast<unsigned long>(off));
        }
        return "?";
    }

    // Log the call stack that reached a :443 connect, so the caller module is visible.
    static void LogConnectCaller(unsigned long destIp)
    {
        void* frames[16] = {0};
        USHORT n = RtlCaptureStackBackTrace(2, 16, frames, nullptr); // skip detour+capture
        if (n == 0) return;

        // Dedup on the innermost caller module so every distinct module that opens a :443 connection is logged once,
        std::string first = ModuleForAddr(frames[0]);
        std::string modName = first.substr(0, first.find("+0x"));
        {
            static std::mutex m;
            static std::set<std::string> seen;
            std::lock_guard<std::mutex> lk(m);
            if (!seen.insert(modName).second) return;// this module's :443 connect already logged
        }

        std::string trace;
        for (USHORT i = 0; i < n && i < 8; ++i)
        {
            trace += ModuleForAddr(frames[i]) + " <- ";
        }
        
        unsigned char* ip = reinterpret_cast<unsigned char*>(&destIp);
        char dst[32];
        wsprintfA(dst, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        LogLine(std::string("connect: :443 to ") + dst + " new caller-module [" + modName + "]: " + trace);
    }

    static bool IContainsA(const char* haystack, const char* needle)
    {
        if (!haystack) return false;

        std::string h(haystack);
        for (auto& c : h)
        {
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
    
        return h.find(needle) != std::string::npos;
    }

    static int WSAAPI DetourGetAddrInfoA(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result)
    {
        if (IContainsA(node, kTargetHostA))
        {
            LogLine("getaddrinfo: graph.oculus.com to 127.0.0.1");
            return GOrigGetAddrInfoA("127.0.0.1", service, hints, result);
        }
        return GOrigGetAddrInfoA(node, service, hints, result);
    }

    static int WSAAPI DetourGetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* result)
    {
        if (node)
        {
            std::wstring h(node);
            if (h.find(kTargetHostW) != std::wstring::npos)
            {
                LogLine("GetAddrInfoW: graph.oculus.com to 127.0.0.1");
                return GOrigGetAddrInfoW(L"127.0.0.1", service, hints, result);
            }
        }
        return GOrigGetAddrInfoW(node, service, hints, result);
    }

    static int WSAAPI DetourConnect(SOCKET s, const sockaddr* name, int namelen)
    {
        // Backstop: any AF_INET :443 to a non-loopback address is redirected so the graph.oculus.com TLS connection reaches the listener even if resolution was cached / bypassed getaddrinfo.
        if (name && name->sa_family == AF_INET && namelen >= static_cast<int>(sizeof(sockaddr_in)))
        {
            sockaddr_in copy = *reinterpret_cast<const sockaddr_in*>(name);
            if (ntohs(copy.sin_port) == 443)
            {
                LogConnectCaller(copy.sin_addr.s_addr);
            }
            
            if (ntohs(copy.sin_port) == 443 && copy.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            {
                copy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                LogLine("connect: :443 non-loopback redirected to 127.0.0.1 (backstop)");
                return GOrigConnect(s, reinterpret_cast<sockaddr*>(&copy), static_cast<int>(sizeof(copy)));
            }
        }
        return GOrigConnect(s, name, namelen);
    }

    static bool Hook(HMODULE mod, const char* name, void* detour, void** orig)
    {
        if (!mod) return false;

        void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
        if (!target)
        {
            LogLine(std::string("redirect: export not found: ") + name);
            return false;
        }
        if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK)
        {
            LogLine(std::string("redirect: hook failed: ") + name);
            return false;
        }
        return true;
    }

    bool InstallRedirectHooks()
    {
        HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
        if (!ws2)
            ws2 = LoadLibraryW(L"ws2_32.dll");
        bool a = Hook(ws2, "getaddrinfo", reinterpret_cast<void*>(&DetourGetAddrInfoA),
                      reinterpret_cast<void**>(&GOrigGetAddrInfoA));
        bool w = Hook(ws2, "GetAddrInfoW", reinterpret_cast<void*>(&DetourGetAddrInfoW),
                      reinterpret_cast<void**>(&GOrigGetAddrInfoW));
        bool c = Hook(ws2, "connect", reinterpret_cast<void*>(&DetourConnect),
                      reinterpret_cast<void**>(&GOrigConnect));
        LogLine(std::string("redirect: getaddrinfo=") + (a ? "ok" : "NO") + " GetAddrInfoW=" + (w ? "ok" : "NO") + " connect=" + (c ? "ok" : "NO"));
        return a || w || c;
    }

}
