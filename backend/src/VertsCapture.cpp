#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

#include <cstdint>
#include <mutex>
#include <string>

// Verts realtime state-capture
//
// The game ships the Verts "state capture" recorder disabled via gatekeeper.
// force the recorder on by setting the flag on the client-options object at connect time, 
// then read the serialized recording blob out of the driver and write it to verts_capture_<n>.bin beside the DLL.
// The blob replays offline through verts_load_state_capture / the StatePlayback driver, so the record layout is unchanged.
// Verts is what likely drives mic and MP player pose states as well as object pose syncing between players. It has not been well documented nor reversed.
//
// All facts below were recovered by static analysis of VertsClient.dll all by name exports:
//   verts_get_default_client_options() returns the 48-byte options object (persistent pointer)
//     opts+0x10 uint8  state_capture_enabled
//     opts+0x14 float  state_capture max length in ms
//     opts+0x18 uint16 abi version (VERTS_POLL_ABI_VERSION is 3)
//   verts_connect_with_options(opts, a1, a2) returns a status, the driver is an out-param
//   verts_driver_get_state_capture(driver) returns a newly-allocated MSVC std::string with the blob
//     str+0x00 char buf[16] (inline data when small) or char* (when capacity is 16 or more)
//     str+0x10 size_t size
//     str+0x18 size_t capacity (16 or more means the data is on the heap, else it is inline)
//   verts_driver_get_disconnect_reason(driver) returns 0 while connected, nonzero on disconnect
//   verts_driver_delete(driver)
//   VERTS_POLL_RECORD_MAGIC is 0x0DA11A50.

namespace home2backend {

    // Client-options field offsets (static analysis of VertsClient.dll).
    static const uintptr_t kOptEnabled = 0x10; // uint8
    static const uintptr_t kOptMaxMs   = 0x14; // float
    static const uintptr_t kOptAbi     = 0x18; // uint16

    static const float          kCaptureMaxMs = 3600000.0f; // retain up to 1 hour of zone history
    static const unsigned short kCaptureAbi   = 3; // VERTS_POLL_ABI_VERSION
    static const unsigned int   kRecordMagic  = 0x0DA11A50u;
    static const size_t         kMaxDumpBytes = 512u * 1024u * 1024u;
    static const DWORD          kDumpIntervalMs = 20000;

    static const uintptr_t kStrSize = 0x10;
    static const uintptr_t kStrCap  = 0x18;

    // One typedef for every hooked/called export
    typedef void* (*VertsFn4)(void*, void*, void*, void*);

    static VertsFn4 GOrigDefaultOpts  = nullptr;
    static VertsFn4 GOrigConnect      = nullptr;
    static VertsFn4 GGetStateCapture  = nullptr;// called directly, not hooked
    static VertsFn4 GOrigDiscReason   = nullptr;
    static VertsFn4 GOrigDriverDelete = nullptr;

    static void* volatile GOpts   = nullptr;
    static void* volatile GDriver = nullptr;
    static std::wstring    GVertsDir;
    static volatile long   GConnectIdx = 0;
    static volatile long   GDiscNoted  = 0;
    static std::mutex      GDumpMutex;

    // Raw field write behind SEH.. no C++ unwinding objects here, so __try is allowed.
    static bool EnableCaptureRaw(void* opts)
    {
        __try
        {
            *reinterpret_cast<unsigned char*>(reinterpret_cast<char*>(opts) + kOptEnabled) = 1;
            *reinterpret_cast<float*>(reinterpret_cast<char*>(opts) + kOptMaxMs) = kCaptureMaxMs;
            *reinterpret_cast<unsigned short*>(reinterpret_cast<char*>(opts) + kOptAbi) = kCaptureAbi;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Read the std::string blob pointer and size behind SEH. Returns false if empty, oversized, or a read faulted. No C++ unwinding objects here so __try is allowed.
    static bool ReadCaptureRaw(void* str, const void** outData, size_t* outSize, unsigned int* outMagic)
    {
        __try
        {
            unsigned long long size = *reinterpret_cast<unsigned long long*>(reinterpret_cast<char*>(str) + kStrSize);
            unsigned long long cap  = *reinterpret_cast<unsigned long long*>(reinterpret_cast<char*>(str) + kStrCap);
            if (size == 0 || size > kMaxDumpBytes)
                return false;
            const void* dataPtr = (cap >= 16) ? *reinterpret_cast<const void* const*>(str) : str;
            *outData = dataPtr;
            *outSize = static_cast<size_t>(size);
            *outMagic = *reinterpret_cast<const unsigned int*>(dataPtr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void WriteBlob(const std::wstring& path, const void* data, size_t size)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            LogLine("verts-capture: could not open " + NarrowUtf8(path));
            return;
        }
        const char* p = static_cast<const char*>(data);
        size_t off = 0;
        while (off < size)
        {
            DWORD chunk = static_cast<DWORD>((size - off > (1u << 20)) ? (1u << 20) : (size - off));
            DWORD w = 0;
            if (!WriteFile(h, p + off, chunk, &w, nullptr) || w == 0) break;

            off += w;
        }
        CloseHandle(h);
    }

    static void DumpCapture(const char* tag)
    {
        void* driver = GDriver;
        if (!driver || !GGetStateCapture) return;

        std::lock_guard<std::mutex> lk(GDumpMutex);
        void* str = GGetStateCapture(driver, nullptr, nullptr, nullptr);
        if (!str) return;

        const void* data = nullptr;
        size_t size = 0;
        unsigned int magic = 0;
        if (!ReadCaptureRaw(str, &data, &size, &magic)) return; // empty or not ready yet

        std::wstring path = GVertsDir + L"\\verts_capture_" + std::to_wstring(static_cast<long>(GConnectIdx)) + L".bin";
        WriteBlob(path, data, size);
        LogLine("verts-capture: dumped " + std::to_string(size) + " bytes to verts_capture_" + std::to_string(static_cast<long>(GConnectIdx)) + ".bin" + (magic == kRecordMagic ? " (valid magic)" : " (unexpected magic)") + " [" + std::string(tag) + "]");
    }

    static void* DetourDefaultOpts(void* a, void* b, void* c, void* d)
    {
        void* r = GOrigDefaultOpts(a, b, c, d);
        GOpts = r; // the persistent options object the game will pass to connect
        return r;
    }

    static void* DetourConnect(void* a, void* b, void* c, void* d)
    {
        // Enable at the last moment on the real options object, falling back to arg0 if it wasn't latched.
        void* opts = GOpts ? GOpts : a;
        bool enabled = EnableCaptureRaw(opts);
        InterlockedIncrement(&GConnectIdx);
        
        LogLine(std::string("verts-capture: connect, state capture ") + (enabled ? "enabled" : "enable faulted") + " on opts");
        return GOrigConnect(a, b, c, d);
    }

    static void* DetourDiscReason(void* a, void* b, void* c, void* d)
    {
        if (!GDriver && a)
        {
            GDriver = a; // arg0 is the driver, polled every frame so it latches quickly
        }

        void* r = GOrigDiscReason(a, b, c, d);
        if (r && InterlockedCompareExchange(&GDiscNoted, 1, 0) == 0)
        {
            LogLine("verts-capture: disconnect detected, final dump");
            DumpCapture("disconnect");
        }
        return r;
    }

    static void* DetourDriverDelete(void* a, void* b, void* c, void* d)
    {
        if (a)
        {
            GDriver = a;
        }
            
        LogLine("verts-capture: driver delete, final dump");
        DumpCapture("pre-delete");
        return GOrigDriverDelete(a, b, c, d);
    }

    // Periodic safety dump on a background thread
    static DWORD WINAPI VertsDumpThread(LPVOID)
    {
        for (;;)
        {
            Sleep(kDumpIntervalMs);
            DumpCapture("interval");
        }
    }

    static bool HookVertsExport(HMODULE mod, const char* name, void* detour, void** orig)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
        if (!target)
        {
            LogLine(std::string("verts-capture: export not found: ") + name);
            return false;
        }

        if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK)
        {
            LogLine(std::string("verts-capture: hook failed: ") + name);
            return false;
        }
        return true;
    }

    static void InstallVertsNow(HMODULE mod)
    {
        GGetStateCapture = reinterpret_cast<VertsFn4>(GetProcAddress(mod, "verts_driver_get_state_capture"));
        if (!GGetStateCapture)
        {
            LogLine("verts-capture: verts_driver_get_state_capture export not found, dumps disabled");
        }

        HookVertsExport(mod, "verts_get_default_client_options", reinterpret_cast<void*>(&DetourDefaultOpts), reinterpret_cast<void**>(&GOrigDefaultOpts));
        HookVertsExport(mod, "verts_connect_with_options", reinterpret_cast<void*>(&DetourConnect), reinterpret_cast<void**>(&GOrigConnect));
        HookVertsExport(mod, "verts_driver_get_disconnect_reason", reinterpret_cast<void*>(&DetourDiscReason), reinterpret_cast<void**>(&GOrigDiscReason));
        HookVertsExport(mod, "verts_driver_delete", reinterpret_cast<void*>(&DetourDriverDelete), reinterpret_cast<void**>(&GOrigDriverDelete));

        CreateThread(nullptr, 0, VertsDumpThread, nullptr, 0, nullptr);
        LogLine("verts-capture: hooks installed on VertsClient.dll, dumping verts_capture_<n>.bin beside the DLL (keep private)");
    }

    // VertsClient.dll loads when the game reaches a realtime zone (entering a world), later than DLL
    // init, so a waiter thread installs the hooks the instant it appears.
    static DWORD WINAPI VertsWaiter(LPVOID)
    {
        for (int i = 0; i < 6000; ++i) // up to ~10 minutes, enough to enter a world
        {
            HMODULE mod = GetModuleHandleW(L"VertsClient.dll");
            if (mod)
            {
                LogLine("verts-capture: VertsClient.dll detected, installing hooks");
                InstallVertsNow(mod);
                return 0;
            }
            Sleep(100);
        }
        LogLine("verts-capture: VertsClient.dll never appeared, capture not installed");
        return 0;
    }

    bool InstallVertsCapture(const std::wstring& dir)
    {
        HMODULE game = GetModuleHandleW(nullptr);
        wchar_t path[MAX_PATH] = {0};
        GetModuleFileNameW(game, path, MAX_PATH);
        if (std::wstring(path).find(L"Home2-Win64-Shipping") == std::wstring::npos)
        {
            LogLine("verts-capture: host is not Home2-Win64-Shipping.exe, skipping");
            return false;
        }

        GVertsDir = dir;
        HANDLE t = CreateThread(nullptr, 0, VertsWaiter, nullptr, 0, nullptr);
        if (t)
        {
            CloseHandle(t);
        }
        return true;
    }

}
