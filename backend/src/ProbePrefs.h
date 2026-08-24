#pragma once
#include <string>

// A read-only view of preferences.json for the backend. Both TokenInject and DllMain read the identity from the preferences.json
// ResponseStore reads the same file itself to avoid a home2backend-to-home2hook include cycle.
namespace home2backend {

struct Identity
{
    std::string userId;      // decimal string, e.g. "111111111111111"
    std::string oculusId;    // ovr_User_GetOculusID (username / alias)
    std::string displayName; // ovr_User_GetDisplayName
    unsigned long long userId64 = 0; // userId parsed as uint64 (0 = unusable)
};

// Reads identity.{userId,oculusId,displayName} from the preferences.json at prefsPath.
// Returns false if the file is missing, unparseable, or has no usable identity
bool LoadIdentity(const std::wstring& prefsPath, Identity& out);

// Feeds identity into TokenInject's spoof accessors
// Empty / zero fields leave the neutral built-in defaults in place.
void SetIdentity(const Identity& identity);

}
