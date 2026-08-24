#pragma once
#include <string>
#include <vector>

// preferences.json: the file lives beside the exe and carries app settings. {home2ExePath,profileImagePath}, identity.{userId,oculusId,displayName}, and the
// user_options block which is backend owned. Every write op does load-modify-write so it preserves fields it does not own
namespace prefs
{
    // Directory of the running exe
    std::wstring AppDir();

    std::string GetHome2ExePath();
    void SetHome2ExePath(const std::string& path);

    std::string GetDisplayName();
    void SetDisplayName(const std::string& name);
    std::string GetProfileImagePath();
    void SetProfileImagePath(const std::string& path);

    std::string GetDefaultWorldId();
    void SetDefaultWorldId(const std::string& id);

    // User configured Oculus library roots for the Apps Library page. The default CoreData location is always scanned in addition to these. Stored at frontend.oculusLibraryPaths as array of strings
    std::vector<std::string> GetOculusLibraryPaths();
    void SetOculusLibraryPaths(const std::vector<std::string>& paths);

    // Writes the full settings schema if preferences.json does not yet exist
    void SeedDefaultsIfMissing();

    std::wstring Widen(const std::string& utf8);
    std::string Narrow(const std::wstring& wide);
}
