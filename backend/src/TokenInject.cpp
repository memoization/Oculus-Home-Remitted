#include "Probe.h"
#include "BackendLogger.h"
#include "ProbePrefs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

// Offline platform-login and access-token synthesis, all named exports of
// LibOVRPlatform64_1.dll, hooked by name.
//
// Fully offline (direct/third-party launch), OVRServer never authenticates the app, so
// the Oculus Platform SDK reports the user as not-logged-in and its async requests never
// complete. UE's FOnlineIdentityOculus::Login then fails "Not currently logged into
// Oculus" and stalls at WaitingPlatformLogin. Spoof the whole login client-side:
//
//   ovr_GetLoggedInUserID()               a real non-zero ovrID (passes the first gate)
//   ovr_Entitlement_GetIsViewerEntitled() record the ovrRequest, synthesize a non-error
//                                           completion (viewer is entitled)
//   ovr_User_GetLoggedInUser()/User_Get() record the ovrRequest, synthesize a completion
//                                           carrying a fake ovrUser (id and name)
//   ovr_User_GetAccessToken()             record, synthesize a dummy-token completion
//
// UE dispatches completions by ovrRequest id, not message type, so we don't need the
// message-type enum constants. The synthesized message just needs the right request id,
// IsError=0, and, for the user, ovr_Message_GetUser and ovr_User_Get* accessors.
//
// Mechanics: each spoofed request is added to GPending keyed by request kind. ovr_PopMessage, when
// the real one returns NULL and something is pending, returns an owned sentinel handle.
// Every ovr_Message_*/ovr_User_* accessor the game calls on it is hooked below.
// In a working dashboard launch the real completions arrive first (PopMessage non-NULL) so the synthesis is a harmless no-op.
namespace home2backend {

    static const unsigned int kMsgUserGetAccessToken = 0x06A85ABEu;

    // Neutral defaults, overridden at init from preferences.json "identity" via SetIdentity.
    // kUserId must be non-zero (0 is the "not logged in" id) and its decimal string must equal the served worlds owner_id. Both derive from identity.userId.
    static const unsigned long long kUserId = 111111111111111ULL;
    static const char kOculusId[] = "Player"; // ovr_User_GetOculusID (username / alias)
    static const char kDisplayName[] = "Player"; // ovr_User_GetDisplayName

    // Live identity used by the spoof accessors (constants above are the fallbacks).
    // Set once at init by SetIdentity, before any hook fires, so plain reads below need no lock.
    static unsigned long long GUserId = kUserId;
    static std::string GOculusId = kOculusId;
    static std::string GDisplayName = kDisplayName;

    void SetIdentity(const Identity& identity)
    {
        if (identity.userId64 != 0)
            GUserId = identity.userId64;
        if (!identity.oculusId.empty())
            GOculusId = identity.oculusId;
        if (!identity.displayName.empty())
            GDisplayName = identity.displayName;
        LogLine("login: identity userId=" + std::to_string(GUserId) + " oculusId=" + GOculusId + " displayName=" + GDisplayName);
    }

    // Placeholder token forwarded as "Bearer" to graph.
    static const char kDummyToken[] = "FRLProbeDummyToken.offline_boot.0000000000000000000000000000000000000000000000";

    enum Kind { KToken, KEntitlement, KUser };

    typedef unsigned long long OvrID;
    typedef unsigned long long OvrRequest;
    typedef void* MsgHandle;
    typedef void* UserHandle;

    typedef OvrID(*GetLoggedInUserIDFn)();
    typedef bool(*IsPlatformInitializedFn)();
    typedef OvrRequest(*ReqVoidFn)();
    typedef OvrRequest(*UserGetFn)(OvrID);
    typedef MsgHandle(*PopMessageFn)();
    typedef unsigned int(*GetTypeFn)(MsgHandle);
    typedef unsigned char(*IsErrorFn)(MsgHandle);
    typedef OvrRequest(*GetRequestIDFn)(MsgHandle);
    typedef const char*(*GetStringFn)(MsgHandle);
    typedef UserHandle(*GetUserFn)(MsgHandle);
    typedef OvrID(*UserGetIDFn)(UserHandle);
    typedef const char*(*UserGetStrFn)(UserHandle);
    typedef void(*FreeMessageFn)(MsgHandle);

    static GetLoggedInUserIDFn GOrigGetLoggedInUserID = nullptr;
    static IsPlatformInitializedFn GOrigIsPlatformInitialized = nullptr;
    static ReqVoidFn GOrigGetAccessToken = nullptr;
    static ReqVoidFn GOrigGetIsViewerEntitled = nullptr;
    static ReqVoidFn GOrigUserGetLoggedInUser = nullptr;
    static UserGetFn GOrigUserGet = nullptr;
    static PopMessageFn GOrigPopMessage = nullptr;
    static GetTypeFn GOrigGetType = nullptr;
    static IsErrorFn GOrigIsError = nullptr;
    static GetRequestIDFn GOrigGetRequestID = nullptr;
    static GetStringFn GOrigGetString = nullptr;
    static GetUserFn GOrigGetUser = nullptr;
    static UserGetIDFn GOrigUserGetID = nullptr;
    static UserGetStrFn GOrigUserGetOculusID = nullptr;
    static UserGetStrFn GOrigUserGetDisplayName = nullptr;
    static FreeMessageFn GOrigFreeMessage = nullptr;

    // A distinct non-null pointer handed back as the ovrUser for the synthesized login.
    // Sized large and zeroed so any accessor we don't explicitly hook reads a zero field (returns null or 0) handing the game a garbage pointer to dereference (access violation).
    static char GFakeUserObj[4096] = {0};
    static UserHandle GFakeUser = GFakeUserObj;

    static std::mutex GMutex;
    static std::unordered_map<OvrRequest, int> GPending; // req to Kind (to synthesize)
    static std::unordered_map<MsgHandle, std::pair<OvrRequest, int>> GFake; // sentinel to req,kind
    static volatile long GLogged = 0;

    static bool IsFake(MsgHandle h, OvrRequest* idOut, int* kindOut)
    {
        std::lock_guard<std::mutex> lock(GMutex);
        auto it = GFake.find(h);
        if (it == GFake.end())
            return false;
        if (idOut)
            *idOut = it->second.first;
        if (kindOut)
            *kindOut = it->second.second;
        return true;
    }

    static void RecordPending(OvrRequest req, int kind, const char* what)
    {
        {
            std::lock_guard<std::mutex> lock(GMutex);
            GPending[req] = kind;
        }
        if (InterlockedIncrement(&GLogged) <= 30)
        {
            LogLine(std::string("login: ") + what + " req=" + std::to_string(req) + " (will synthesize completion)");
        }
    }

    static volatile long GUserIdCalls = 0;

    static OvrID DetourGetLoggedInUserID()
    {
        OvrID real = GOrigGetLoggedInUserID ? GOrigGetLoggedInUserID() : 0;
        long n = InterlockedIncrement(&GUserIdCalls);
        if (n <= 12)
        {
            LogLine("login: ovr_GetLoggedInUserID() call #" + std::to_string(n) + " real=" + std::to_string(real) + (real ? "" : ", spoof " + std::to_string(GUserId)));
        }
            
    
        if (real != 0) return real; // dashboard launch? keep the real id
        return GUserId;  // direct launch: OVRServer said 0, so spoof a valid id
    }

    // Force platform-initialized. The OculusWorlds module appears to early-out "platform login failed" synchronously when the platform isn't initialized, instead of waiting for the async login.
    // Forcing this true should make it wait, so the synthesized login completion lands inside the waiting window.
    // In a dashboard launch, it's already true
    static bool DetourIsPlatformInitialized()
    {
        return true;
    }

    static OvrRequest DetourGetAccessToken()
    {
        OvrRequest req = GOrigGetAccessToken();
        RecordPending(req, KToken, "ovr_User_GetAccessToken");
        return req;
    }

    static OvrRequest DetourGetIsViewerEntitled()
    {
        OvrRequest req = GOrigGetIsViewerEntitled();
        RecordPending(req, KEntitlement, "ovr_Entitlement_GetIsViewerEntitled");
        return req;
    }

    static OvrRequest DetourUserGetLoggedInUser()
    {
        OvrRequest req = GOrigUserGetLoggedInUser();
        RecordPending(req, KUser, "ovr_User_GetLoggedInUser");
        return req;
    }

    static OvrRequest DetourUserGet(OvrID id)
    {
        OvrRequest req = GOrigUserGet(id);
        if (id == GUserId) // only the logged-in-user fetch, not arbitrary users
            RecordPending(req, KUser, "ovr_User_Get(self)");
        return req;
    }

    static MsgHandle DetourPopMessage()
    {
        MsgHandle real = GOrigPopMessage();
        if (real) return real; // real messages pass through untouched

        OvrRequest req = 0;
        int kind = KToken;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(GMutex);
            if (!GPending.empty())
            {
                auto it = GPending.begin();
                req = it->first;
                kind = it->second;
                GPending.erase(it);
                have = true;
            }
        }
        if (!have) return nullptr;
        MsgHandle sentinel = new char[64]();
        {
            std::lock_guard<std::mutex> lock(GMutex);
            GFake[sentinel] = std::make_pair(req, kind);
        }

        if (InterlockedIncrement(&GLogged) <= 30) {
            LogLine("login: synthesized completion for req=" + std::to_string(req) + " kind=" + std::to_string(kind));
        }

        return sentinel;
    }

    static unsigned int DetourGetType(MsgHandle h)
    {
        if (IsFake(h, nullptr, nullptr))
            return kMsgUserGetAccessToken; // routing is by request id, type is unused
        return GOrigGetType(h);
    }

    static unsigned char DetourIsError(MsgHandle h)
    {
        if (IsFake(h, nullptr, nullptr))
            return 0; // token/entitlement/user all "succeed"
        return GOrigIsError(h);
    }

    static OvrRequest DetourGetRequestID(MsgHandle h)
    {
        OvrRequest id = 0;
        if (IsFake(h, &id, nullptr))
            return id;
        return GOrigGetRequestID(h);
    }

    static const char* DetourGetString(MsgHandle h)
    {
        int kind = KToken;
        if (IsFake(h, nullptr, &kind))
            return (kind == KToken) ? kDummyToken : "";
        return GOrigGetString(h);
    }

    static UserHandle DetourMessageGetUser(MsgHandle h)
    {
        int kind = KToken;
        if (IsFake(h, nullptr, &kind))
            return (kind == KUser) ? GFakeUser : nullptr;
        return GOrigGetUser(h);
    }

    static OvrID DetourUserGetID(UserHandle u)
    {
        if (u == GFakeUser)
            return GUserId;
        return GOrigUserGetID(u);
    }

    static const char* DetourUserGetOculusID(UserHandle u)
    {
        if (u == GFakeUser)
            return GOculusId.c_str();
        return GOrigUserGetOculusID(u);
    }

    static const char* DetourUserGetDisplayName(UserHandle u)
    {
        if (u == GFakeUser)
            return GDisplayName.c_str();
        return GOrigUserGetDisplayName(u);
    }

    // Other ovrUser string accessors the world-join / profile path may call on the fake user.
    // While unhooked, they'd read past the dummy object and hand the game a garbage char*
    // to dereference, an access violation in the game exe.
    // Return "" for the fake user.
    #define FAKE_USER_STR(NAME)                                                            \
        static UserGetStrFn GOrig_##NAME = nullptr;                                         \
        static const char* Detour_##NAME(UserHandle u)                                      \
        {                                                                                  \
            if (u == GFakeUser)                                                            \
                return "";                                                                 \
            return GOrig_##NAME(u);                                                        \
        }
    FAKE_USER_STR(ImageUrl)
    FAKE_USER_STR(SmallImageUrl)
    FAKE_USER_STR(InviteToken)
    FAKE_USER_STR(OrgScopedID)
    FAKE_USER_STR(PresenceDeeplinkMessage)
    FAKE_USER_STR(PresenceDestinationApiName)
    FAKE_USER_STR(PresenceLobbySessionId)
    FAKE_USER_STR(PresenceMatchSessionId)
    #undef FAKE_USER_STR

    static void DetourFreeMessage(MsgHandle h)
    {
        bool ours = false;
        {
            std::lock_guard<std::mutex> lock(GMutex);
            auto it = GFake.find(h);
            if (it != GFake.end())
            {
                GFake.erase(it);
                ours = true;
            }
        }
        if (ours)
        {
            delete[] reinterpret_cast<char*>(h);
            return; // never pass the sentinel to the real ovr_FreeMessage
        }
        GOrigFreeMessage(h);
    }

    static bool HookByName(HMODULE mod, const char* name, void* detour, void** orig,
                           bool required = true)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
        if (!target)
        {
            if (required)
                LogLine(std::string("login: export not found: ") + name);
            return false;
        }
        if (MH_CreateHook(target, detour, orig) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            LogLine(std::string("login: hook failed: ") + name);
            return false;
        }
        return true;
    }

    bool InstallTokenHooks()
    {
        HMODULE lib = GetModuleHandleW(L"LibOVRPlatform64_1.dll");
        if (!lib)
        {
            lib = LoadLibraryW(L"LibOVRPlatform64_1.dll");
        }

        if (!lib)
        {
            LogLine("login: LibOVRPlatform64_1.dll not available, login/token hooks skipped");
            return false;
        }
        bool ok = true;
        // Identity and async request functions (record what to synthesize).
        ok &= HookByName(lib, "ovr_GetLoggedInUserID",
                         reinterpret_cast<void*>(&DetourGetLoggedInUserID),
                         reinterpret_cast<void**>(&GOrigGetLoggedInUserID));
        HookByName(lib, "ovr_IsPlatformInitialized",
                   reinterpret_cast<void*>(&DetourIsPlatformInitialized),
                   reinterpret_cast<void**>(&GOrigIsPlatformInitialized), false);
        ok &= HookByName(lib, "ovr_User_GetAccessToken",
                         reinterpret_cast<void*>(&DetourGetAccessToken),
                         reinterpret_cast<void**>(&GOrigGetAccessToken));
        ok &= HookByName(lib, "ovr_Entitlement_GetIsViewerEntitled",
                         reinterpret_cast<void*>(&DetourGetIsViewerEntitled),
                         reinterpret_cast<void**>(&GOrigGetIsViewerEntitled));
        ok &= HookByName(lib, "ovr_User_GetLoggedInUser",
                         reinterpret_cast<void*>(&DetourUserGetLoggedInUser),
                         reinterpret_cast<void**>(&GOrigUserGetLoggedInUser));
        HookByName(lib, "ovr_User_Get", reinterpret_cast<void*>(&DetourUserGet),
                   reinterpret_cast<void**>(&GOrigUserGet), false);
        // Message pump and accessors (synthesize the completions).
        ok &= HookByName(lib, "ovr_PopMessage",
                         reinterpret_cast<void*>(&DetourPopMessage),
                         reinterpret_cast<void**>(&GOrigPopMessage));
        ok &= HookByName(lib, "ovr_Message_GetType",
                         reinterpret_cast<void*>(&DetourGetType),
                         reinterpret_cast<void**>(&GOrigGetType));
        ok &= HookByName(lib, "ovr_Message_IsError",
                         reinterpret_cast<void*>(&DetourIsError),
                         reinterpret_cast<void**>(&GOrigIsError));
        ok &= HookByName(lib, "ovr_Message_GetRequestID",
                         reinterpret_cast<void*>(&DetourGetRequestID),
                         reinterpret_cast<void**>(&GOrigGetRequestID));
        ok &= HookByName(lib, "ovr_Message_GetString",
                         reinterpret_cast<void*>(&DetourGetString),
                         reinterpret_cast<void**>(&GOrigGetString));
        ok &= HookByName(lib, "ovr_Message_GetUser",
                         reinterpret_cast<void*>(&DetourMessageGetUser),
                         reinterpret_cast<void**>(&GOrigGetUser));
        ok &= HookByName(lib, "ovr_User_GetID",
                         reinterpret_cast<void*>(&DetourUserGetID),
                         reinterpret_cast<void**>(&GOrigUserGetID));
        ok &= HookByName(lib, "ovr_User_GetOculusID",
                         reinterpret_cast<void*>(&DetourUserGetOculusID),
                         reinterpret_cast<void**>(&GOrigUserGetOculusID));
        ok &= HookByName(lib, "ovr_User_GetDisplayName",
                         reinterpret_cast<void*>(&DetourUserGetDisplayName),
                         reinterpret_cast<void**>(&GOrigUserGetDisplayName));
        // Harden the fake user against unhooked string accessors (return "" for it).
    #define HOOK_FAKE_USER_STR(NAME)                                                       \
        HookByName(lib, "ovr_User_Get" #NAME, reinterpret_cast<void*>(&Detour_##NAME),     \
                   reinterpret_cast<void**>(&GOrig_##NAME), false)
        HOOK_FAKE_USER_STR(ImageUrl);
        HOOK_FAKE_USER_STR(SmallImageUrl);
        HOOK_FAKE_USER_STR(InviteToken);
        HOOK_FAKE_USER_STR(OrgScopedID);
        HOOK_FAKE_USER_STR(PresenceDeeplinkMessage);
        HOOK_FAKE_USER_STR(PresenceDestinationApiName);
        HOOK_FAKE_USER_STR(PresenceLobbySessionId);
        HOOK_FAKE_USER_STR(PresenceMatchSessionId);
    #undef HOOK_FAKE_USER_STR
        ok &= HookByName(lib, "ovr_FreeMessage", reinterpret_cast<void*>(&DetourFreeMessage), reinterpret_cast<void**>(&GOrigFreeMessage));
    
        LogLine(std::string("login: LibOVRPlatform login+token hooks installed (") +
                (ok ? "all ok" : "some FAILED") +
                "). Spoofing ovr_GetLoggedInUserID + synthesizing entitlement/user/token "
                "completions so FOnlineIdentityOculus passes WaitingPlatformLogin offline.");
        return ok;
    }

}
