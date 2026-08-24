#include "fetchworlds.h"
#include "imageconv.h"
#include "Prefs.h"
#include "HomeLogger.h"

#include <cpr/cpr.h>
#include "json11.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace fetchworlds
{

    namespace fs = std::filesystem;

    static const char* kGraphUrl        = "https://graph.oculus.com/graphql?forced_locale=en_US";
    static const char* kDocWorldsList   = "2517010291730152"; // user node (full), returns node.worlds.nodes
    static const char* kDocWorldContent = "2021902227865170"; // world content, returns objects and customizations
    static const char* kDocItemDefs     = "2340400929361818"; // item definitions (incl UGC and asset uris)

    static bool WriteFileAtomic(const fs::path& path, const std::string& content)
    {
        fs::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f)
                return false;
            f.write(content.data(), (std::streamsize)content.size());
        }
        std::error_code ec;
        fs::rename(tmp, path, ec); // MSVC std::filesystem::rename replaces an existing target
        if (ec)
        {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    }

    // cubemap_uri is base64(url) on the wire unlike the plain screenshot_uri
    static std::string Base64Decode(const std::string& in)
    {
        static const char* kChars ="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int T[256];
        for (int i = 0; i < 256; ++i) T[i] = -1;
        for (int i = 0; i < 64; ++i) T[(unsigned char)kChars[i]] = i;

        std::string out;
        int val = 0, bits = -8;
        for (unsigned char c : in)
        {
            if (c == '=') break;
            if (T[c] == -1) continue;
            val = (val << 6) + T[c];
            bits += 6;
            if (bits >= 0)
            {
                out.push_back((char)((val >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return out;
    }

    // POST a persisted-query to graph.oculus.com. Returns the parsed top-level JSON and sets err on any failure
    static json11::Json GraphQL(const std::string& token, const std::string& docId, const std::string& variablesJson, std::string& err)
    {
        cpr::Response r = cpr::Post(
            cpr::Url{ kGraphUrl },
            cpr::Payload{ { "access_token", token }, { "doc_id", docId }, { "variables", variablesJson } },
            cpr::Timeout{ 30000 });

        if (r.error)
        {
            err = "network error: " + r.error.message;
            return json11::Json();
        }

        if (r.status_code != 200)
        {
            err = "HTTP " + std::to_string(r.status_code);
            return json11::Json();
        }

        std::string perr;
        json11::Json j = json11::Json::parse(r.text, perr);
        if (!perr.empty())
        {
            err = "could not parse the response";
            return json11::Json();
        }

        if (j["errors"].is_array() && !j["errors"].array_items().empty())
        {
            err = "backend: " + j["errors"][0]["message"].string_value();
            return json11::Json();
        }
        return j;
    }

    static std::string Download(const std::string& url)
    {
        if (url.empty()) return std::string();

        cpr::Response r = cpr::Get(cpr::Url{ url }, cpr::Timeout{ 30000 });
        if (r.error || r.status_code != 200) return std::string();
        return r.text;
    }

    // Map a worlds-list summary node and its content (objects and decoded customizations) to the on-disk config.json the backend reads/serves (same field set as ResponseStore::buildWorldNodeJson and buildCanonicalConfig)
    static json11::Json BuildConfig(const json11::Json& w, const json11::Json& objects, const json11::Json& customizations)
    {
        auto strOr = [](const json11::Json& v, const char* d)
        { return v.is_string() ? v.string_value() : std::string(d); };

        return json11::Json::object{
            { "world_id", w["id"].string_value() },
            { "name", w["name"].string_value() },
            { "creation_index", w["creation_index"].is_number() ? w["creation_index"].int_value() : 0 },
            { "name_index", w["name_index"].is_number() ? w["name_index"].int_value() : 0 },
            { "multiplayer_privacy", strOr(w["multiplayer_privacy"], "PRIVATE") },
            { "max_mp_guests", w["max_mp_guests"].is_number() ? w["max_mp_guests"].int_value() : 7 },
            { "user_locked_edit", w["user_locked_edit"].bool_value() },
            { "auto_capture_enabled", w["auto_capture_enabled"].is_bool() ? w["auto_capture_enabled"].bool_value() : true },
            { "is_liked", w["is_liked"].bool_value() },
            { "like_count", w["like_count"].is_number() ? w["like_count"].int_value() : 0 },
            { "visible_to_employee_only", w["visible_to_employee_only"].bool_value() },
            { "guest_users_list", w["guest_users_list"].is_array() ? w["guest_users_list"] : json11::Json(json11::Json::array{}) },
            { "invited_users_list", w["invited_users_list"].is_array() ? w["invited_users_list"] : json11::Json(json11::Json::array{}) },
            { "cubemap_id", strOr(w["cubemap_id"], "0") },
            { "customizations", customizations.is_object() ? customizations : json11::Json(json11::Json::object{}) },
            { "objects", objects.is_array() ? objects : json11::Json(json11::Json::array{}) }
        };
    }

    // Download a fetched world's UGC assets. UGC objects (item_definition.__typename WorldsUGCItemDefinition / WorldsUGCPlaceDefinition) and customizations.UGCBase reference custom content the user uploaded.
    // Resolve defs via item-definitions, where each carries hash_from_client (the WorldsCache filename) and compressed_zstd_uri (fbcdn .zst).
    // Save the .zst into world_<id>/ugc/<hash>.zst with a minimal ugc-hashes.json manifest (def id maps to {hash_from_client, __typename, asset_key}) and merge it into the global manifest
    // The backend UGC-inventory reads from the global hashes manifest
    static void FetchWorldUgc(const std::string& token, const json11::Json& objects, const json11::Json& customizations, const fs::path& folder)
    {
        std::vector<std::string> ugcIds;
        std::set<std::string> seen;
        for (const auto& obj : objects.array_items())
        {
            const json11::Json& def = obj["item_definition"];
            if (def["__typename"].string_value().rfind("WorldsUGC", 0) == 0)
            {
                std::string id = def["id"].string_value();
                if (!id.empty() && seen.insert(id).second) ugcIds.push_back(id);
            }
        }
        std::string ugcBase = customizations["UGCBase"].string_value();
        if (!ugcBase.empty() && ugcBase != "0" && seen.insert(ugcBase).second)
        {
            ugcIds.push_back(ugcBase);
        } 

        if (ugcIds.empty()) return;

        json11::Json::array idsArr;
        for (const auto& id : ugcIds)
        {
            idsArr.push_back(id);
        }

        std::string err;
        json11::Json resp = GraphQL(token, kDocItemDefs, json11::Json(json11::Json::object{ {"item_def_ids", json11::Json(idsArr)} }).dump(), err);
        if (!err.empty())
        {
            homeLogger.write() << "FetchWorlds: UGC item-defs failed (" << err.c_str() << "); world saved without UGC assets." << std::endl;
            return;
        }

        std::error_code ec;
        fs::path ugcDir = folder / "ugc";
        fs::create_directories(ugcDir, ec);

        json11::Json::object manifest;
        int got = 0;
        for (const auto& node : resp["data"]["nodes"].array_items())
        {
            if (node["__typename"].string_value().rfind("WorldsUGC", 0) != 0) continue; // only UGC defs (the request can echo standard defs too)
            std::string defId = node["id"].string_value();
            std::string hash = node["hash_from_client"].string_value();

            if (defId.empty() || hash.empty()) continue;

            fs::path zst = ugcDir / (hash + ".zst");
            if (!fs::exists(zst, ec))
            {
                std::string bytes = Download(node["compressed_zstd_uri"].string_value());
                if (!bytes.empty())
                {
                    WriteFileAtomic(zst, bytes);
                    ++got;
                }
                else
                {
                    homeLogger.write() << "FetchWorlds: UGC asset download failed for hash " << hash.c_str() << " (def " << defId.c_str() << ")." << std::endl;
                }
            }

            // Manifest entry is the full def node minus only the signed uris, so offline serving has real bounds/flags/etc. that is if the game ever needs more than the hash.
            json11::Json::object m = node.object_items();
            m.erase("glb_uri");
            m.erase("compressed_glb_uri");
            m.erase("compressed_zstd_uri");
            manifest[defId] = json11::Json(m);
        }

        if (!manifest.empty())
        {
            WriteFileAtomic(ugcDir / "ugc-hashes.json", json11::Json(manifest).dump());
        }
        homeLogger.write() << "FetchWorlds: UGC " << manifest.size() << " def(s), " << got << " new asset(s) downloaded." << std::endl;
    }

    Result FetchMyWorlds(std::string token, std::string userId, Progress* progress)
    {
        Result res;
        if (token.empty() || userId.empty())
        {
            res.error = "Both the FRL token and the User ID are required.";
            return res;
        }

        // List the user's own worlds (user node, node.worlds.nodes)
        std::string err;
        json11::Json list = GraphQL(token, kDocWorldsList, "{\"user_id\":\"" + userId + "\"}", err);
        if (!err.empty())
        {
            res.error = "Listing worlds failed: " + err;
            return res;
        }
        json11::Json nodes = list["data"]["node"]["worlds"]["nodes"];
        if (!nodes.is_array())
        {
            res.error = "No worlds found in the response (check the token and user id).";
            return res;
        }

        if (progress)
        {
            progress->total.store((int)nodes.array_items().size());
        }
        
        homeLogger.write() << "FetchWorlds: listing returned " << nodes.array_items().size() << " world(s)." << std::endl;

        fs::path worldsRoot = fs::path(prefs::AppDir()) / "store" / "worlds";
        std::error_code ec;
        fs::create_directories(worldsRoot, ec);

        int saved = 0;
        for (const auto& w : nodes.array_items())
        {
            std::string worldId = w["id"].string_value();
            if (worldId.empty()) continue;

            // World content: objects and decoded customizations
            std::string cerr;
            json11::Json content = GraphQL(token, kDocWorldContent, "{\"world_node_id\":\"" + worldId + "\"}", cerr);
            if (!cerr.empty())
            {
                homeLogger.write() << "FetchWorlds: content fetch failed for " << worldId.c_str() << " (" << cerr.c_str() << "); skipping." << std::endl;
                continue;
            }
            json11::Json objects = content["data"]["node"]["objects"]["nodes"];
            json11::Json customizations = json11::Json::object{};
            {
                std::string custStr = content["data"]["node"]["customizations"].string_value();
                if (!custStr.empty())
                {
                    std::string derr;
                    json11::Json parsed = json11::Json::parse(custStr, derr);

                    if (derr.empty() && parsed.is_object())
                    {
                        customizations = parsed;
                    }
                }
            }

            // save the config.json
            fs::path folder = worldsRoot / ("world_" + worldId);
            fs::create_directories(folder, ec);
            if (!WriteFileAtomic(folder / "config.json", BuildConfig(w, objects, customizations).dump()))
            {
                homeLogger.write() << "FetchWorlds: failed to write config.json for " << worldId.c_str() << "; skipping." << std::endl;
                continue;
            }

            // Media: screenshot (plain url, JPG bytes, then screenshot.png) and cubemap (base64(url), .dds bytes, then cubemap.dds). A missing image is not blocking
            std::string shot = Download(w["screenshot_uri"].string_value());
            if (!shot.empty())
            {
                imageconv::JpegBytesToPngFile(shot, (folder / "screenshot.png").wstring());
            }

            std::string cubeUriB64 = w["cubemap_uri"].string_value();
            if (!cubeUriB64.empty())
            {
                std::string cube = Download(Base64Decode(cubeUriB64));
                if (!cube.empty())
                {
                    WriteFileAtomic(folder / "cubemap.dds", cube);
                }
            }

            // pull any UGC assets this world references into world_<id>/ugc/
            FetchWorldUgc(token, objects, customizations, folder);

            ++saved;
            if (progress)
            {
                progress->done.store(saved);
            }
            
            homeLogger.write() << "FetchWorlds: saved world " << worldId.c_str() << " (" << objects.array_items().size() << " objects)." << std::endl;
        }

        res.ok = true;
        res.worldsSaved = saved;
        if (saved == 0) res.error = "No worlds were downloaded (the account may have none).";
        return res;
    }

}
