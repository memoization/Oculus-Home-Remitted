#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Worlds.h"
#include "Prefs.h"
#include "HomeLogger.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "json11.hpp"
#include <windows.h>

namespace worlds
{

    namespace fs = std::filesystem;

    static std::string ReadFileUtf8(const std::wstring& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return std::string();
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }


    std::vector<WorldCardInfo> Scan()
    {
        std::vector<WorldCardInfo> out;
        std::error_code ec;
        fs::path dir = fs::path(prefs::AppDir()) / "store" / "worlds";
        if (!fs::is_directory(dir, ec))
        {
            return out;
        }

        std::string defId = prefs::GetDefaultWorldId();

        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_directory()) continue;

            std::wstring folderName = entry.path().filename().wstring();
            if (folderName.rfind(L"world_", 0) != 0) continue;

            std::string worldId = prefs::Narrow(folderName.substr(6));
            if (worldId.empty()) continue;

            std::string text = ReadFileUtf8((entry.path() / "config.json").wstring());
            if (text.empty()) continue;

            std::string err;
            json11::Json cfg = json11::Json::parse(text, err);
            if (!err.empty() || !cfg.is_object()) continue;

            WorldCardInfo info;
            info.worldId = worldId;
            info.name = cfg["name"].string_value();
            info.objectCount = (int)cfg["objects"].array_items().size();
            // UGC traces: an object whose item_definition.__typename is not "WorldsItemDefinition"
            // (WorldsUGCItemDefinition / WorldsUGCPlaceDefinition) references an asset the user uploaded to the Oculus servers. customizations.UGCBase is an indicator of a custom map.
            for (const auto& obj : cfg["objects"].array_items())
            {
                const std::string& tn = obj["item_definition"]["__typename"].string_value();
                if (!tn.empty() && tn == "WorldsUGCItemDefinition")
                {
                    info.ugcObjectCount++;
                }
            }
            std::string ugcBase = cfg["customizations"]["UGCBase"].string_value();
            info.ugcBase = (!ugcBase.empty() && ugcBase != "0");
            info.creationIndex = cfg["creation_index"].int_value();
            info.nameIndex = cfg["name_index"].int_value();
            info.isDefault = (!defId.empty() && defId == worldId);
            info.screenshotPng = prefs::Narrow((entry.path() / "screenshot.png").c_str());
            out.push_back(std::move(info));
        }

        std::sort(out.begin(), out.end(),
                  [](const WorldCardInfo& a, const WorldCardInfo& b)
                  { return a.creationIndex > b.creationIndex; });
        return out;
    }

    static std::string MintWorldId()
    {
        static std::atomic<unsigned long long> salt{0};
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned long long ticks = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        unsigned long long s = salt.fetch_add(1, std::memory_order_relaxed);
        unsigned long long low = (ticks + s * 2654435761ULL) % 1000000000000000ULL; // 10^15
        char buf[32];

        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "8%015llu", low);
        return std::string(buf);
    }

    static bool HasAnyWorldFolder(const fs::path& worldsRoot)
    {
        std::error_code ec;
        if (!fs::is_directory(worldsRoot, ec)) return false;

        for (const auto& e : fs::directory_iterator(worldsRoot, ec))
        {
            if (e.is_directory() && e.path().filename().wstring().rfind(L"world_", 0) == 0)
            {
                return true;
            }
        }
        return false;
    }

    static bool WriteFileAtomic(const std::wstring& path, const std::string& content)
    {
        std::wstring tmp = path + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;

            f.write(content.data(), (std::streamsize)content.size());
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    void SeedDefaultIfEmpty()
    {
        std::error_code ec;
        fs::path appDir = fs::path(prefs::AppDir());
        fs::path worldsRoot = appDir / "store" / "worlds";

        if (HasAnyWorldFolder(worldsRoot)) return;

        // Fallback default world
        json11::Json customizations = json11::Json::object{
            {"CeilingMaterialIndex", std::string("1307410099367935")},
            {"WallsMaterialIndex",   std::string("268649833656140")},
            {"FloorMaterialIndex",   std::string("395295614222659")},
            {"TrimIndex",            std::string("1997259783886885")},
            {"SkyboxIndex",          std::string("1594405190615066")},
            {"MusicIndex",           std::string("134810140648857")},
            {"AmbientSoundIndex",    std::string("2138994519491875")}
        };
        json11::Json objects = json11::Json::array{};

        // Seed the object layout / customizations from "store\templates\default_world.json" if present
        // Only its "objects" array and "customizations" values are used.
        fs::path templatePath = appDir / "store" / "templates" / "default_world.json";
        std::string raw = ReadFileUtf8(templatePath.wstring());
        if (!raw.empty())
        {
            std::string err;
            json11::Json parsed = json11::Json::parse(raw, err);
            if (!err.empty())
            {
                homeLogger.write() << "Worlds: default_world.json parse failed, using empty object list and default customizations. Err - " << err.c_str() << std::endl;
            }
            else
            {
                const json11::Json& node = parsed["data"]["node"];

                if (node["objects"]["nodes"].is_array())
                {
                    objects = node["objects"]["nodes"];
                }

                if (node["customizations"].is_string())
                {
                    std::string cerr;
                    json11::Json cparsed = json11::Json::parse(node["customizations"].string_value(), cerr);
                    if (cerr.empty() && cparsed.is_object())
                    {
                        customizations = cparsed;
                    }
                }
            }
        }

        std::string worldId = MintWorldId();
        json11::Json cfg = json11::Json::object{
            {"world_id", worldId},
            {"name", std::string("")},
            {"creation_index", 0},
            {"name_index", 0},
            {"multiplayer_privacy", std::string("PRIVATE")},
            {"max_mp_guests", 7},
            {"user_locked_edit", false},
            {"auto_capture_enabled", true},
            {"is_liked", false},
            {"like_count", 0},
            {"visible_to_employee_only", false},
            {"guest_users_list", json11::Json::array{}},
            {"invited_users_list", json11::Json::array{}},
            {"cubemap_id", std::string("0")},
            {"customizations", customizations},
            {"objects", objects}
        };

        fs::path folder = worldsRoot / ("world_" + worldId);
        fs::create_directories(folder, ec);

        // Seed screenshot.png directly. Find the source next to the exe first
        fs::path seedPng = appDir / "images" / "world-default.png";
        if (!fs::exists(seedPng, ec))
        {
            seedPng = fs::path("images") / "world-default.png";
        }
            
        if (fs::exists(seedPng, ec))
        {
            CopyFileW(seedPng.wstring().c_str(), (folder / "screenshot.png").wstring().c_str(), FALSE);
        }

        // config.json last, so a half-written folder is never treated as complete.
        if (!WriteFileAtomic((folder / "config.json").wstring(), cfg.dump()))
        {
            homeLogger.write() << "Worlds: failed to write config.json for seeded default world." << std::endl;
            return;
        }

        prefs::SetDefaultWorldId(worldId);
        homeLogger.write() << "Worlds: auto-seeded empty default world " << worldId.c_str() << "." << std::endl;
    }

    void EnsureValidDefault()
    {
        std::error_code ec;
        fs::path worldsRoot = fs::path(prefs::AppDir()) / "store" / "worlds";

        // Nothing left on disk, recreate a new default
        if (!HasAnyWorldFolder(worldsRoot))
        {
            SeedDefaultIfEmpty();
            return;
        }

        // The recorded default still exists
        std::string defId = prefs::GetDefaultWorldId();
        if (!defId.empty() && fs::exists(worldsRoot / ("world_" + defId) / "config.json", ec))
        {
            return;
        }

        // Default is blank or its folder was deleted, fallback to any existing world
        for (const auto& e : fs::directory_iterator(worldsRoot, ec))
        {
            if (!e.is_directory()) continue;

            std::wstring folderName = e.path().filename().wstring();
            if (folderName.rfind(L"world_", 0) != 0) continue;

            std::string text = ReadFileUtf8((e.path() / "config.json").wstring());
            if (text.empty()) continue;

            std::string err;
            json11::Json cfg = json11::Json::parse(text, err);
            if (!err.empty() || !cfg.is_object()) continue;

            std::string worldId = prefs::Narrow(folderName.substr(6));
            prefs::SetDefaultWorldId(worldId);
            homeLogger.write() << "Worlds: default world missing! Falling back to existing world " << worldId.c_str() << "." << std::endl;
            return;
        }
    }

    void PopulateUgcCache()
    {
        std::error_code ec;
        fs::path appDir = fs::path(prefs::AppDir());
        fs::path worldsRoot = appDir / "store" / "worlds";

        if (!fs::is_directory(worldsRoot, ec)) return;

        // Destination cache (the game reads UGC cache here). Empty if LOCALAPPDATA is unavailable, so still sync the global manifest below.
        fs::path cacheDir;
        {
            wchar_t lad[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                cacheDir = fs::path(lad) / "Home2" / "WorldsCache";
                fs::create_directories(cacheDir, ec);
            }
        }

        // App-root global UGC manifest is the single source for mapping the user's UGC inventory.
        // The app grows it by unioning each world's local ugc-hashes.json, which also absorbs a dropped-in shared world folder. The local files stay per-world for that drop-in sharability.
        fs::path globalPath = appDir / "ugc-hashes-global.json";
        json11::Json::object global;
        {
            std::string t = ReadFileUtf8(globalPath.wstring());
            if (!t.empty())
            {
                std::string err;
                json11::Json j = json11::Json::parse(t, err);
                if (err.empty() && j.is_object())
                {
                    global = j.object_items();
                }
            }
        }
        bool globalChanged = false;

        int copied = 0;
        for (const auto& w : fs::directory_iterator(worldsRoot, ec))
        {
            if (!w.is_directory()) continue;
            fs::path ugc = w.path() / "ugc";

            if (!fs::is_directory(ugc, ec)) continue;

            // 1. Copy this world's UGC blobs into WorldsCache
            if (!cacheDir.empty())
            {
                for (const auto& f : fs::directory_iterator(ugc, ec))
                {
                    if (!f.is_regular_file() || f.path().extension() != L".zst") continue;
                    fs::path dst = cacheDir / f.path().filename();
                    
                    if (fs::exists(dst, ec)) continue; // skip existing
                    
                    if (CopyFileW(f.path().wstring().c_str(), dst.wstring().c_str(), TRUE))
                    {
                        ++copied;
                    }
                }
            }

            // 2. Union this world's local ugc-hashes.json into the global for unique def ids only
            std::string mt = ReadFileUtf8((ugc / "ugc-hashes.json").wstring());
            if (!mt.empty())
            {
                std::string err;
                json11::Json m = json11::Json::parse(mt, err);
                if (err.empty() && m.is_object())
                {
                    for (const auto& kv : m.object_items())
                    {
                        if (global.find(kv.first) == global.end())
                        {
                            global[kv.first] = kv.second;
                            globalChanged = true;
                        }
                    }
                }
            }
        }

        if (globalChanged)
        {
            WriteFileAtomic(globalPath.wstring(), json11::Json(global).dump());
        }

        if (copied > 0)
        {
            homeLogger.write() << "Worlds: copied " << copied << " UGC asset(s) into WorldsCache." << std::endl;
        } 
    }

}
