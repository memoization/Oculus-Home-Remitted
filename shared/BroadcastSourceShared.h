#pragma once
#include <cstdint>

namespace shared
{

    inline constexpr wchar_t  kBroadcastSourceObject[]  = L"Local\\OculusHomeRemitted.BroadcastSource";
    inline constexpr uint32_t kBroadcastSourceVersion   = 1;

    enum BroadcastSourceKind : uint32_t {
        BroadcastSourceMonitor = 0, // id = (uint64_t)(uintptr_t)HMONITOR
        BroadcastSourceApp     = 1,// id = (uint64_t)(uintptr_t)HWND
    };

    #pragma pack(push, 8)
    struct BroadcastSourceShared {
        uint32_t version;    // equals kBroadcastSourceVersion, probe ignores the block if this differs
        uint32_t kind;       // BroadcastSourceKind
        uint64_t id;         // Monitor: HMONITOR value. App: HWND value. 0 means unset, the primary monitor
        uint32_t generation; // seqlock: frontend bumps odd, writes, bumps even, probe retries mid-write
        uint32_t reserved;   // must be 0 (pads to 24 bytes, 8-byte aligned)
    };
    #pragma pack(pop)

    static_assert(sizeof(BroadcastSourceShared) == 24, "BroadcastSourceShared must be 24 bytes on both sides");

}
