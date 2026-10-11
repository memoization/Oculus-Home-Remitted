#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Worlds.h"
#include "Prefs.h"
#include "HomeLogger.h"
#include "Zip.h"

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
        fs::path dir = fs::path(prefs.AppDir()) / "store" / "worlds";
        if (!fs::is_directory(dir, ec))
        {
            return out;
        }

        std::string defId = prefs.GetDefaultWorldId();

        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_directory()) continue;

            std::wstring folderName = entry.path().filename().wstring();
            if (folderName.rfind(L"world_", 0) != 0) continue;

            std::string worldId = prefs.Narrow(folderName.substr(6));
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
            info.screenshotPng = prefs.Narrow((entry.path() / "screenshot.png").c_str());
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
        fs::path appDir = fs::path(prefs.AppDir());
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

        prefs.SetDefaultWorldId(worldId);
        homeLogger.write() << "Worlds: auto-seeded empty default world " << worldId.c_str() << "." << std::endl;
    }

    void EnsureValidDefault()
    {
        std::error_code ec;
        fs::path worldsRoot = fs::path(prefs.AppDir()) / "store" / "worlds";

        // Nothing left on disk, recreate a new default
        if (!HasAnyWorldFolder(worldsRoot))
        {
            SeedDefaultIfEmpty();
            return;
        }

        // The recorded default still exists
        std::string defId = prefs.GetDefaultWorldId();
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

            std::string worldId = prefs.Narrow(folderName.substr(6));
            prefs.SetDefaultWorldId(worldId);
            homeLogger.write() << "Worlds: default world missing! Falling back to existing world " << worldId.c_str() << "." << std::endl;
            return;
        }
    }

    bool RenameWorld(const std::string& worldId, const std::string& newName)
    {
        if (worldId.empty()) return false;

        fs::path cfgPath = fs::path(prefs.AppDir()) / "store" / "worlds" / ("world_" + worldId) / "config.json";

        std::string text = ReadFileUtf8(cfgPath.wstring());
        if (text.empty()) return false;

        std::string err;
        json11::Json cfg = json11::Json::parse(text, err);
        if (!err.empty() || !cfg.is_object()) return false;

        json11::Json::object obj = cfg.object_items();
        obj["name"] = newName;

        if (!WriteFileAtomic(cfgPath.wstring(), json11::Json(obj).dump()))
        {
            homeLogger.write() << "Worlds: failed to write config.json while renaming world " << worldId.c_str() << "." << std::endl;
            return false;
        }

        homeLogger.write() << "Worlds: renamed world " << worldId.c_str() << " to \"" << newName.c_str() << "\"." << std::endl;
        return true;
    }

    void PopulateUgcCache()
    {
        std::error_code ec;
        fs::path appDir = fs::path(prefs.AppDir());
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

        // The global import manifest is the single source for mapping imported UGC inventory. It is located in store\uploaded-ugc alongside the imported blobs.
        // The app adds to it by unioning any world's local ugc-hashes.json, which also absorbs a dropped-in shared world folder. The local files stay per-world for sharability.
        fs::path uploadedDir = appDir / "store" / "uploaded-ugc";
        fs::create_directories(uploadedDir, ec);
        fs::path globalPath = uploadedDir / "import-hashes-global.json";
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

            // Copy this world's UGC blobs into WorldsCache so the game loads them and into store\uploaded-ugc
            for (const auto& f : fs::directory_iterator(ugc, ec))
            {
                if (!f.is_regular_file() || f.path().extension() != L".zst") continue;

                if (!cacheDir.empty())
                {
                    fs::path dst = cacheDir / f.path().filename();
                    if (!fs::exists(dst, ec) && CopyFileW(f.path().wstring().c_str(), dst.wstring().c_str(), TRUE))
                    {
                        ++copied;
                    }
                }

                fs::path up = uploadedDir / f.path().filename();
                if (!fs::exists(up, ec))
                {
                    CopyFileW(f.path().wstring().c_str(), up.wstring().c_str(), TRUE);
                }
            }

            // Union this world's local ugc-hashes.json into the global for unique def ids only
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
                            // Record the migration time as its "created_time"
                            json11::Json::object node = kv.second.is_object() ? kv.second.object_items() : json11::Json::object{};
                            node["created_time"] = static_cast<double>(std::time(nullptr));
                            global[kv.first] = json11::Json(node);
                            globalChanged = true;
                        }
                    }
                }
            }
        }

        // Mirror every uploaded blob into WorldsCache so the game can load any placed UGC
        if (!cacheDir.empty() && fs::is_directory(uploadedDir, ec))
        {
            for (const auto& f : fs::directory_iterator(uploadedDir, ec))
            {
                if (!f.is_regular_file() || f.path().extension() != L".zst") continue;

                fs::path dst = cacheDir / f.path().filename();
                if (!fs::exists(dst, ec) && CopyFileW(f.path().wstring().c_str(), dst.wstring().c_str(), TRUE))
                {
                    ++copied;
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

    static fs::path ImportManifestPath()
    {
        return fs::path(prefs.AppDir()) / "store" / "uploaded-ugc" / "import-hashes-global.json";
    }

    std::vector<ImportInfo> ScanImports()
    {
        std::vector<ImportInfo> out;
        std::error_code ec;
        fs::path manifestPath = ImportManifestPath();
        std::string t = ReadFileUtf8(manifestPath.wstring());
        if (t.empty()) return out;

        std::string err;
        json11::Json j = json11::Json::parse(t, err);
        if (!err.empty() || !j.is_object()) return out;

        fs::path uploadedDir = manifestPath.parent_path();
        for (const auto& kv : j.object_items())
        {
            if (!kv.second.is_object()) continue;

            ImportInfo info;
            info.defId = kv.first;
            info.typeName = kv.second["__typename"].string_value();
            info.name = kv.second["name"].string_value();
            info.hash = kv.second["hash_from_client"].string_value();
            if (kv.second["created_time"].is_number())
            {
                info.createdTime = (unsigned long long)kv.second["created_time"].number_value();
            }
                
            if (!info.hash.empty())
            {
                std::uintmax_t sz = fs::file_size(uploadedDir / (info.hash + ".zst"), ec);
                if (!ec) info.zstBytes = (unsigned long long)sz;
            }
            out.push_back(std::move(info));
        }
        return out;
    }

    bool DeleteImport(const std::string& defId)
    {
        if (defId.empty()) return false;

        std::error_code ec;
        fs::path appDir = fs::path(prefs.AppDir());
        fs::path manifestPath = ImportManifestPath();
        fs::path uploadedDir = manifestPath.parent_path();

        // Resolve the def to its hash and type from the global hashes json catalog.
        json11::Json::object global;
        {
            std::string t = ReadFileUtf8(manifestPath.wstring());
            std::string err;
            json11::Json j = json11::Json::parse(t, err);
            if (err.empty() && j.is_object()) global = j.object_items();
        }
        std::string hash, typeName;
        auto git = global.find(defId);
        if (git != global.end() && git->second.is_object())
        {
            hash = git->second["hash_from_client"].string_value();
            typeName = git->second["__typename"].string_value();
        }

        bool isPlace = (typeName == "WorldsUGCPlaceDefinition");

        // The entry point is a placed WorldsItemDefinition whose position is the player spawn.
        // When a map is removed the world reverts to the default room, so remove the entry point, else the spawn point set from the UGC template can drop the player out of bounds.
        const std::string kEntryPointDefId = "1746732968957267";

        // Purge the def from every world, same as the backend salvage.
        fs::path worldsRoot = appDir / "store" / "worlds";
        if (fs::is_directory(worldsRoot, ec))
        {
            for (const auto& w : fs::directory_iterator(worldsRoot, ec))
            {
                if (!w.is_directory()) continue;

                fs::path cfgPath = w.path() / "config.json";
                std::string ctext = ReadFileUtf8(cfgPath.wstring());
                if (!ctext.empty())
                {
                    std::string cerr;
                    json11::Json cj = json11::Json::parse(ctext, cerr);
                    if (cerr.empty() && cj.is_object())
                    {
                        json11::Json::object cfg = cj.object_items();
                        bool touched = false;

                        // this world uses the map if it places it or references it as the UGCBase customization. Only then is the entry point reset.
                        auto custIt = cfg.find("customizations");
                        bool ugcBaseMatch = isPlace && custIt != cfg.end() && custIt->second.is_object() && custIt->second["UGCBase"].string_value() == defId;
                        bool hasMapObject = false;
                        for (const auto& obj : cfg["objects"].array_items())
                        {
                            if (obj["item_definition"]["id"].string_value() == defId) 
                            {
                                hasMapObject = true;
                                break;
                            }
                        }
                            
                        bool revertMap = isPlace && (ugcBaseMatch || hasMapObject);

                        std::vector<json11::Json> kept;
                        for (const auto& obj : cfg["objects"].array_items())
                        {
                            std::string oid = obj["item_definition"]["id"].string_value();
                            if (oid == defId)// drop the placed instance
                            {
                                touched = true;
                                continue;
                            }

                            if (revertMap && oid == kEntryPointDefId) // reset the spawn with the map
                            {
                                touched = true;
                                continue;
                            }
                            kept.push_back(obj);
                        }
                        if (touched) cfg["objects"] = json11::Json(kept);

                        if (ugcBaseMatch)
                        {
                            json11::Json::object cust = custIt->second.object_items();
                            cust.erase("UGCBase");
                            cfg["customizations"] = json11::Json(cust);
                            touched = true;
                        }

                        if (touched)
                        {
                            WriteFileAtomic(cfgPath.wstring(), json11::Json(cfg).dump());
                        } 
                    }
                }

                // drop the def from the world's own ugc hashes json and delete its blob copy.
                fs::path ugcDir = w.path() / "ugc";
                fs::path pm = ugcDir / "ugc-hashes.json";
                std::string mt = ReadFileUtf8(pm.wstring());
                if (!mt.empty())
                {
                    std::string merr;
                    json11::Json mj = json11::Json::parse(mt, merr);
                    if (merr.empty() && mj.is_object())
                    {
                        json11::Json::object m = mj.object_items();
                        if (m.erase(defId) != 0)
                        {
                            WriteFileAtomic(pm.wstring(), json11::Json(m).dump());
                        }
                    }
                }
                if (!hash.empty()) fs::remove(ugcDir / (hash + ".zst"), ec);
            }
        }

        // delete the blob from WorldsCache and the uploaded store too.
        if (!hash.empty())
        {
            wchar_t lad[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                fs::remove(fs::path(lad) / "Home2" / "WorldsCache" / (hash + ".zst"), ec);
            }
                
            fs::remove(uploadedDir / (hash + ".zst"), ec);
        }

        // drop the def from the global import manifest.
        if (global.erase(defId) != 0)
        {
            WriteFileAtomic(manifestPath.wstring(), json11::Json(global).dump());
        }

        homeLogger.write() << "Worlds: deleted import " << defId.c_str() << " (" << (isPlace ? "map" : "object") << ")." << std::endl;
        return true;
    }

    static std::string ToForwardSlash(const fs::path& rel)
    {
        std::string s = prefs.Narrow(rel.wstring());
        for (auto& c : s)
        {
            if (c == '\\') c = '/';
        }
        return s;
    }

    bool ExportHome(const std::string& worldId, const std::wstring& outPath)
    {
        if (worldId.empty()) return false;

        std::error_code ec;
        fs::path appDir = fs::path(prefs.AppDir());
        fs::path worldFolder = appDir / "store" / "worlds" / ("world_" + worldId);
        if (!fs::is_directory(worldFolder, ec)) return false;

        fs::path cfgPath = worldFolder / "config.json";
        std::string ctext = ReadFileUtf8(cfgPath.wstring());
        if (ctext.empty()) return false;

        std::string cerr;
        json11::Json cfg = json11::Json::parse(ctext, cerr);
        if (!cerr.empty() || !cfg.is_object()) return false;

        // Collect the UGC def ids this home references including its placed UGC objects and any UGC base map.
        std::vector<std::string> ugcDefIds;
        auto addDef = [&](const std::string& id)
        {
            if (id.empty() || id == "0") return;
            if (std::find(ugcDefIds.begin(), ugcDefIds.end(), id) == ugcDefIds.end())
            {
                ugcDefIds.push_back(id);
            }
        };

        for (const auto& obj : cfg["objects"].array_items())
        {
            const std::string& tn = obj["item_definition"]["__typename"].string_value();
            if (tn == "WorldsUGCItemDefinition" || tn == "WorldsUGCPlaceDefinition")
            {
                addDef(obj["item_definition"]["id"].string_value());
            }
        }
        addDef(cfg["customizations"]["UGCBase"].string_value());

        // Load the global import manifest to resolve each def to its hash
        fs::path manifestPath = ImportManifestPath();
        fs::path uploadedDir = manifestPath.parent_path();
        json11::Json::object global;
        {
            std::string t = ReadFileUtf8(manifestPath.wstring());
            std::string err;
            json11::Json j = json11::Json::parse(t, err);

            if (err.empty() && j.is_object())
            {
                global = j.object_items();
            }
        }

        std::string folderName = "world_" + worldId;
        std::vector<zip::FileEntry> entries;

        // Every file in the home folder except its ugc subfolder, which will rebuild from uploaded-ugc so the archive is correct no matter what the ugc folder lacks.
        for (const auto& f : fs::recursive_directory_iterator(worldFolder, ec))
        {
            if (!f.is_regular_file()) continue;

            fs::path rel = fs::relative(f.path(), worldFolder, ec);
            if (ec || rel.empty()) continue;

            std::wstring relw = rel.wstring();
            if (relw.rfind(L"ugc\\", 0) == 0 || relw.rfind(L"ugc/", 0) == 0) continue;
            if (f.path().extension() == L".tmp") continue;

            zip::FileEntry e;
            e.pathInArchive = folderName + "/" + ToForwardSlash(rel);
            e.sourcePath = f.path().wstring();
            entries.push_back(std::move(e));
        }

        // Stage each referenced UGC blob and a new portable ugc-hashes.json. These are sourced from the store's uploaded-ugc
        json11::Json::object stagedManifest;
        for (const auto& defId : ugcDefIds)
        {
            auto it = global.find(defId);
            if (it == global.end() || !it->second.is_object())
            {
                homeLogger.write() << "Worlds: export " << worldId.c_str() << " could not resolve UGC def " << defId.c_str() << ", skipped." << std::endl;
                continue;
            }

            std::string hash = it->second["hash_from_client"].string_value();
            if (hash.empty()) continue;

            fs::path blob = uploadedDir / (hash + ".zst");
            if (fs::exists(blob, ec))
            {
                zip::FileEntry e;
                e.pathInArchive = folderName + "/ugc/" + hash + ".zst";
                e.sourcePath = blob.wstring();
                entries.push_back(std::move(e));
            }
            else
            {
                homeLogger.write() << "Worlds: export " << worldId.c_str() << " missing blob for hash " << hash.c_str() << ", object may not load on import." << std::endl;
            }

            // Keep a portable def node
            json11::Json::object clean = it->second.object_items();
            clean.erase("glb_uri");
            clean.erase("compressed_glb_uri");
            clean.erase("compressed_zstd_uri");
            clean.erase("owned_entry_id");
            clean.erase("created_time");
            stagedManifest[defId] = json11::Json(clean);
        }

        if (!stagedManifest.empty())
        {
            zip::FileEntry e;
            e.pathInArchive = folderName + "/ugc/ugc-hashes.json";
            e.data = json11::Json(stagedManifest).dump();
            entries.push_back(std::move(e));
        }

        if (!zip::CreateArchive(outPath, entries))
        {
            homeLogger.write() << "Worlds: export failed to write archive for world " << worldId.c_str() << "." << std::endl;
            return false;
        }

        homeLogger.write() << "Worlds: exported home " << worldId.c_str() << " with " << stagedManifest.size() << " UGC asset(s)." << std::endl;
        return true;
    }

    bool ImportHome(const std::wstring& inPath)
    {
        std::error_code ec;
        fs::path appDir = fs::path(prefs.AppDir());
        fs::path worldsRoot = appDir / "store" / "worlds";
        fs::create_directories(worldsRoot, ec);

        fs::path staging = worldsRoot / (".import_tmp_" + MintWorldId());
        fs::create_directories(staging, ec);

        if (!zip::ExtractArchive(inPath, staging.wstring()))
        {
            fs::remove_all(staging, ec);
            homeLogger.write() << "Worlds: import failed to extract archive." << std::endl;
            return false;
        }

        // The archive holds a single world_<id> folder.
        fs::path srcWorld;
        for (const auto& e : fs::directory_iterator(staging, ec))
        {
            if (e.is_directory() && e.path().filename().wstring().rfind(L"world_", 0) == 0)
            {
                srcWorld = e.path();
                break;
            }
        }
        if (srcWorld.empty())
        {
            fs::remove_all(staging, ec);
            homeLogger.write() << "Worlds: import archive had no world folder." << std::endl;
            return false;
        }

        // Prefer the id written in config.json but fall back to the folder name.
        fs::path cfgPath = srcWorld / "config.json";
        std::string worldId;
        json11::Json::object cfgObj;
        {
            std::string t = ReadFileUtf8(cfgPath.wstring());
            std::string err;
            json11::Json j = json11::Json::parse(t, err);
            if (err.empty() && j.is_object())
            {
                cfgObj = j.object_items();
                worldId = j["world_id"].string_value();
            }
        }
        if (worldId.empty())
        {
            worldId = prefs.Narrow(srcWorld.filename().wstring().substr(6));
        }

        // The id is provided in the archive, so re-importing the same home overwrites
        fs::path dst = worldsRoot / ("world_" + worldId);
        if (worldId.empty())
        {
            worldId = MintWorldId();
            if (!cfgObj.empty())
            {
                cfgObj["world_id"] = worldId;
                WriteFileAtomic(cfgPath.wstring(), json11::Json(cfgObj).dump());
            }
            dst = worldsRoot / ("world_" + worldId);
        }
        else if (fs::exists(dst, ec))
        {
            fs::remove_all(dst, ec); // replace the existing home that has this id
        }

        // Copy to the store's world folder
        std::error_code mec;
        fs::rename(srcWorld, dst, mec);
        if (mec)
        {
            fs::copy(srcWorld, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
        fs::remove_all(staging, ec);

        // Populate the imported UGC blobs and manifest into WorldsCache, uploaded-ugc, and the global manifest.
        PopulateUgcCache();

        homeLogger.write() << "Worlds: imported home world " << worldId.c_str() << "." << std::endl;
        return true;
    }

}
