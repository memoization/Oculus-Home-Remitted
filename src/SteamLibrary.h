#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <filesystem>

namespace fs = std::filesystem;

class SteamLibrary
{
public:
    // Steam first-party apps to exclude
    bool IsExcludedSteamApp(const std::string& appId)
    {
        static const char* const kBlock[] = {
            "250820", // SteamVR
            "228980", // Steamworks Common Redistributables
            "1070560", // Steam Linux Runtime
            "1391110", // Steam Linux Runtime 2.0
            "1628350", // Steam Linux Runtime 3.0
            "1493710", // Proton Experimental
        };
        for (const char* b : kBlock)
        {
            if (appId == b) return true;
        }
        return false;
    }

    // An installed Steam app as read from its appmanifest_*.acf.
    struct SteamInstalled
    {
        std::string appId;
        std::string name;
        fs::path installDir; // <lib>\steamapps\common\<installdir>
        long long acquiredTime; // manifest mtime in unix seconds
    };

    // Classification and cover urls for a Steam app from the storefront or the on disk cache.
    struct SteamDetails
    {
        bool known = false; // a details fetch answer was found
        std::string name; // the store display name used for apps not found in a local manifest
        std::string landscapeUrl;
        std::string headerUrl;
        std::string portraitUrl;
        std::string shot0Url;
        std::string shot1Url;
    };

    // The five resolved cover URIs
    struct SteamCovers
    {
        std::string square, portrait, landscape, shot0, shot1;
    };

    // Parsing a binary KeyValues node. A node carries a string value and an object carries children
    struct KvNode
    {
        std::string name;
        std::string value;
        std::vector<KvNode> children;

        const KvNode* child(const char* key) const // first match and case insensitive (VDF keys are also case insensitive)
        {
            for (const auto& c : children)
            {
                if (_stricmp(c.name.c_str(), key) == 0) return &c;
            }
            return nullptr;
        }
    };

    // Rudimentary VDF/KeyValues reader
    std::vector<std::string> VdfQuotedTokens(const std::string& text);

    // First value following a key token, or "" when absent. VDF stores a key and its value as two adjacent quoted strings
    std::string VdfValue(const std::vector<std::string>& toks, const char* key);

    // Every value following each occurrence of a key used for the many "path" entries in libraryfolders.vdf.
    std::vector<std::string> VdfValues(const std::vector<std::string>& toks, const char* key);

    // Steam install folder path found from the registry key. HKCU SteamPath first, then the HKLM 32 bit "InstallPath".
    std::string SteamRoot();

    // Every Steam library root, each of which holds a steamapps folder. The main install plus the extra drives listed in libraryfolders.vdf
    std::vector<std::string> SteamLibraryRoots(const std::string& steamRoot);

    std::vector<SteamInstalled> EnumerateSteamInstalled(const std::vector<std::string>& roots);

    // Query the Steam storefront for app details data
    SteamDetails FetchSteamDetails(const std::string& appId);

    // Resolve an app's portrait cover url via the storefront IStoreBrowseService/GetItems endpoint
    std::string FetchSteamPortraitUrl(const std::string& appId);

    // Download/build a steam app's covers under the shared image cache and return their store paths
    SteamCovers BuildSteamCovers(const std::string& appId, const SteamDetails& d, const fs::path& imagesDir, std::vector<std::string>& cachedImageFiles, int& failures);

    // Read a null terminated UTF-8 string and advance pos. False in case of overrun.
    bool KvReadCStr(const unsigned char* buf, size_t len, size_t& pos, std::string& out);

    // Parse one object's fields from "buf" until an end marker (0x08 or 0x0B) or EOF. This advances pos past the end marker.
    // Known binary KeyValues type bytes: 0x00 nested object, 0x01 string, 0x02 int32, 0x03 float32, 0x04 pointer, 0x05 widestring, 0x06 color, 0x07 uint64, 0x08 end, 0x0A int64, 0x0B end.
    bool KvParseObject(const unsigned char* buf, size_t len, size_t& pos, std::vector<KvNode>& children, int depth);
    bool KvParseBinary(const std::string& bytes, KvNode& root);

    // A localized value node is either a plain string or an object with language children. This prefers english, otherwise the first language.
    std::string LocalizedValue(const KvNode* n);

    // Locate a Steam user's stats file for an app without resolving the account id by matching UserGameStats_<accountid>_<appid>.bin.
    fs::path FindUserStatsFile(const fs::path& statsDir, const std::string& appId);

    // Make a positive numeric id for a steam achievement. Oculus Home relies on achievements by a numeric id
    std::string SyntheticAchievementId(const std::string& appId, const std::string& bitIndex);
};

extern SteamLibrary steamLib;