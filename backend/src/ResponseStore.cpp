#include "ResponseStore.h"
#include "ResponseDefinitions.h"
#include "BackendLogger.h"
#include "HttpParse.h" // home2hook::UrlDecode (update_name_world name = b64 of urlencoded)

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // MoveFileExW/CopyFileW (atomic writes of prefs and world config.json/media)

// GDI+ decodes the uploaded screenshot JPEG so WriteWorldMedia writes screenshot.png directly.
// objidl for IStream and objbase for CreateStreamOnHGlobal are stripped by WIN32_LEAN_AND_MEAN.
// gdiplus.h uses unqualified min/max which NOMINMAX removed, so bring them in from std first.
#include <objidl.h>
#include <objbase.h>
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>
using namespace home2backend;
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace home2hook {

    ResponseStore GStore;

    // doc_id values are single-sourced in ResponseDefinitions.h. These are thin file-local aliases so existing call sites keep the familiar kDoc* names
    static const char* kDocWorldLogin      = home2hook::doc::WorldLogin;
    static const char* kDocWorldKeepAlive  = home2hook::doc::WorldKeepAlive;
    static const char* kDocWorldsPoll      = home2hook::doc::WorldsPoll;
    static const char* kDocWorldsList      = home2hook::doc::WorldsList;
    static const char* kDocInventory       = home2hook::doc::Inventory;
    static const char* kDocItemDefs        = home2hook::doc::ItemDefs;
    static const char* kDocWorldContent    = home2hook::doc::WorldContent;
    static const char* kDocDefaultWorld    = home2hook::doc::DefaultWorld;
    static const char* kDocWorldCreate     = home2hook::doc::WorldCreate;
    static const char* kDocSetDefaultWorld = home2hook::doc::SetDefaultWorld;
    static const char* kDocUpdateNameWorld = home2hook::doc::UpdateNameWorld;
    static const char* kDocWorldBatchUpdate= home2hook::doc::WorldBatchUpdate;
    static const char* kDocWorldLikeToggle = home2hook::doc::WorldLikeToggle;
    static const char* kDocWorldDelete     = home2hook::doc::WorldDelete;
    static const char* kDocWorldLockedEdit = home2hook::doc::WorldLockedEdit;
    static const char* kDocSetUserOptions  = home2hook::doc::SetUserOptions;
    static const char* kDocWorldsApps      = home2hook::doc::WorldsApps;
    static const char* kDocWorldsGuestApps = home2hook::doc::WorldsGuestApps;
    static const char* kDocNuxModules      = home2hook::doc::NuxModules;

    // The canonical starter-world content template. Its objects.nodes[] holds roughly 71 nodes and its decoded customizations seed every world_create config.json
    static const char* kCanonicalTemplateStem = "2021902227865170__1000000000000001";

    // Hardcoded canned responses: small, static (or identity / __CMID__ placeholder) doc_id replies
    namespace {
        struct HardcodedTemplate { const char* stem; const char* body; };
        // world templates list involving two doc_ids, same empty payload
        const HardcodedTemplate kHardcodedTemplates[] = {
            {"2470834406377364", R"({"data":{"templates":[]}})"},
            {"2617284248330218", R"({"data":{"templates":[]}})"},
            // my_world_data slices
            {"2297391550372593", R"({"data":{"my_world_data":{"currency_amount":0}}})"},
            {"2478487052219194", R"({"data":{"my_world_data":{"all_announcements":[]}}})"},
            {"2199360326856076",
             R"({"data":{"my_world_data":{"local_ugc_items_endorsed":[]}}})"},
            // default_world_id: empty-store fallback for kDocDefaultWorld (folder-backed default wins)
            {"3313418545373770", R"({"data":{"my_world_data":{"default_world_id":"1000000000000001"}}})"},
            // worlds list: buildWorldsList() serves this dynamically from the locally stored worlds (homes)
            {"2517010291730152", R"({"data":{"node":{"__typename":"User","worlds":{"nodes":[]}}}})"},
            // add_world_visit_history ack (__CMID__ is filled per request)
            {"2793108640763332",
             R"({"data":{"add_world_visit_history":{"client_mutation_id":"__CMID__","visit_time":178410000}}})"},
            // light user node (identity placeholders filled at Load)
            {"2687696671294861",
             R"({"data":{"node":{"__typename":"User","display_name":"__DISPLAY_NAME__","alias":"__OCULUS_ID__"}}})"},
            // friend requests (received/sent)
            {"3344194228954366",
             R"({"data":{"node":{"__typename":"User","friend_requests_received":{"edges":[]},"friend_requests_sent":{"edges":[]}}}})"}, // each edges entry is "{"node":{"id":""}}"
        };
    }

    // A second world_create within this window of the previous real create is the game's spurious duplicate observed roughly 2s behind with a different client_mutation_id, so it is collapsed with no new folder.
    static const long long kCreateCollapseMs = 10000;

    // De-identified identity fallbacks (used when preferences.json is missing/incomplete).
    static const char* kDefaultUserId      = "111111111111111";
    static const char* kDefaultDisplayName = "Player";
    static const char* kDefaultOculusId    = "Player";

    // Scrubbed default user_options served in world_login when preferences.json has no userOptions. Raw JSON, JsonEscape'd into the field at build.
    static const char* kDefaultUserOptions =
        "{\"TeleportDirection\":\"0\",\"ShowPlayArea\":\"true\",\"AutoGrouping\":\"false\","
        "\"SnappingOn\":\"true\",\"volume_music\":\"1.000000\",\"ShowOverlay\":\"false\","
        "\"HandsSharePrefs\":\"true\",\"TranslationSpeed\":\"1.000000\","
        "\"SnapTurnAngle\":\"45.000000\",\"SmoothTurnSpeed\":\"1.000000\","
        "\"LeftMovementMode\":\"Teleport\",\"LeftTurnAroundEnabled\":\"false\","
        "\"LeftTurningMode\":\"SnapTurning\",\"LeftForwardMode\":\"HeadForward\","
        "\"RightMovementMode\":\"Teleport\",\"RightTurnAroundEnabled\":\"false\","
        "\"RightTurningMode\":\"SnapTurning\",\"RightForwardMode\":\"HeadForward\","
        "\"SafetyBubbleEnabled\":\"false\",\"ShowExampleHomes\":\"false\"}";

    // Owned-set inventory nodes carry fixed timestamps, the values from the capture. The game treats these as informational
    static const long long kTimeFirstReceived = 1683508536;
    static const long long kTimeOwnedUpdated  = 1683508535;
    // Served owned_count per inventory node. Large so a placement never drives owned toward 0 and the game's on-reload owned/used reconciliation can't run away into the loading-void hang.
    static const int kOwnedCount = 9999;

    const char* ActionName(ResponseAction action)
    {
        switch (action)
        {
        case ResponseAction::WorldLogin:      return "world_login";
        case ResponseAction::WorldKeepAlive:  return "world_keep_alive";
        case ResponseAction::WorldsPoll:      return "worlds_poll";
        case ResponseAction::WorldsList:      return "worlds_list";
        case ResponseAction::WorldContent:    return "world_content";
        case ResponseAction::DefaultWorld:    return "default_world";
        case ResponseAction::WorldCreate:     return "world_create";
        case ResponseAction::SetDefaultWorld: return "set_default_world";
        case ResponseAction::UpdateNameWorld: return "update_name_world";
        case ResponseAction::WorldBatchUpdate:return "world_batch_update_objects";
        case ResponseAction::WorldLikeToggle: return "world_like_toggle";
        case ResponseAction::WorldDelete:     return "world_delete";
        case ResponseAction::WorldSetLockedEdit: return "world_set_user_locked_edit";
        case ResponseAction::Inventory:       return "inventory";
        case ResponseAction::ItemDefs:        return "item_defs";
        case ResponseAction::WorldsApps:      return "worlds_apps_and_achievements";
        case ResponseAction::WorldsGuestApps: return "worlds_guest_apps_and_achievements";
        case ResponseAction::Canned:          return "canned";
        case ResponseAction::SetUserOptions:  return "set_user_options";
        default:                              return "passthrough";
        }
    }

    static long long NowUnix()
    {
        return static_cast<long long>(::time(nullptr));
    }

    static std::string JsonEscape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size() + 2);
        for (char c : in)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += c;
                }
            }
        }
        return out;
    }

    // Decode standard-alphabet base64 (set_user_options carries user_options as base64(JSON)).
    // Tolerates '=' padding and embedded whitespace, and stops at the first '='.
    static std::string Base64Decode(const std::string& in)
    {
        int lookup[256];
        for (int i = 0; i < 256; ++i)
        {
            lookup[i] = -1;
        }
        
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
        {
            lookup[static_cast<unsigned char>(alphabet[i])] = i;
        }

        std::string out;
        int val = 0;
        int bits = 0;
        for (unsigned char c : in)
        {
            if (c == '=') break;
            int d = lookup[c];
            if (d < 0) continue; // skip whitespace / non-alphabet bytes

            val = (val << 6) | d;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<char>((val >> bits) & 0xFF));
            }
        }
        return out;
    }

    // Standard-alphabet base64 with '=' padding. cubemap_uri is base64-of-the-URL on the wire (unlike screenshot_uri, which is a plain file:// URI)
    static std::string Base64Encode(const std::string& in)
    {
        static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 3 <= in.size(); i += 3)
        {
            unsigned n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
            out.push_back(alphabet[(n >> 18) & 63]);
            out.push_back(alphabet[(n >> 12) & 63]);
            out.push_back(alphabet[(n >> 6) & 63]);
            out.push_back(alphabet[n & 63]);
        }
        if (i < in.size())
        {
            bool two = (i + 1 < in.size());
            unsigned n = static_cast<unsigned char>(in[i]) << 16;
            if (two)
                n |= static_cast<unsigned char>(in[i + 1]) << 8;
            out.push_back(alphabet[(n >> 18) & 63]);
            out.push_back(alphabet[(n >> 12) & 63]);
            out.push_back(two ? alphabet[(n >> 6) & 63] : '=');
            out.push_back('=');
        }
        return out;
    }

    static std::string ReplaceAll(std::string subject, const std::string& from, const std::string& to)
    {
        if (from.empty()) return subject;

        size_t pos = 0;
        while ((pos = subject.find(from, pos)) != std::string::npos)
        {
            subject.replace(pos, from.size(), to);
            pos += to.size();
        }
        return subject;
    }

    static std::string FrameHttp(const std::string& jsonBody)
    {
        // Connection: close deliberately resolves the length-change crux
        std::string response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n";
        response += "Connection: close\r\n";
        response += "\r\n";
        response += jsonBody;
        return response;
    }

    static bool ReadFileText(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;

        std::ostringstream buffer;
        buffer << file.rdbuf();
        out = buffer.str();
        return true;
    }

    // A captured GraphQL body can carry a top-level "errors" block alongside "data".
    // Serving that makes the game treat the whole channel as failed with "Unable to communicate with the Oculus servers" so it goes offline
    static bool StripTopLevelErrors(const std::string& stem, std::string& body)
    {
        std::string err;
        json11::Json parsed = json11::Json::parse(body, err);
        if (!err.empty() || !parsed.is_object()) return false; // not parseable or not an object, so leave as-is

        if (!parsed["errors"].is_array() && !parsed["errors"].is_object()) return false; //no top-level errors, so leave verbatim

        json11::Json::object cleaned = parsed.object_items();
        cleaned.erase("errors");
        body = json11::Json(cleaned).dump();
        LogLine("store: stripped top-level 'errors' from template " + stem);
        return true;
    }

    // Stable 16-digit inventory-entry id derived from the def id.
    // Leading '7' keeps it distinct from real def ids and from minted object/world ids that lead with '8'.
    // This is stable across launches so duplicates of an owned item share one entry id (mirrors real owned_items[].id).
    // Being distinct from the def id is the whole point. Created objects no longer collapse inventory_item.id onto item_definition.id.
    static std::string DeriveInventoryEntryId(const std::string& defId)
    {
        unsigned long long h = 1469598103934665603ULL;
        for (char c : defId)
        {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ULL;
        }
        unsigned long long low = h % 1000000000000000ULL; // 10^15
        char buf[32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "7%015llu", low);
        return std::string(buf);
    }

    bool ResponseStore::Load(const std::wstring& storeDir)
    {
        namespace fs = std::filesystem;
        fs::path root(storeDir);

        // user_options from the preferences.json, a sibling of store\. De-identified fallbacks keep every shipped source neutral.
        // The userId used here is the same field TokenInject spoofs, so owner_id matches string(kUserId).
        identityUserId = kDefaultUserId;
        identityDisplayName = kDefaultDisplayName;
        identityOculusId = kDefaultOculusId;
        userOptionsJson = kDefaultUserOptions;
        prefsPath = (root.parent_path() / "preferences.json").wstring();

        std::string prefsText;
        if (ReadFileText(prefsPath, prefsText))
        {
            std::string perr;
            json11::Json pj = json11::Json::parse(prefsText, perr);
            if (perr.empty() && pj.is_object())
            {
                const json11::Json& id = pj["identity"];
                if (id.is_object())
                {
                    if (id["userId"].is_string() && !id["userId"].string_value().empty())
                        identityUserId = id["userId"].string_value();
                    if (id["displayName"].is_string() && !id["displayName"].string_value().empty())
                        identityDisplayName = id["displayName"].string_value();
                    if (id["oculusId"].is_string() && !id["oculusId"].string_value().empty())
                        identityOculusId = id["oculusId"].string_value();
                }
                if (pj["userOptions"].is_object())
                    userOptionsJson = pj["userOptions"].dump();
                if (pj["defaultWorldId"].is_string())
                    defaultWorldId = pj["defaultWorldId"].string_value();
            }
        }

        // Per-world folder locations, where the portable folder root is storeDir's parent.
        worldsDir = (root / "worlds").wstring();
        appDir = root.parent_path().wstring();

        // Canned .json templates on disk. The friendly-named files below are loaded with fixed keys.
        std::error_code ec;
        fs::path templatesDir = root / "templates";

        std::string wl;
        if (ReadFileText(templatesDir / "world_login.json", wl))
        {
            worldLoginTemplate = wl;
            worldLoginLoaded = wl.find("__SERVER_TIME__") != std::string::npos && wl.find("__CMID__") != std::string::npos;
        }
        else
        {
            LogLine("store: worlds: failed to read the world login json template. Home may fail offline.");
        }

        // Nux tutorials response
        std::string nuxBody;
        if (ReadFileText(templatesDir / "nux_module_definitions.json", nuxBody))
        {
            std::string keyStem = kDocNuxModules;
            StripTopLevelErrors(keyStem, nuxBody);
            cannedTemplates[keyStem] = nuxBody;
            cannedDocIds.insert(keyStem);
        }
        else
        {
            LogLine("store: nux: failed to read nux modules json template. Tutorials will not appear.");
        }

        // Starter-world content in default_world.json, formerly the doc-id-named world-content template.
        // It is registered under the stem the game asks for (kDocWorldContent, then "__", then the default world id) so buildCanned's world-content fallback serves it when the folder store is empty.
        std::string dwBody;
        if (ReadFileText(templatesDir / "default_world.json", dwBody))
        {
            std::string keyStem = kCanonicalTemplateStem;
            StripTopLevelErrors(keyStem, dwBody);
            cannedTemplates[kCanonicalTemplateStem] = dwBody;
            cannedDocIds.insert(kDocWorldContent);
        }
        else
        {
            LogLine("store: worlds: failed to read the default world seed json template. World loading fallbacks might fail.");
        }

        // Bake in the hardcoded canned templates
        for (const auto& t : kHardcodedTemplates)
        {
            cannedTemplates[t.stem] = t.body;
            std::string stem = t.stem;
            size_t sep = stem.find("__");
            cannedDocIds.insert(sep != std::string::npos ? stem.substr(0, sep) : stem);
        }

        // Fill the identity placeholders in every canned template once per launch. This is correct for the "applies on next launch" semantics and has zero per-request cost.
        std::string ownerIdEsc = JsonEscape(identityUserId);
        std::string displayNameEsc = JsonEscape(identityDisplayName);
        std::string oculusIdEsc = JsonEscape(identityOculusId);
        for (auto& kv : cannedTemplates)
        {
            kv.second = ReplaceAll(kv.second, "__OWNER_ID__", ownerIdEsc);
            kv.second = ReplaceAll(kv.second, "__DISPLAY_NAME__", displayNameEsc);
            kv.second = ReplaceAll(kv.second, "__OCULUS_ID__", oculusIdEsc);
        }

        std::string masterText;
        if (ReadFileText(root / "item-definitions.master.json", masterText))
        {
            std::string err;
            masterDb = json11::Json::parse(masterText, err);
            masterLoaded = err.empty() && masterDb.is_object();
        }

        std::string ownedText;
        if (ReadFileText(root / "inventory-owned.json", ownedText))
        {
            std::string err;
            json11::Json parsed = json11::Json::parse(ownedText, err);
            if (err.empty() && parsed["owned"].is_array())
            {
                ownedItems = parsed["owned"];
                ownedLoaded = true;
            }
        }

        // Optional: the user's local Oculus app library. When present it lets the "worlds_apps_and_achievements" handlers reconstruct real titles and thumbnails for placed GameBox, cartridge, and achievement tiles.
        // When absent the handlers serve the empty form so tiles render blank without a crash. Accept either a bare {"apps":[...]} object or an [...] array.
        std::string appLibText;
        if (ReadFileText(root / "apps-library.json", appLibText))
        {
            std::string err;
            json11::Json parsed = json11::Json::parse(appLibText, err);
            if (err.empty())
            {
                if (parsed.is_array())
                    appLibrary = json11::Json(json11::Json::object{ { "apps", parsed } });
                else if (parsed.is_object() && parsed["apps"].is_array())
                    appLibrary = parsed;
                
                appLibraryLoaded = appLibrary["apps"].is_array() && !appLibrary["apps"].array_items().empty();
            }
        }

        // Load the user's uploaded-UGC catalog once. Seeded into ugcDefs each loadWorlds so item-defs resolve every UGC def even if a world's own manifest is incomplete.
        // Placed ownership for the inventory comes from each world's actual objects "augmentInventoryFromWorldUgc" and not from this catalog.
        std::string gtext;
        if (ReadFileText(root.parent_path() / "ugc-hashes-global.json", gtext))
        {
            std::string gerr;
            json11::Json gj = json11::Json::parse(gtext, gerr);
            if (gerr.empty() && gj.is_object())
            {
                globalUgcManifest = gj;
            }
        }

        loadWorlds();

        // Make each world's placed UGC objects owned in the offline inventory. The game gates editing a UGC object on finding the object's inventory_item.id among "owned_items".
        // Without it the entity is read-only and the game never sends a customization update.
        // Emit an owned node per world UGC object carrying its real inventory_item.id, distinct from the def id so there is no owned/used reconciliation loading void. Then rebuild the entry-id to def-id reverse map.
        augmentInventoryFromWorldUgc();
        rebuildInventoryReverseMap();

        LogLine("store: world_login=" + std::string(worldLoginLoaded ? "yes" : "no") +
                " canned_templates=" + std::to_string(cannedTemplates.size()) +
                " master_db=" + std::string(masterLoaded ? "yes" : "no") +
                " owned=" + std::string(ownedLoaded ? "yes" : "no") +
                " app_library=" + (appLibraryLoaded ? std::to_string(appLibrary["apps"].array_items().size()) : std::string("0 (blank tiles)")) +
                " worlds=" + (worldsLoaded ? std::to_string(worlds.size()) : std::string("0 (static fallback)")));
        
        return worldLoginLoaded;
    }

    // Scan store\worlds\world_<id>\config.json into the WorldEntry vector. worldsLoaded stays false on an empty/missing dir so the static worlds-list/content/default templates answer
    void ResponseStore::loadWorlds()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        worlds.clear();
        ugcDefs.clear();
        ugcZstUri.clear();
        worldsLoaded = false;

        // Seed ugcDefs from the global UGC manifest so item-defs resolve every advertised UGC def, not only those present in a local world folder.
        // Per-world loadWorldUgc below adds the file:// .zst uris and any richer node on top.
        // A global-only def with no local .zst still resolves, since the game loads its blob from WorldsCache by hash, so it counts toward the Uploaded gate.
        if (globalUgcManifest.is_object())
        {
            for (const auto& kv : globalUgcManifest.object_items())
            {
                if (!kv.first.empty() && kv.second.is_object())
                    ugcDefs[kv.first] = kv.second;
            }

        }

        fs::path dir(worldsDir);
        if (!fs::is_directory(dir, ec))
        {
            // Create it so the empty-store self-seed below has somewhere to write.
            fs::create_directories(dir, ec);
        }

        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_directory()) continue;

            std::string folderName = entry.path().filename().string();
            if (folderName.rfind("world_", 0) != 0) continue;

            std::string worldId = folderName.substr(6);
            if (worldId.empty()) continue;

            std::string text;
            if (!ReadFileText(entry.path() / "config.json", text))
            {
                LogLine("store: worlds: folder " + folderName + " has no config.json, skipping");
                continue;
            }
            std::string err;
            json11::Json cfg = json11::Json::parse(text, err);
            if (!err.empty() || !cfg.is_object())
            {
                LogLine("store: worlds: folder " + folderName + " config.json invalid, skipping");
                continue;
            }

            WorldEntry e;
            e.worldId = worldId;
            e.folder = entry.path().wstring();
            e.name = cfg["name"].string_value();
            e.config = cfg;
            worlds.push_back(std::move(e));

            loadWorldUgc(entry.path().wstring());
        }

        if (!worlds.empty())
        {
            worldsLoaded = true;
            LogLine("store: loaded " + std::to_string(worlds.size()) + " world folder(s), default=" + (defaultWorldId.empty() ? std::string("first") : defaultWorldId));
            return;
        }

        // Empty store\worlds: seed a folder-backed blank default world so in-VR object edits persist.
        // Without a folder-backed WorldEntry, world_batch_update_objects has nowhere to write.
        // The frontend auto-seed only covers frontend startup. This covers the store becoming empty afterwards.
        std::string worldId = mintWorldId();
        json11::Json cfg = buildCanonicalConfig(worldId, 0, 0);
        fs::path folder = dir / ("world_" + worldId);
        fs::create_directories(folder, ec);
        fs::path seedPng = fs::path(appDir) / "images" / "world-default.png";
        if (fs::exists(seedPng, ec))
        {
            CopyFileW(seedPng.wstring().c_str(), (folder / "screenshot.png").wstring().c_str(), FALSE);
        }

        if (writeFileAtomic((folder / "config.json").wstring(), cfg.dump()))
        {
            WorldEntry e;
            e.worldId = worldId;
            e.folder = folder.wstring();
            e.name = cfg["name"].string_value();
            e.config = cfg;
            worlds.push_back(std::move(e));
            worldsLoaded = true;

            if (defaultWorldId.empty())
            {
                defaultWorldId = worldId;
            }

            LogLine("store: worlds dir empty: seeded folder-backed blank default world " + worldId);
        }
        else
        {
            LogLine("store: worlds dir empty: blank-world seed failed, serving static templates");
        }
    }

    ResponseAction ResponseStore::Classify(const std::string& docId) const
    {
        if (docId == kDocWorldLogin)
            return worldLoginLoaded ? ResponseAction::WorldLogin : ResponseAction::PassThrough;
        if (docId == kDocWorldKeepAlive)
            return ResponseAction::WorldKeepAlive;
        if (docId == kDocWorldsPoll)
            return ResponseAction::WorldsPoll;
        if (docId == kDocWorldsList)
            return worldsLoaded ? ResponseAction::WorldsList : (cannedDocIds.count(docId) ? ResponseAction::Canned : ResponseAction::PassThrough);
        if (docId == kDocWorldContent)
            return worldsLoaded ? ResponseAction::WorldContent : (cannedDocIds.count(docId) ? ResponseAction::Canned : ResponseAction::PassThrough);
        if (docId == kDocDefaultWorld)
            return worldsLoaded ? ResponseAction::DefaultWorld : (cannedDocIds.count(docId) ? ResponseAction::Canned : ResponseAction::PassThrough);
        if (docId == kDocWorldCreate)
            return ResponseAction::WorldCreate;
        if (docId == kDocSetDefaultWorld)
            return ResponseAction::SetDefaultWorld;
        if (docId == kDocUpdateNameWorld)
            return ResponseAction::UpdateNameWorld;
        if (docId == kDocWorldBatchUpdate)
            return ResponseAction::WorldBatchUpdate;
        if (docId == kDocWorldLikeToggle)
            return ResponseAction::WorldLikeToggle;
        if (docId == kDocWorldDelete)
            return ResponseAction::WorldDelete;
        if (docId == kDocWorldLockedEdit)
            return ResponseAction::WorldSetLockedEdit;
        if (docId == kDocInventory)
            return ownedLoaded ? ResponseAction::Inventory : ResponseAction::PassThrough;
        if (docId == kDocItemDefs)
            return masterLoaded ? ResponseAction::ItemDefs : ResponseAction::PassThrough;
        if (docId == kDocWorldsApps)
            return ResponseAction::WorldsApps;
        if (docId == kDocWorldsGuestApps)
            return ResponseAction::WorldsGuestApps;
        if (docId == kDocSetUserOptions)
            return ResponseAction::SetUserOptions;
        if (cannedDocIds.count(docId) != 0)
            return ResponseAction::Canned;
        
        return ResponseAction::PassThrough;
    }

    std::string ResponseStore::buildWorldLogin(const std::string& clientMutationId) const
    {
        if (!worldLoginLoaded) return std::string();
        std::string body = ReplaceAll(worldLoginTemplate, "__SERVER_TIME__",
                                      std::to_string(NowUnix()));
        body = ReplaceAll(body, "__CMID__", JsonEscape(clientMutationId));
        // Serve user_options as a raw escaped JSON string
        std::string opts;
        {
            std::lock_guard<std::mutex> lock(userOptionsMutex);
            opts = userOptionsJson;
        }
        body = ReplaceAll(body, "__USER_OPTIONS__", JsonEscape(opts));
        return body;
    }

    std::string ResponseStore::buildKeepAlive(const std::string& clientMutationId) const
    {
        std::string body = "{\"data\":{\"world_keep_alive\":{\"events\":[],"
                           "\"client_mutation_id\":\"";
        body += JsonEscape(clientMutationId);
        body += "\",\"server_time\":";
        body += std::to_string(NowUnix());
        body += "}}}";
        return body;
    }

    // A generic World node for a requested id that has no folder. Shared by the !worldsLoaded path and worlds-poll's not-found edge case.
    static json11::Json GenericWorldNode(const std::string& id, const std::string& ownerId, const std::string& ownerName)
    {
        return json11::Json::object{
            {"__typename", std::string("World")},
            {"id", id},
            {"name", std::string("Home")},
            {"owner_id", ownerId},
            {"cubemap_uri", std::string("")},
            {"cubemap_id", std::string("0")},
            {"screenshot_uri", std::string("")},
            {"owner_name", ownerName},
            {"is_liked", false},
            {"visible_to_employee_only", false},
            {"like_count", 0},
            {"creation_index", 0},
            {"name_index", 0},
            {"multiplayer_privacy", std::string("PRIVATE")},
            {"guest_users_list", json11::Json::array{}},
            {"max_mp_guests", 7},
            {"user_locked_edit", false},
            {"auto_capture_enabled", true},
            {"invited_users_list", json11::Json::array{}},
        };
    }

    std::string ResponseStore::buildWorldsPoll(
        const std::vector<std::string>& worldNodeIds) const
    {
        json11::Json::array nodes;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            for (const auto& id : worldNodeIds)
            {
                const WorldEntry* e = worldsLoaded ? findWorldLocked(id) : nullptr;
                if (e)
                    nodes.push_back(buildWorldNodeJson(*e, true));
                else
                    nodes.push_back(GenericWorldNode(id, identityUserId, identityDisplayName));
            }
        }
        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{ {"nodes", nodes} }}
        }).dump();
    }

    const ResponseStore::WorldEntry* ResponseStore::findWorldLocked(const std::string& worldId) const
    {
        for (const auto& e : worlds)
        {
            if (e.worldId == worldId)
            {
                return &e;
            }
        }

        return nullptr;
    }

    // "file:///" then the forward-slashed absolute path for an existing media file, or "" if it is absent.
    // Composed at serve time so a scrubbed seed carries no machine path.
    static std::string FileUriIfExists(const std::filesystem::path& file)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(file, ec))
            return std::string();
        std::string abs = NarrowUtf8(fs::absolute(file, ec).wstring());
        for (auto& c : abs)
            if (c == '\\')
                c = '/';
        return "file:///" + abs;
    }

    // Stable non-zero numeric cubemap_id derived from the world id (FNV-1a 64-bit).
    // Deterministic, never "0", and identical across serves for a given world. No persistence needed.
    static std::string DeriveCubemapId(const std::string& worldId)
    {
        unsigned long long h = 1469598103934665603ULL;
        for (char c : worldId)
        {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ULL;
        }
        unsigned long long low = h % 1000000000000000ULL; // 10^15
        char buf[32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "7%015llu", low);
        return std::string(buf);
    }

    // Serve-time file:/// url for a world's screenshot, never stored so no machine path lives in a seed.
    // Serves "" if neither exists, and luckily the game tolerates an empty screenshot_uri.
    std::string ResponseStore::screenshotFileUri(const WorldEntry& entry) const
    {
        std::filesystem::path folder(entry.folder);
        std::string uri = FileUriIfExists(folder / "screenshot.png");
        if (uri.empty())
            uri = FileUriIfExists(folder / "screenshot.jpg");
        return uri;
    }

    // Handle cubemap_uri: base64("file:///…/cubemap.dds") when the world folder holds one uploaded as an OCH2CUBE .dds.
    // cubemap_uri is base64-of-the-url on the wire. It serves "" when no cubemap.dds.
    std::string ResponseStore::cubemapUriBase64(const WorldEntry& entry) const
    {
        std::string uri = FileUriIfExists(std::filesystem::path(entry.folder) / "cubemap.dds");
        if (uri.empty())
            return std::string();
        return Base64Encode(uri);
    }

    void ResponseStore::noteCubemapServe(const std::string& worldId) const
    {
        std::lock_guard<std::mutex> lock(gapsMutex);
        if (loggedCubemaps.insert(worldId).second)
            LogLine("store: serving cubemap_uri for world " + worldId);
    }

    // The shared ~19-field worlds-list and poll node.
    // Stored fields come from config.json, owner_id and owner_name from identity, screenshot_uri is composed at serve time, and cubemap_uri is "" to ship the captured-valid empty state.
    // asWorldTypename adds "__typename":"World" for poll nodes and omits it for list nodes, which sit under the outer User node.
    json11::Json ResponseStore::buildWorldNodeJson(const WorldEntry& entry,
                                                   bool asWorldTypename) const
    {
        const json11::Json& c = entry.config;
        json11::Json::object node;
        if (asWorldTypename)
            node["__typename"] = std::string("World");
        node["id"] = entry.worldId;
        node["name"] = entry.name;
        node["owner_id"] = identityUserId;
        // cubemap: serve base64("file:///…/cubemap.dds") and a non-zero id when the world has one, else the captured-valid empty state of "" and "0"
        std::string cubemapUri = cubemapUriBase64(entry);
        if (!cubemapUri.empty())
        {
            node["cubemap_uri"] = cubemapUri;
            node["cubemap_id"] = DeriveCubemapId(entry.worldId);
            noteCubemapServe(entry.worldId);
        }
        else
        {
            node["cubemap_uri"] = std::string("");
            node["cubemap_id"] = c["cubemap_id"].is_string() ? c["cubemap_id"].string_value() : std::string("0");
        }
        node["screenshot_uri"] = screenshotFileUri(entry);
        node["owner_name"] = identityDisplayName;
        node["is_liked"] = c["is_liked"].bool_value();
        node["visible_to_employee_only"] = c["visible_to_employee_only"].bool_value();
        node["like_count"] = c["like_count"].is_number() ? c["like_count"].int_value() : 0;
        node["creation_index"] = c["creation_index"].is_number() ? c["creation_index"].int_value() : 0;
        node["name_index"] = c["name_index"].is_number() ? c["name_index"].int_value() : 0;
        node["multiplayer_privacy"] = c["multiplayer_privacy"].is_string() ? c["multiplayer_privacy"].string_value() : std::string("PRIVATE");
        node["guest_users_list"] = c["guest_users_list"].is_array() ? c["guest_users_list"] : json11::Json(json11::Json::array{});
        node["max_mp_guests"] = c["max_mp_guests"].is_number() ? c["max_mp_guests"].int_value() : 7;
        node["user_locked_edit"] = c["user_locked_edit"].bool_value();
        node["auto_capture_enabled"] = c["auto_capture_enabled"].is_bool() ? c["auto_capture_enabled"].bool_value() : true;
        node["invited_users_list"] = c["invited_users_list"].is_array() ? c["invited_users_list"] : json11::Json(json11::Json::array{});
        return json11::Json(node);
    }

    std::string ResponseStore::buildWorldsList() const
    {
        json11::Json::array nodes;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            for (const auto& e : worlds)
                nodes.push_back(buildWorldNodeJson(e, false));
        }
        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"node", json11::Json::object{
                    {"__typename", std::string("User")},
                    {"worlds", json11::Json::object{ {"nodes", nodes} }}
                }}
            }}
        }).dump();
    }

    // World content from the folder's config.json: objects verbatim and customizations re-serialized as a raw escaped JSON string
    std::string ResponseStore::buildWorldContent(const std::string& worldNodeId, const std::string& clientMutationId) const
    {
        std::string name, id;
        json11::Json objects, customizations;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            const WorldEntry* e = findWorldLocked(worldNodeId);
            if (e)
            {
                found = true;
                name = e->name;
                id = e->worldId;
                objects = e->config["objects"];
                customizations = e->config["customizations"];
            }
        }

        if (!found) return buildCanned(kDocWorldContent, clientMutationId, worldNodeId);

        std::string customizationsStr = customizations.is_null() ? std::string("{}")
                                                                 : customizations.dump();
        json11::Json::object node{
            {"__typename", std::string("World")},
            {"name", name},
            {"id", id},
            {"objects", json11::Json::object{
                {"nodes", objects.is_array() ? objects : json11::Json(json11::Json::array{})} }},
            {"customizations", customizationsStr},
            {"reported_object_ids", json11::Json::array{}}
        };
        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{ {"node", node} }}
        }).dump();
    }

    // Serve-time defensive resolution of the default world id: return defaultWorldId if it maps to a loaded world whose folder still exists on disk.
    // Otherwise fall back to an existing loaded world (lowest creation_index) so a stale/dangling default never serves a non-existent world.
    std::string ResponseStore::resolveDefaultWorldId() const
    {
        namespace fs = std::filesystem;
        std::string requested;
        {
            std::lock_guard<std::mutex> lock(defaultWorldMutex);
            requested = defaultWorldId;
        }

        std::lock_guard<std::mutex> lock(worldsMutex);
        if (worlds.empty()) return std::string();

        std::error_code ec;
        if (!requested.empty())
        {
            const WorldEntry* e = findWorldLocked(requested);
            if (e && fs::exists(fs::path(e->folder) / "config.json", ec)) return requested; // valid and still on disk
        }

        // Fall back to the loaded world with the lowest creation_index whose folder still exists (the default is creation_index 0)
        const WorldEntry* best = nullptr;
        for (const auto& e : worlds)
        {
            if (!fs::exists(fs::path(e.folder) / "config.json", ec)) continue;
            
            if (!best || e.config["creation_index"].int_value() < best->config["creation_index"].int_value())
            {
                best = &e;
            }
        }
        if (!best) return std::string();

        if (best->worldId != requested)
        {
            LogLine("store: default world " + (requested.empty() ? std::string("unset") : requested) + " missing, serving " + best->worldId);
        }

        return best->worldId;
    }

    // default-world read: the defensively resolved default
    std::string ResponseStore::buildDefaultWorld() const
    {
        std::string id = resolveDefaultWorldId();
        if (id.empty())
        {
            std::string canned = buildCanned(kDocDefaultWorld, "0", std::string());
            if (!canned.empty())
                return canned;
        }
        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"my_world_data", json11::Json::object{ {"default_world_id", id} }}
            }}
        }).dump();
    }

    // A 16-digit numeric string, a fixed non-zero leading digit '8' and 15 low digits folded from the system clock plus a per-call salt for uniqueness.
    // Ids are uint64 strings to the game. Used for world ids and provisional object ids.
    std::string ResponseStore::mintNumericId() const
    {
        static std::atomic<unsigned long long> salt{0};
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned long long ticks =
            ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        unsigned long long s = salt.fetch_add(1, std::memory_order_relaxed);
        unsigned long long low = (ticks + s * 2654435761ULL) % 1000000000000000ULL; // 10^15
        char buf[32];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "8%015llu", low);
        return std::string(buf);
    }

    std::string ResponseStore::mintWorldId() const
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (int tries = 0; tries < 64; ++tries)
        {
            std::string id = mintNumericId();
            fs::path folder = fs::path(worldsDir) / ("world_" + id);
            if (!fs::exists(folder, ec))
                return id;
        }
        return mintNumericId(); // extremely unlikely, accept the last mint though
    }

    // Build a fresh config.json object for a newly-created world, owner-neutral with an empty name and fresh indices.
    // Objects are seeded empty: on an in-VR create the game populates the new world itself via world_batch_update_objects with the starter furniture
    // Default room customizations are hardcoded and default. The game's populate batch overwrites them anyway if it sends its own.
    // The furnished 71-object default ships as a prebuilt world
    json11::Json ResponseStore::buildCanonicalConfig(const std::string& worldId, int creationIndex, int nameIndex) const
    {
        json11::Json customizations = json11::Json::object{
            {"CeilingMaterialIndex", std::string("1307410099367935")},
            {"WallsMaterialIndex",   std::string("268649833656140")},
            {"FloorMaterialIndex",   std::string("395295614222659")},
            {"TrimIndex",            std::string("1997259783886885")},
            {"SkyboxIndex",          std::string("1594405190615066")},
            {"MusicIndex",           std::string("134810140648857")},
            {"AmbientSoundIndex",    std::string("2138994519491875")}
        };

        return json11::Json(json11::Json::object{
            {"world_id", worldId},
            {"name", std::string("")},
            {"creation_index", creationIndex},
            {"name_index", nameIndex},
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
            {"objects", json11::Json::array{}}
        });
    }

    bool ResponseStore::writeFileAtomic(const std::wstring& path, const std::string& content) const
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

    // Frame the world_create ack for a given (already-created) world id.
    static std::string WorldCreateAck(const std::string& worldId, const std::string& clientMutationId)
    {
        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"world_create", json11::Json::object{
                    {"world", json11::Json::object{ {"id", worldId} }},
                    {"client_mutation_id", clientMutationId}
                }}
            }}
        }).dump();
    }

    // world_create: mint id, then folder and config.json with config.json written last.
    // Append a WorldEntry so a same-session worlds-list or poll includes it, then ack.
    // Also handles the game's second create call, fired roughly 2s after the real one, via a time window so one in VR create makes one folder.
    std::string ResponseStore::buildWorldCreate(const std::string& clientMutationId, const std::string& variablesJson) const
    {
        long long nowMs = static_cast<long long>(GetTickCount64());
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            if (worldCreateLogCount < 8)
            {
                ++worldCreateLogCount;
                LogLine("store: world_create REQUEST variables #" + std::to_string(worldCreateLogCount) + ": " + variablesJson);
            }

            // Collapse a create that lands within the window of the previous real create.
            if (!lastCreateWorldId.empty() && (nowMs - lastCreateTimeMs) < kCreateCollapseMs)
            {
                LogLine("store: world_create collapsed (" + std::to_string(nowMs - lastCreateTimeMs) + "ms after world " + lastCreateWorldId + "), no new folder, cmid " + clientMutationId);
                return WorldCreateAck(lastCreateWorldId, clientMutationId);
            }
        }

        std::string worldId = mintWorldId();

        int creationIndex = 0, nameIndex = 0;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            // creation_index keeps the monotonic max(existing) plus 1, mirroring the real backend's all-time counter used for ordering and default-world selection.
            // name_index is the display title the frontend turns into "Home #NN", so make it the world count +1. 
            nameIndex = -1 + (static_cast<int>(worlds.size()) + 1);
            for (const auto& e : worlds)
            {
                int ci = e.config["creation_index"].int_value();
                if (ci + 1 > creationIndex) creationIndex = ci + 1;
            }
        }

        json11::Json cfg = buildCanonicalConfig(worldId, creationIndex, nameIndex);

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path folder = fs::path(worldsDir) / ("world_" + worldId);
        fs::create_directories(folder, ec);

        // Seed screenshot from the bundled default PNG on a best-effort basis, and the frontend regenerates a sidecar from any later JPG upload.
        fs::path seed = fs::path(appDir) / "images" / "world-default.png";
        if (fs::exists(seed, ec))
            CopyFileW(seed.wstring().c_str(), (folder / "screenshot.png").wstring().c_str(), FALSE);

        if (!writeFileAtomic((folder / "config.json").wstring(), cfg.dump()))
            LogLine("store: world_create: config.json write failed for " + worldId);

        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            WorldEntry e;
            e.worldId = worldId;
            e.folder = folder.wstring();
            e.name = "";
            e.config = cfg;
            worlds.push_back(std::move(e));
            worldsLoaded = true;

            // Anchor the collapse window to this real create. It is not extended on a collapse, so a genuine later create minutes away is never folded in.
            lastCreateTimeMs = nowMs;
            lastCreateWorldId = worldId;
        }
        LogLine("store: world_create: new world " + worldId + " (creation_index " + std::to_string(creationIndex) + ", client_mutation_id " + clientMutationId + ")");

        return WorldCreateAck(worldId, clientMutationId);
    }

    // set_default_world: live-mutable defaultWorldId, where an in-VR set overrides the frontend's choice
    std::string ResponseStore::buildSetDefaultWorld(const std::string& clientMutationId, const std::string& worldId) const
    {
        {
            std::lock_guard<std::mutex> lock(defaultWorldMutex);
            defaultWorldId = worldId;
            persistPrefsField("defaultWorldId", worldId);
        }
        LogLine("store: set_default_world: " + worldId + " (persisted to preferences.json)");
        
        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"set_default_world", json11::Json::object{
                    {"world_id", worldId},
                    {"client_mutation_id", clientMutationId}
                }}
            }}
        }).dump();
    }

    // update_name_world: name arrives as base64(urlencode(name)). Decode both layers, write config.json "name", update the in-memory entry.
    std::string ResponseStore::buildUpdateNameWorld(const std::string& clientMutationId, const std::string& worldId, const std::string& nameBase64) const
    {
        std::string decoded = UrlDecode(Base64Decode(nameBase64));

        std::wstring cfgPath;
        json11::Json newCfg;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            for (auto& e : worlds)
            {
                if (e.worldId == worldId)
                {
                    json11::Json::object obj = e.config.object_items();
                    obj["name"] = decoded;
                    e.config = json11::Json(obj);
                    e.name = decoded;
                    newCfg = e.config;
                    cfgPath = (std::filesystem::path(e.folder) / "config.json").wstring();
                    found = true;
                    break;
                }
            }
        }
        if (found)
        {
            if (writeFileAtomic(cfgPath, newCfg.dump()))
                LogLine("store: update_name_world: " + worldId + " name updated");
            else
                LogLine("store: update_name_world: config.json write failed for " + worldId);
        }
        else
        {
            LogLine("store: update_name_world: world " + worldId + " not found (ack only)");
        }

        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"update_name_world", json11::Json::object{
                    {"world", json11::Json::object{ {"id", worldId} }}
                }}
            }}
        }).dump();
    }

    // world_batch_update wire-to-stored encoding. The wire sends animation_offset as a numeric string and simulate_physics as a numeric string.
    // World-content stores the first as an int and the second as the game's enum string. Shared by the create and update paths.
    static int MapAnimationOffset(const std::string& wire)
    {
        if (wire.empty()) return 0;

        char* end = nullptr;
        long v = std::strtol(wire.c_str(), &end, 10);
        if (end == wire.c_str() || *end != '\0')
        {
            LogLine("store: world_batch_update_objects: animation_offset '" + wire + "' not an int, stored 0");
            return 0;
        }

        return static_cast<int>(v);
    }

    static std::string MapSimulatePhysics(const std::string& wire)
    {
        if (wire.empty() || wire == "0")   return std::string("DEFAULT");

        LogLine("store: world_batch_update_objects: simulate_physics non-zero '" + wire + "' stored verbatim");
        return wire;
    }

    // world_batch_update_objects: batches are deltas merged into the world's config.json objects[], never a replace.
    // Mutate the in-memory WorldEntry.config under worldsMutex, then writeFileAtomic.
    // update[] patches transforms/anim/physics/updated_time and keeps id/inventory_item/ item_definition/customization.
    // create[] mints an id per entry in request order and builds a full node.
    // delete[] erases by id. world_customizations, base64(JSON) on the wire, is decoded and stored as an object. Response shape unchanged.
    std::string ResponseStore::buildWorldBatchUpdate(const std::string& clientMutationId, const std::string& worldId, const std::string& worldCustomizationsB64, const json11::Json& createArr, const json11::Json& updateArr, const json11::Json& deleteArr) const
    {
        json11::Json::array created, updated, deleted;

        std::wstring cfgPath;
        json11::Json newCfg;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            for (auto& e : worlds) // findWorldLocked is const, so mutation handlers iterate directly.
            {
                if (e.worldId != worldId) continue;
                found = true;

                json11::Json::object cfg = e.config.object_items();
                std::vector<json11::Json> objs = cfg["objects"].array_items();

                // update[]: overwrite transforms/anim/physics/updated_time, keep identity fields.
                for (const auto& u : updateArr.array_items())
                {
                    std::string oid = u["object_id"].string_value();
                    bool matched = false;
                    for (auto& node : objs)
                    {
                        if (node["id"].string_value() != oid) continue;

                        json11::Json::object n = node.object_items();
                        n["position"] = u["position"];
                        n["rotation"] = u["rotation"];
                        n["scale"]    = u["scale"];
                        n["animation_offset"] = MapAnimationOffset(u["animation_offset"].string_value());
                        n["simulate_physics"] = MapSimulatePhysics(u["simulate_physics"].string_value());

                        // Persist a per-object customization when the update carries one. Confirmed
                        // from a live capture: a UGC place's lighting edit rides here as base64(JSON)
                        // (same wire form as world_customizations), e.g. {"InteriorLightColor":
                        // "X=.. Y=.. Z=..","DirectionaLightColor":".."}. Store it decoded as a raw
                        // JSON string to match config.json's objects[].customization form. Standard
                        // furniture updates omit the field, so keeping the existing value is the no-op
                        // default with no regression. A value that isn't valid base64(JSON) is ignored.
                        if (u["customization"].is_string() && !u["customization"].string_value().empty())
                        {
                            std::string decoded = Base64Decode(u["customization"].string_value());
                            std::string cerr;
                            json11::Json parsed = json11::Json::parse(decoded, cerr);
                            
                            if (cerr.empty() && parsed.is_object())
                                n["customization"] = decoded;
                            else
                                LogLine("store: world_batch_update_objects: object " + oid + " customization decode failed, kept existing");
                        }
                        
                        n["updated_time"] = static_cast<int>(NowUnix());
                        node = json11::Json(n);
                        matched = true;
                        break;
                    }

                    updated.push_back(oid); // echo regardless (game does optimistic UI).
                    if (!matched) LogLine("store: world_batch_update_objects: update object " + oid + " not found in world " + worldId + " (echoed anyway)");
                }

                // create[]: mint id per entry in request order, build a full node.
                for (const auto& c : createArr.array_items())
                {
                    std::string oid = mintNumericId();

                    // single re-mint on the astronomically-unlikely collision.
                    for (const auto& node : objs)
                    {
                        if (node["id"].string_value() == oid)
                        {
                            oid = mintNumericId();
                            break;
                        }
                    }

                    // invId is the wire inventory-entry id from the inventory node "id".
                    // The catalog def id is distinct now, so recover it for item_definition.id
                    // Fallback to invId, the old behavior, only for an unknown entry id, which shouldn't happen offline since the game can only place from the user's own inventory.
                    std::string invId = c["inventory_item_id"].string_value();
                    std::string defId = defIdForInventoryEntry(invId);
                    if (defId.empty())
                    {
                        LogLine("store: world_batch_update_objects: create unknown inventory_item_id " + invId + ", item_definition.id falls back to it");
                        defId = invId;
                    }

                    json11::Json::object node{
                        {"animation_offset", MapAnimationOffset(c["animation_offset"].string_value())},
                        {"customization", std::string("")},
                        {"id", oid},
                        {"inventory_item", json11::Json::object{ {"id", invId} }},
                        {"item_definition", json11::Json::object{
                            {"__typename", std::string("WorldsItemDefinition")}, {"id", defId} }},
                        {"position", c["position"]},
                        {"rotation", c["rotation"]},
                        {"scale", c["scale"]},
                        {"simulate_physics", MapSimulatePhysics(c["simulate_physics"].string_value())},
                        {"updated_time", static_cast<int>(NowUnix())}
                    };

                    objs.push_back(json11::Json(node));
                    created.push_back(oid);
                    LogLine("store: world_batch_update_objects: create inventory_item_id=" + invId + " item_definition_id=" + defId + " minted object_id=" + oid);
                }

                // delete[]: element is {object_id} or a bare string, erase by id, echo regardless.
                for (const auto& d : deleteArr.array_items())
                {
                    std::string oid = d.is_string() ? d.string_value() : d["object_id"].string_value();
                    for (auto it = objs.begin(); it != objs.end(); ++it)
                    {
                        if ((*it)["id"].string_value() == oid)
                        {
                            objs.erase(it);
                            break;
                        }
                    }
                       
                    deleted.push_back(oid);
                }

                // world_customizations: came as base64(JSON) stored as the decoded object
                if (!worldCustomizationsB64.empty())
                {
                    std::string decoded = Base64Decode(worldCustomizationsB64);
                    std::string cerr;
                    json11::Json parsed = json11::Json::parse(decoded, cerr);

                    if (cerr.empty() && parsed.is_object())
                        cfg["customizations"] = parsed;
                    else
                        LogLine("store: world_batch_update_objects: customizations decode failed, kept existing");
                }

                cfg["objects"] = json11::Json(objs);
                e.config = json11::Json(cfg);
                newCfg = e.config;
                cfgPath = (std::filesystem::path(e.folder) / "config.json").wstring();
                break;
            }
        }

        if (found)
        {
            if (writeFileAtomic(cfgPath, newCfg.dump()))
                LogLine("store: world_batch_update_objects: persisted world " + worldId +
                        " created=" + std::to_string(created.size()) +
                        " updated=" + std::to_string(updated.size()) +
                        " deleted=" + std::to_string(deleted.size()));
            else
                LogLine("store: world_batch_update_objects: config.json write failed for " + worldId);
        }
        else
        {
            // Safety branch when the world is not loaded: ack only, mint created[] in order, echo the rest.
            for (size_t i = 0; i < createArr.array_items().size(); ++i)
            {
                created.push_back(mintNumericId());
            }

            for (const auto& u : updateArr.array_items())
            {
                updated.push_back(u.is_string() ? u.string_value() : u["object_id"].string_value());
            }
               
            for (const auto& d : deleteArr.array_items())
            {
                deleted.push_back(d.is_string() ? d.string_value() : d["object_id"].string_value());
            }
            LogLine("store: world_batch_update_objects: world " + worldId + " not found (ack only)");
        }

        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"world_batch_update_objects", json11::Json::object{
                    {"client_mutation_id", clientMutationId},
                    {"created", created},
                    {"updated", updated},
                    {"deleted", deleted}
                }}
            }}
        }).dump();
    }

    // Like/unlike toggle with no direction flag: flip is_liked, set like_count to is_liked?1:0, which offline is a user's own like only, persist config.json atomically, update the in-memory entry.
    // buildWorldNodeJson serves is_liked and like_count from config, so a queried worlds-list reflects it.
    std::string ResponseStore::buildWorldLikeToggle(const std::string& clientMutationId, const std::string& worldId) const
    {
        bool newLiked = false;
        std::wstring cfgPath;
        json11::Json newCfg;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            for (auto& e : worlds)
            {
                if (e.worldId == worldId)
                {
                    newLiked = !e.config["is_liked"].bool_value();
                    json11::Json::object obj = e.config.object_items();
                    obj["is_liked"] = newLiked;
                    obj["like_count"] = newLiked ? 1 : 0;
                    e.config = json11::Json(obj);
                    newCfg = e.config;
                    cfgPath = (std::filesystem::path(e.folder) / "config.json").wstring();
                    found = true;
                    break;
                }
            }
        }
        if (found)
        {
            if (writeFileAtomic(cfgPath, newCfg.dump()))
                LogLine("store: world like toggle: world " + worldId + " is_liked=" + (newLiked ? "true" : "false"));
            else
                LogLine("store: world like toggle: config.json write failed for " + worldId);
        }
        else
        {
            LogLine("store: world like toggle: world " + worldId + " not found (ack only)");
        }

        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"world_update_is_liked", json11::Json::object{
                    {"world", json11::Json::object{
                        {"id", worldId},
                        {"is_liked", newLiked}
                    }},
                    {"client_mutation_id", clientMutationId}
                }}
            }}
        }).dump();
    }

    // Delete a world: remove its folder recursively and drop the in-memory entry.
    // If it was the last world, worldsLoaded flips false so serving falls back to the static templates until the frontend auto-seeds.
    // A deleted default is covered by resolveDefaultWorldId.
    std::string ResponseStore::buildWorldDelete(const std::string& clientMutationId, const std::string& worldId) const
    {
        std::wstring folder;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            for (auto it = worlds.begin(); it != worlds.end(); ++it)
            {
                if (it->worldId == worldId)
                {
                    folder = it->folder;
                    worlds.erase(it);
                    break;
                }
            }
            if (worlds.empty())
            {
                worldsLoaded = false; // fallback until the frontend re-seeds
            }
        }

        std::error_code ec;
        std::filesystem::path target = folder.empty() ? (std::filesystem::path(worldsDir) / ("world_" + worldId)) : std::filesystem::path(folder);
        std::filesystem::remove_all(target, ec);
        LogLine("store: world_delete: removed world " + worldId + (ec ? " (folder remove error " + std::to_string(ec.value()) + ")" : ""));

        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"world_delete", json11::Json::object{
                    {"world", json11::Json::object{ {"id", worldId} }},
                    {"client_mutation_id", clientMutationId}
                }}
            }}
        }).dump();
    }

    // user_locked_edit: persist config.json user_locked_edit field to the new value. Served by buildWorldNodeJson.
    // {"data":{"world_set_user_locked_edit":{"success":true}}}.
    std::string ResponseStore::buildWorldSetLockedEdit(const std::string& clientMutationId, const std::string& worldId, bool newLockedEdit) const
    {
        (void)clientMutationId; // the confirmed response does not echo the cmid
        std::wstring cfgPath;
        json11::Json newCfg;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            for (auto& e : worlds)
            {
                if (e.worldId == worldId)
                {
                    json11::Json::object obj = e.config.object_items();
                    obj["user_locked_edit"] = newLockedEdit;
                    e.config = json11::Json(obj);
                    newCfg = e.config;
                    cfgPath = (std::filesystem::path(e.folder) / "config.json").wstring();
                    found = true;
                    break;
                }
            }
        }
        if (found)
        {
            if (writeFileAtomic(cfgPath, newCfg.dump()))
                LogLine("store: world_set_user_locked_edit: world " + worldId + " user_locked_edit=" + (newLockedEdit ? "true" : "false"));
            else
                LogLine("store: world_set_user_locked_edit: config.json write failed for " + worldId);
        }
        else
        {
            LogLine("store: world_set_user_locked_edit: world " + worldId + " not found (ack only)");
        }
        return json11::Json(json11::Json::object{
            {"data", json11::Json::object{
                {"world_set_user_locked_edit", json11::Json::object{ {"success", true} }}
            }}
        }).dump();
    }

    // Lazy, thread-safe GDI+ startup for the process lifetime, never shut down. Only ever reached from the TLS request thread via WriteWorldMedia, never DllMain, so starting GDI+ here is safe.
    static bool EnsureGdiplus()
    {
        static std::once_flag once;
        static bool ok = false;
        static ULONG_PTR token = 0;
        std::call_once(once, []
        {
            Gdiplus::GdiplusStartupInput input;
            ok = (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok);
        });
        return ok;
    }

    static bool GetPngEncoderClsid(CLSID& clsid)
    {
        UINT num = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        if (size == 0) return false;

        std::vector<BYTE> buf(size);
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
        Gdiplus::GetImageEncoders(num, size, codecs);
        
        for (UINT i = 0; i < num; ++i)
        {
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0)
            {
                clsid = codecs[i].Clsid;
                return true;
            }
        }

        return false;
    }

    // Decode the uploaded image bytes (JPEG) via GDI+ and write them as PNG at pngPath
    static bool WriteImageBytesAsPng(const std::string& bytes, const std::wstring& pngPath)
    {
        if (!EnsureGdiplus() || bytes.empty()) return false;

        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        if (!hMem) return false;

        if (void* p = GlobalLock(hMem))
        {
            memcpy(p, bytes.data(), bytes.size());
            GlobalUnlock(hMem);
        }

        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK) // TRUE: stream frees hMem on release
        {
            GlobalFree(hMem);
            return false;
        }

        std::wstring tmp = pngPath + L".tmp";
        bool ok = false;
        {
            Gdiplus::Bitmap bmp(stream, FALSE);
            CLSID clsid;
            if (bmp.GetLastStatus() == Gdiplus::Ok && GetPngEncoderClsid(clsid))
            {
                ok = (bmp.Save(tmp.c_str(), &clsid, nullptr) == Gdiplus::Ok);
            }
        } // bmp destroyed (releases its hold on the stream) before release the stream
        stream->Release();

        if (!ok)
        {
            DeleteFileW(tmp.c_str());
            return false;
        }

        if (!MoveFileExW(tmp.c_str(), pngPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    // Persist uploaded world media. Screenshots arrive as JPEG but are converted to screenshot.png so the .png is the only screenshot file both the game via screenshot_uri and the frontend UI read.
    // A decode failure falls back to screenshot.jpg so at least an upload is never lost.
    // Cubemaps are OCH2CUBE .dds and stored as cubemap.dds.
    bool ResponseStore::WriteWorldMedia(const std::string& worldId, bool isScreenshot, const std::string& bytes)
    {
        std::wstring folder;
        {
            std::lock_guard<std::mutex> lock(worldsMutex);
            const WorldEntry* e = findWorldLocked(worldId);
            if (e)
            {
                folder = e->folder;
            }  
        }

        if (folder.empty())
        {
            folder = (std::filesystem::path(worldsDir) / ("world_" + worldId)).wstring();
        }

        std::error_code ec;
        std::filesystem::create_directories(folder, ec);

        if (!isScreenshot) return writeFileAtomic((std::filesystem::path(folder) / L"cubemap.dds").wstring(), bytes);

        std::wstring pngPath = (std::filesystem::path(folder) / L"screenshot.png").wstring();
        if (WriteImageBytesAsPng(bytes, pngPath))
        {
            return true;
        }
        
        LogLine("store: screenshot JPEG-to-PNG convert failed, wrote raw screenshot.jpg");
        return writeFileAtomic((std::filesystem::path(folder) / L"screenshot.jpg").wstring(), bytes);
    }

    // Add an owned_items entry per placed UGC object across all loaded worlds, keyed by the object's real inventory_item.id, the entry_id, so the game recognizes the object as owned and lets its UGC editor commit.
    // A UGC place's lighting update is withheld unless its inventory_item.id is owned.
    // It is de-duped by entry id, skipping ids already present..
    void ResponseStore::augmentInventoryFromWorldUgc()
    {
        std::unordered_set<std::string> haveEntry;
        for (const auto& item : ownedItems.array_items())
        {
            std::string defId = item["item_def_id"].string_value();
            if (defId.empty()) continue;

            haveEntry.insert(item["entry_id"].is_string() && !item["entry_id"].string_value().empty() ? item["entry_id"].string_value() : DeriveInventoryEntryId(defId));
        }

        std::vector<json11::Json> owned = ownedItems.array_items();
        size_t added = 0;
        for (const auto& e : worlds)
        {
            for (const auto& obj : e.config["objects"].array_items())
            {
                const std::string& tn = obj["item_definition"]["__typename"].string_value();
                if (tn.rfind("WorldsUGC", 0) != 0) continue;
                
                std::string invId = obj["inventory_item"]["id"].string_value();
                std::string defId = obj["item_definition"]["id"].string_value();

                if (invId.empty() || defId.empty()) continue;
                if (!haveEntry.insert(invId).second) continue; // this inventory_item is already owned

                owned.push_back(json11::Json::object{ {"item_def_id", defId}, {"entry_id", invId} });
                ++added;
            }
        }

        if (added)
        {
            ownedItems = json11::Json(owned);
            ownedLoaded = true;
            LogLine("store: owned " + std::to_string(added) + " placed UGC object(s) from world(s) (editable-UGC gate)");
        }
    }

    // Rebuild the entry-id to def-id reverse map from the final ownedItems after any UGC augmentation, honoring explicit entry_ids.
    // It lets the create path recover item_definition.id from a wire inventory_item_id, including placed UGC objects.
    void ResponseStore::rebuildInventoryReverseMap()
    {
        inventoryEntryToDefId.clear();
        for (const auto& item : ownedItems.array_items())
        {
            std::string defId = item["item_def_id"].string_value();
            if (defId.empty()) continue;
            
            std::string entryId =
                item["entry_id"].is_string() && !item["entry_id"].string_value().empty()
                    ? item["entry_id"].string_value()
                    : DeriveInventoryEntryId(defId);
            inventoryEntryToDefId[entryId] = defId;
        }
    }

    std::string ResponseStore::buildInventory() const
    {
        std::string nodes;
        bool first = true;
        for (const auto& item : ownedItems.array_items())
        {
            std::string defId = item["item_def_id"].string_value();
            if (defId.empty()) continue;
            
            if (!first)
            {
                nodes += ",";
            }
                
            first = false;
            // A node "id" is a placed UGC object which carries an explicit entry_id of its real inventory_item.id so the game matches the world object to its owned node and so treats
            // it as owned and editable. Everything else uses a stable, derived id, since a real owned_items[].id is not the item_definition_id.
            // Collapsing the entry id onto the def id made created objects the only ones that match an inventory node on reload, arming a runaway owned/used reconciliation loading void.
            // Both forms keep the entry id distinct from the def id. owned_count is large for the same reason.
            std::string entryId = item["entry_id"].is_string() && !item["entry_id"].string_value().empty() ? item["entry_id"].string_value() : DeriveInventoryEntryId(defId);
            std::string escEntryId = JsonEscape(entryId);
            std::string escDefId = JsonEscape(defId);
            nodes += "{\"id\":\"" + escEntryId + "\",\"is_new\":false,"
                     "\"item_definition_id\":\"" + escDefId + "\",\"owned_count\":" +
                     std::to_string(kOwnedCount) +
                     ",\"used_count\":0,\"time_first_received\":" +
                     std::to_string(kTimeFirstReceived) +
                     ",\"time_owned_count_updated\":" + std::to_string(kTimeOwnedUpdated) +
                     ",\"achievements\":{\"edges\":[]},\"applications\":[]}";
        }
        std::string body = "{\"data\":{\"my_world_data\":{\"last_inventory_view_time\":";
        body += std::to_string(NowUnix());
        body += ",\"owned_items\":{\"nodes\":[" + nodes + "]}}}}";
        return body;
    }

    // entry id, as served in inventory node "id", maps to def id, or "" if unknown.
    // It is built at Load from ownedItems and lets the create path fill item_definition.id from the wire inventory_item_id.
    std::string ResponseStore::defIdForInventoryEntry(const std::string& entryId) const
    {
        auto it = inventoryEntryToDefId.find(entryId);
        return it != inventoryEntryToDefId.end() ? it->second : std::string();
    }

    // Merge a world's ugc\ugc-hashes.json, a map of def_id to item-def node with signed uris stripped, into ugcDefs, and record each def's local .zst as a file:// uri in ugcZstUri.
    // This is keyed by hash_from_client, so buildItemDefs can serve the UGC def offline. No use if no ugc manifest.
    void ResponseStore::loadWorldUgc(const std::wstring& folder)
    {
        namespace fs = std::filesystem;
        fs::path ugcDir = fs::path(folder) / "ugc";
        std::string text;
        if (!ReadFileText(ugcDir / "ugc-hashes.json", text)) return;

        std::string err;
        json11::Json manifest = json11::Json::parse(text, err);
        if (!err.empty() || !manifest.is_object()) return;

        size_t n = 0;
        for (const auto& kv : manifest.object_items())
        {
            const std::string& defId = kv.first;
            const json11::Json& node = kv.second;
            if (defId.empty() || !node.is_object()) continue;

            ugcDefs[defId] = node;
            ++n;
            std::string hash = node["hash_from_client"].string_value();
            if (!hash.empty() && ugcZstUri.find(hash) == ugcZstUri.end())
            {
                std::string uri = FileUriIfExists(ugcDir / (hash + ".zst"));
                if (!uri.empty())
                {
                    ugcZstUri[hash] = uri;
                }
            }
        }

        if (n)
        {
            LogLine("store: loaded UGC manifest (" + std::to_string(n) + " def(s)) from " + fs::path(folder).filename().string() + "\\ugc");
        }
    }

    std::string ResponseStore::buildItemDefs(const std::vector<std::string>& itemDefIds) const
    {
        std::string nodes;
        for (size_t i = 0; i < itemDefIds.size(); ++i)
        {
            if (i != 0)
            {
                nodes += ",";
            }

            const std::string& id = itemDefIds[i];
            auto ugcIt = ugcDefs.find(id);
            if (ugcIt != ugcDefs.end())
            {
                // Serve the stored UGC def node with real bounds and flags from ugc-hashes.json plus a file:// asset uri so the game maps the def to its cached blob.
                // The frontend copies world_<id>\ugc\<hash>.zst into WorldsCache before launch.
                json11::Json::object o = ugcIt->second.object_items();
                std::string hash = ugcIt->second["hash_from_client"].string_value();
                auto uriIt = ugcZstUri.find(hash);
                o["compressed_zstd_uri"] = uriIt != ugcZstUri.end() ? uriIt->second : std::string("");
                o["glb_uri"] = std::string("");
                o["compressed_glb_uri"] = std::string("");
                nodes += json11::Json(o).dump();
                continue;
            }

            const json11::Json& node = masterDb[id];
            if (node.is_object())
            {
                nodes += node.dump();
            }
            else
            {
                noteGap(id);
                std::string escId = JsonEscape(id);
                nodes += "{\"__typename\":\"WorldsItemDefinition\",\"id\":\"" + escId +
                         "\",\"name\":\"\",\"item_description\":\"\",\"is_scalable\":true,"
                         "\"is_unlimited\":true,\"salvage_value\":0,"
                         "\"world_placement_cap\":-1,\"stackable\":true,\"tags\":[],"
                         "\"rarity\":\"COMMON\",\"local_bounds_origin\":{\"x\":\"00000000\","
                         "\"y\":\"00000000\",\"z\":\"00000000\"},\"local_bounds_extent\":"
                         "{\"x\":\"00000000\",\"y\":\"00000000\",\"z\":\"00000000\"},"
                         "\"asset_key\":\"\",\"asset_pack\":null}";
            }
        }
        return "{\"data\":{\"nodes\":[" + nodes + "]}}";
    }

    // Requested app ids from the query variables. The wire uses non-standard JSON with an unquoted key `app_ids` and single-quoted ids.
    std::vector<std::string> ResponseStore::parseRequestedAppIds(const std::string& variablesJson, bool base64Encoded)
    {
        std::vector<std::string> ids;
        if (variablesJson.empty())
            return ids;

        std::string blob;
        {
            std::string err;
            json11::Json vars = json11::Json::parse(variablesJson, err);
            if (err.empty() && vars.is_object())
            {
                if (base64Encoded && vars["app_ids_encoded"].is_string())
                    blob = Base64Decode(vars["app_ids_encoded"].string_value()); // guest: {app_ids:[...],app_ach_id_pairs:[...]}
                else if (vars["app_ids_json"].is_string())
                    blob = vars["app_ids_json"].string_value(); // non-guest: {app_ids: [...]}
            }
        }

        if (blob.empty()) blob = variablesJson; // defense: scan the raw variables if the field wasn't where we expect

        std::unordered_set<std::string> seen;
        for (size_t i = 0; i < blob.size();)
        {
            char q = blob[i];
            if (q == '\'' || q == '"')
            {
                size_t end = blob.find(q, i + 1);
                if (end == std::string::npos) break;

                std::string tok = blob.substr(i + 1, end - i - 1);
                if (!tok.empty() && tok.find_first_not_of("0123456789") == std::string::npos && seen.insert(tok).second)
                {
                    ids.push_back(tok);
                }
                    
                i = end + 1;
            }
            else
            {
                ++i;
            }
        }
        return ids;
    }

    // Transform a library app, or achievement, object into its delivery form.
    // Every URI field holds a raw url in the manifest and is base64-encoded.
    // Nested Achievements are transformed the same way. Non-URI fields (ID/Canonical/Title/AcquiredTime/Description/UnlockTime) pass through unchanged.
    json11::Json ResponseStore::buildAppNode(const json11::Json& libApp) const
    {
        if (!libApp.is_object()) return json11::Json();

        json11::Json::object o;
        for (const auto& kv : libApp.object_items())
        {
            const std::string& k = kv.first;
            const json11::Json& v = kv.second;
            if (k.size() >= 3 && k.compare(k.size() - 3, 3, "URI") == 0 && v.is_string())
            {
                o[k] = v.string_value().empty() ? std::string() : Base64Encode(v.string_value());
            }
            else if (k == "Achievements" && v.is_array())
            {
                std::vector<json11::Json> achs;
                for (const auto& a : v.array_items())
                {
                    achs.push_back(buildAppNode(a));
                }
                o[k] = achs;
            }
            else
            {
                o[k] = v;
            }
        }
        return json11::Json(o);
    }

    std::string ResponseStore::buildWorldsApps(const std::string& variablesJson) const
    {
        std::unordered_set<std::string> want;
        for (const auto& id : parseRequestedAppIds(variablesJson, /*base64Encoded=*/false))
        {
            want.insert(id);
        }

        std::vector<json11::Json> elems; //each element is a serialized app object held as a string
        if (appLibraryLoaded)
        {
            for (const auto& app : appLibrary["apps"].array_items())
            {
                if (!app.is_object()) continue;
                if (!want.empty() && want.count(app["ID"].string_value()) == 0) continue; // serve only the apps this world actually placed

                json11::Json node = buildAppNode(app);
                if (node.is_object())
                {
                    elems.push_back(json11::Json(node.dump()));
                }
            }
        }
        std::string inner = json11::Json(elems).dump(); // "[]" or "[\"{...}\",...]"
        return "{\"data\":{\"worlds_apps_and_achievements\":\"" + JsonEscape(inner) + "\"}}";
    }

    // worlds_guest_apps_and_achievements (guest variant). The field value is a JSON string whose content is {"apps":[...],"achs":[...]}
    std::string ResponseStore::buildWorldsGuestApps(const std::string& variablesJson) const
    {
        std::unordered_set<std::string> want;
        for (const auto& id : parseRequestedAppIds(variablesJson, /*base64Encoded=*/true))
        {
            want.insert(id);
        }

        std::vector<json11::Json> apps, achs;
        if (appLibraryLoaded)
        {
            for (const auto& app : appLibrary["apps"].array_items())
            {
                if (!app.is_object()) continue;
                if (!want.empty() && want.count(app["ID"].string_value()) == 0) continue;

                json11::Json node = buildAppNode(app);
                if (!node.is_object()) continue;

                apps.push_back(node);
                for (const auto& a : node["Achievements"].array_items())
                {
                    achs.push_back(a);
                }
            }
        }
        json11::Json inner = json11::Json::object{ { "apps", apps }, { "achs", achs } };
        return "{\"data\":{\"worlds_guest_apps_and_achievements\":\"" + JsonEscape(inner.dump()) + "\"}}";
    }

    // Builing a OAF library entitlement node. Per-app fields (id/title/packageName/image urls/grant time) come from apps-library.json.
    // The remaining 50 fields are constant defaults matching a live reply. Image urls are raw file://, since the OAF channel does not base64-encode them, unlike the graphql worlds_apps field.
    json11::Json ResponseStore::buildOafEntitlement(const json11::Json& app) const
    {
        std::string square = app["SquareURI"].string_value();
        std::string landscape = app["LandscapeURI"].string_value();
        std::string icon = app["IconURI"].string_value();
        long long acq = (long long)app["AcquiredTime"].number_value();
        if (acq <= 0) acq = 1451606400LL; //use a 2016-01-01 fallback
        std::string acqMs = std::to_string(acq * 1000LL);

        return json11::Json(json11::Json::object{
            { "activeState", "PERMANENT" },
            { "appGroupingId", "" },
            { "autoUpdateTime", json11::Json() }, // null
            { "availableVersion", "" },
            { "availableVersionCode", -1 },
            { "canAccessFeatureKeys", json11::Json::array{} },
            { "category", "GAMES" },
            { "cloudFileIsDownloading", false },
            { "cloudFileIsEnabled", false },
            { "cloudFileIsSyncing", false },
            { "comfortRating", "" },
            { "coverLandscapeImageLargeUrl", landscape },
            { "coverLandscapeImageUrl", landscape },
            { "coverSquareImageUrl", square },
            { "dlc", json11::Json::array{} },
            { "dominantColor", json11::Json() }, // null
            { "expirationTimeMs", 0.0 },
            { "gameModes", json11::Json::array{} },
            { "genres", json11::Json::array{} },
            { "grantReason", "PAID_OFFER" },
            { "grantTimeMs", (double)(acq * 1000LL) },
            { "hasAchievements", false },
            { "hasIap", false },
            { "hasViewerLeaderboards", false },
            { "iconImageUrl", icon },
            { "id", app["ID"].string_value() },
            { "imageUrl", square },
            { "installedVersion", "1" },
            { "installedVersionCode", 1 },
            { "is2dModeSupported", false },
            { "isDucNonCompliant", false },
            { "isHidden", false },
            { "isInStartMenu", false },
            { "isPinnedInTaskBar", false },
            { "isRefundable", false },
            { "isThirdParty", false },
            { "isVisibleinHome", true },
            { "itemType", "STORE" },
            { "lastAccessedTimeMs", acqMs },
            { "livestreamingStatus", "" },
            { "logoTransparentImageUrl", "" },
            { "longDescription", "" },
            { "packageName", app["Canonical"].string_value() },
            { "playTime", 0 },
            { "queuePosition", -1 },
            { "renderMode", "VR" },
            { "requiredSpace", "0" },
            { "shortDescription", "" },
            { "size", "0" },
            { "smallLandscapeImageUrl", landscape },
            { "starRating", 0.0 },
            { "state", "install_available" },
            { "statusProgress", 0.0 },
            { "statusProgressMax", 0.0 },
            { "supportedControllers", json11::Json::array{} },
            { "supportedInAppLanguages", json11::Json::array{} },
            { "title", app["Title"].string_value() },
        });
    }

    std::string ResponseStore::BuildOafLibraryReply(const std::string& seq, const std::string& ts) const
    {
        json11::Json::array ents;
        if (appLibraryLoaded)
        {
            for (const auto& app : appLibrary["apps"].array_items())
            {
                if (app.is_object() && !app["ID"].string_value().empty())
                {
                    ents.push_back(buildOafEntitlement(app));
                }
            }
        }

        json11::Json payload = json11::Json::object{
            { "entitlements", ents },
            { "isFullLibrary", true },
        };

        std::string body = "{\"messageType\":\"RESPONSE\",\"payload\":" + payload.dump() +
                           ",\"payloadType\":\"LIBRARY_UPDATE\",\"sequenceId\":\"" + JsonEscape(seq) +
                           "\",\"timestamp\":\"" + JsonEscape(ts) + "\"}";
        return body;
    }

    std::string ResponseStore::buildCanned(const std::string& docId, const std::string& clientMutationId, const std::string& worldNodeId) const
    {
        std::string key = docId;
        if (docId == kDocWorldContent)
        {
            // world content is discriminated by world_node_id. Only the chosen start world has a template. Others yield "" and passthrough to the live backend.
            std::string discriminated = docId + "__" + worldNodeId;
            if (cannedTemplates.count(discriminated) != 0)
                key = discriminated;
            else if (cannedTemplates.count(docId) != 0)
                key = docId;
            else
                return std::string();
        }

        auto it = cannedTemplates.find(key);
        if (it == cannedTemplates.end()) return std::string();

        std::string body = it->second;
        if (body.find("__CMID__") != std::string::npos)
        {
            body = ReplaceAll(body, "__CMID__", JsonEscape(clientMutationId));
        }

        return body;
    }

    std::string ResponseStore::buildSetUserOptions(const std::string& clientMutationId, const std::string& base64UserOptions) const
    {
        std::string decoded = Base64Decode(base64UserOptions);
        std::string err;
        json11::Json parsed = json11::Json::parse(decoded, err);
        if (err.empty() && parsed.is_object())
        {
            std::lock_guard<std::mutex> lock(userOptionsMutex);
            userOptionsJson = parsed.dump(); // reflected by a re-queried world_login this session
            persistUserOptions(parsed);
        }
        else
        {
            LogLine("store: set_user_options payload did not base64-decode to a JSON object. Put ack only, not persisted");
        }

        // Bare ack echoing client_mutation_id
        std::string body = "{\"data\":{\"set_user_options\":{\"client_mutation_id\":\"";
        body += JsonEscape(clientMutationId);
        body += "\"}}}";
        return FrameHttp(body);
    }

    // Load-modify-write one top-level preferences.json field, then atomic-replace so the file never tears.
    // Preserves every field this writer does not own, such as frontend and identity
    void ResponseStore::persistPrefsField(const std::string& key, const json11::Json& value) const
    {
        if (prefsPath.empty()) return;

        std::lock_guard<std::mutex> fileLock(prefsFileMutex);
        std::string text;
        ReadFileText(prefsPath, text);
        std::string err;
        json11::Json existing = text.empty() ? json11::Json() : json11::Json::parse(text, err);
        json11::Json::object root = existing.is_object() ? existing.object_items() : json11::Json::object();
        root[key] = value;

        std::wstring tmp = prefsPath + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                LogLine("store: persistPrefsField(" + key + "): could not open temp prefs file for write");
                return;
            }
            f << json11::Json(root).dump();
        }
        if (!MoveFileExW(tmp.c_str(), prefsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            LogLine("store: persistPrefsField(" + key + "): atomic replace of preferences.json failed (err " + std::to_string(GetLastError()) + ")");
            DeleteFileW(tmp.c_str());
            return;
        }
    }

    // Persist userOptions with a load-modify-write and atomic replace. Called under userOptionsMutex.
    void ResponseStore::persistUserOptions(const json11::Json& userOptions) const
    {
        persistPrefsField("userOptions", userOptions);
        LogLine("store: set_user_options: persisted userOptions to preferences.json");
    }

    void ResponseStore::noteGap(const std::string& itemDefId) const
    {
        std::lock_guard<std::mutex> lock(gapsMutex);
        if (loggedGaps.insert(itemDefId).second)
            LogLine("store: item_def " + itemDefId +
                    " not in Master DB, minimal placeholder node (capture gap)");
    }

    std::string ResponseStore::BuildResponse(ResponseAction action, const std::string& docId, const std::string& variablesJson) const
    {
        std::string clientMutationId = "0";
        std::string worldNodeId;
        std::vector<std::string> worldNodeIds;
        std::vector<std::string> itemDefIds;
        std::string userOptionsB64; // set_user_options carries user_options as base64(JSON)
        std::string worldId;       // set_default_world / update_name_world / world_batch_update
        std::string nameB64;       // update_name_world: base64(urlencode(name))
        std::string worldCustomizationsB64; // world_batch_update_objects: base64(JSON) room customizations
        json11::Json createArr, updateArr, deleteArr; // world_batch_update_objects
        bool newUserLockedEdit = false; // world_set_user_locked_edit

        if (!variablesJson.empty())
        {
            std::string err;
            json11::Json vars = json11::Json::parse(variablesJson, err);
            if (err.empty() && vars.is_object())
            {
                // What a mess..
                if (vars["client_mutation_id"].is_string())
                    clientMutationId = vars["client_mutation_id"].string_value();
                if (vars["world_node_id"].is_string())
                    worldNodeId = vars["world_node_id"].string_value();
                if (vars["world_node_ids"].is_array())
                    for (const auto& item : vars["world_node_ids"].array_items())
                        if (item.is_string())
                            worldNodeIds.push_back(item.string_value());
                if (vars["item_def_ids"].is_array())
                    for (const auto& item : vars["item_def_ids"].array_items())
                        if (item.is_string())
                            itemDefIds.push_back(item.string_value());
                if (vars["user_options"].is_string())
                    userOptionsB64 = vars["user_options"].string_value();
                if (vars["world_id"].is_string())
                    worldId = vars["world_id"].string_value();
                if (vars["world_customizations"].is_string())
                    worldCustomizationsB64 = vars["world_customizations"].string_value();
                if (vars["name"].is_string())
                    nameB64 = vars["name"].string_value();
                if (vars["create"].is_array())
                    createArr = vars["create"];
                if (vars["update"].is_array())
                    updateArr = vars["update"];
                if (vars["delete"].is_array())
                    deleteArr = vars["delete"];
                if (vars["new_user_locked_edit"].is_bool())
                    newUserLockedEdit = vars["new_user_locked_edit"].bool_value();
                else if (vars["new_user_locked_edit"].is_string())
                    newUserLockedEdit = (vars["new_user_locked_edit"].string_value() == "true");
            }
        }

        std::string body;
        switch (action)
        {
        case ResponseAction::WorldLogin:
            body = buildWorldLogin(clientMutationId);
            break;
        case ResponseAction::WorldKeepAlive:
            body = buildKeepAlive(clientMutationId);
            break;
        case ResponseAction::WorldsPoll:
            body = buildWorldsPoll(worldNodeIds);
            break;
        case ResponseAction::WorldsList:
            body = buildWorldsList();
            break;
        case ResponseAction::WorldContent:
            body = buildWorldContent(worldNodeId, clientMutationId);
            break;
        case ResponseAction::DefaultWorld:
            body = buildDefaultWorld();
            break;
        case ResponseAction::WorldCreate:
            body = buildWorldCreate(clientMutationId, variablesJson);
            break;
        case ResponseAction::SetDefaultWorld:
            body = buildSetDefaultWorld(clientMutationId, worldId);
            break;
        case ResponseAction::UpdateNameWorld:
            body = buildUpdateNameWorld(clientMutationId, worldId, nameB64);
            break;
        case ResponseAction::WorldBatchUpdate:
            body = buildWorldBatchUpdate(clientMutationId, worldId, worldCustomizationsB64, createArr, updateArr, deleteArr);
            break;
        case ResponseAction::WorldLikeToggle:
            body = buildWorldLikeToggle(clientMutationId, worldId);
            break;
        case ResponseAction::WorldDelete:
            body = buildWorldDelete(clientMutationId, worldId);
            break;
        case ResponseAction::WorldSetLockedEdit:
            body = buildWorldSetLockedEdit(clientMutationId, worldId, newUserLockedEdit);
            break;
        case ResponseAction::Inventory:
            body = buildInventory();
            break;
        case ResponseAction::ItemDefs:
            body = buildItemDefs(itemDefIds);
            break;
        case ResponseAction::WorldsApps:
            body = buildWorldsApps(variablesJson);
            break;
        case ResponseAction::WorldsGuestApps:
            body = buildWorldsGuestApps(variablesJson);
            break;
        case ResponseAction::Canned:
            body = buildCanned(docId, clientMutationId, worldNodeId);
            break;
        case ResponseAction::SetUserOptions:
            // buildSetUserOptions already frames its own HTTP response
            return buildSetUserOptions(clientMutationId, userOptionsB64);
        default:
            return std::string();
        }
        
        if (body.empty())
        {
            return std::string();
        }
        return FrameHttp(body);
    }

}
