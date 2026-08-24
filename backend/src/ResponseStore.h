#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

#include "json11.hpp"

namespace home2hook {

    // Payload-replace classes for the game's OpenSSL 1.0.2 GraphQL channel. Everything not listed here (incl. /verts/connect and the 1.1.0h/1.1.0d layers) can pass through untouched to the live backend.
    enum class ResponseAction {
        PassThrough,
        WorldLogin,      // 2474314252665145 (templated: server_time=now, echo cmid)
        WorldKeepAlive,  // 2784629574894815 (generated)
        WorldsPoll,      // 2324561257653109 (folder-backed node per requested id, else synth)
        WorldsList,      // 2517010291730152 (one node per store\worlds\world_<id> folder)
        WorldContent,    // 2021902227865170 (objects and customizations from the folder config.json)
        DefaultWorld,    // 3313418545373770 (preferences.defaultWorldId, live-mutable)
        WorldCreate,     // 2229535317067212 (mint id, folder and config.json, then ack)
        SetDefaultWorld, // 3121238591227357 (atomic-write defaultWorldId, live-mutable)
        UpdateNameWorld, // 2254519954639578 (b64 then urldecode name, then config.json)
        WorldBatchUpdate,// 3393266397380374 (merge object create/update/delete and customizations into config.json)
        WorldLikeToggle, // 2285528538195458 (toggle is_liked/like_count into config.json)
        WorldDelete,     // 2213511855433466 (delete world folder and drop entry)
        WorldSetLockedEdit,// 2507097196064477 (persist user_locked_edit into config.json)
        Inventory,       // 3098640583495832 (DB-backed from the Owned set)
        ItemDefs,        // 2340400929361818 (DB-backed per requested item_def_ids[])
        WorldsApps,      // 3420023344706951 (app/achievement tile metadata, empty-but-valid or library-backed)
        WorldsGuestApps, // 2426930340689946 (guest variant of the above)
        Canned,          // disk template keyed by doc_id (with optional discriminator)
        SetUserOptions   // 2659373614156793 (base64-decode, persist to prefs, then ack)
    };

    const char* ActionName(ResponseAction action);

    // DLL-owned Response Store: loaded from the store directory next to the DLL. Holds the world_login template, the canned template set, the Master Item DB, and the Owned set
    class ResponseStore {
    public:
        // storeDir contains: templates\ (world_login.json, default_world.json, doc-id-named canned responses), item-definitions.master.json, inventory-owned.json.
        // Returns true if it loaded enough to be useful w/ a world_login template present.
        bool Load(const std::wstring& storeDir);

        ResponseAction Classify(const std::string& docId) const;

        // Builds a complete Content-Length-framed HTTP/1.1 response for docId, using variablesJson to echo ids / select the discriminated template.
        // Returns "" if nothing can be built (e.g. an unmatched world_node_id) so the caller falls back to passthrough method
        std::string BuildResponse(ResponseAction action, const std::string& docId, const std::string& variablesJson) const;

        // OAF /library/fetchall reply: the user's Oculus app library, built from apps-library.json, so the in-Home inventory App Library lists a user's games OafRewrite calls this for that route.
        // btw: seq/ts are this exchange's OafIpc sequenceId/timestamp.
        std::string BuildOafLibraryReply(const std::string& seq, const std::string& ts) const;

        // REST uploads (TlsServer routes non-graphql multipart POSTs here): write the raw image bytes into store\worlds\world_<worldId>\screenshot.jpg "isScreenshot" or cubemap.jpg atomically
        bool WriteWorldMedia(const std::string& worldId, bool isScreenshot, const std::string& bytes);

    private:
        // per-world folder model: one entry per store\worlds\world_<id> folder.
        struct WorldEntry
        {
            std::string worldId;   // folder id, the <world_id>
            std::wstring folder;   // abs path store\worlds\world_<id>
            std::string name;     // config.json "name" (raw / decoded)
            json11::Json config;   // full parsed config.json (objects/customizations/indices)
        };
        std::string worldLoginTemplate;
        bool worldLoginLoaded = false;

        // Canned templates keyed by file stem ("<doc_id>" or "<doc_id>__<discriminator>").
        std::unordered_map<std::string, std::string> cannedTemplates;
        // doc_ids that have at least one canned template
        std::unordered_set<std::string> cannedDocIds;

        json11::Json masterDb;   // item_def_id to WorldsItemDefinition node
        bool masterLoaded = false;
        json11::Json ownedItems; // array of { item_def_id, asset_key, owned_count }
        bool ownedLoaded = false;

        // The user's local Oculus app library, used to reconstruct app/achievement tile metadata for GameBox/cartridge/achievement objects (worlds_apps_and_achievements response)
        //When absent the handlers still serve a valid empty string so the tiles render blank instead of the game crashing on a Null-as-String field.
        // Schema: { "apps": [ { "ID","Canonical","Title", "AcquiredTime", "SquareURI"/"IconURI"/... (raw urls, base64-encoded at serve time), "Achievements":[ {"ID","Title","Description","UnlockTime","UnlockedURI"} ] } ] }.
        json11::Json appLibrary; // the object with "apps": array
        bool appLibraryLoaded = false;

        // served inventory-entry id (node "id", DeriveInventoryEntryId(def id)) maps to def id. Built at Load from ownedItems, and lets the create path recover item_definition.id from the wire inventory_item_id, which is now the entry id, not the def id.
        std::unordered_map<std::string, std::string> inventoryEntryToDefId;

        // Identity (from preferences.json.identity, de-identified fallbacks in Load).
        // These fill __OWNER_ID__/__DISPLAY_NAME__/__OCULUS_ID__ across canned templates and the generated worlds-poll node, so owner_id matches string(kUserId) by construction.
        std::string identityUserId;
        std::string identityDisplayName;
        std::string identityOculusId;

        // Raw (unescaped) user_options JSON served in world_login, round-tripped by the set_user_options write path.
        mutable std::string userOptionsJson;

        // Portable-folder preferences.json (sibling of store\), rewritten by set_user_options.
        std::wstring prefsPath;
        mutable std::mutex userOptionsMutex;

        mutable std::mutex gapsMutex;
        mutable std::unordered_set<std::string> loggedGaps;
        mutable std::unordered_set<std::string> loggedCubemaps; // one cubemap-serve log per world

        // World folders (scanned at Load, appended to by world_create under worldsMutex).
        // worldsLoaded=false falls back to the static worlds-list/content/default templates.
        // mutable: the const build* mutation handlers append or edit entries in place.
        mutable std::vector<WorldEntry> worlds;
        mutable std::mutex worldsMutex;
        std::wstring worldsDir;              // storeDir\worlds
        std::wstring appDir;                // storeDir parent (portable-folder root, images\ lives here)

        // UGC (user-uploaded item/place) defs loaded from each world's ugc\ugc-hashes.json at Load.
        // buildItemDefs serves these (hash_from_client with file:// asset uris) for their def ids so the game maps the UGC def to its cached blob. Empty when no world has UGC.
        std::unordered_map<std::string, json11::Json> ugcDefs;// def_id to stored item-def node
        std::unordered_map<std::string, std::string> ugcZstUri;//hash_from_client to file:// .zst uri

        // The user's full uploaded-UGC inventory (single source, portable-root ugc-hashes-global.json, maintained by the wrapper).
        // Loaded once at Load. Its def ids are advertised in the offline inventory, driving the game "Uploaded above 0" UGC-creator state that unlocks the UGC-place lighting editor, and seeded into ugcDefs each loadWorlds so item-defs resolve them.
        json11::Json globalUgcManifest;
        mutable bool worldsLoaded = false; // by world_create (first from-empty create)

        // preferences.defaultWorldId, live-mutable so an in-VR set_default_world is reflected by a same-session default-world read (in-VR overrides the wrapper-set default
        mutable std::string defaultWorldId;
        mutable std::mutex defaultWorldMutex;
        mutable std::mutex prefsFileMutex;// serializes all preferences.json load-modify-write

        // Double-create collapse: the game issues world_create twice per in-VR create, roughly 2s apart for some reason...
        // A create within kCreateCollapseMs of the previous real create re-acks that world instead of minting a new second folder
        mutable long long lastCreateTimeMs = 0; // GetTickCount64 of the last real create
        mutable std::string lastCreateWorldId;// world id of the last real create
        mutable int worldCreateLogCount = 0; // one-time-ish request-variable logging counter

        std::string buildWorldLogin(const std::string& clientMutationId) const;
        std::string buildKeepAlive(const std::string& clientMutationId) const;
        std::string buildWorldsPoll(const std::vector<std::string>& worldNodeIds) const;
        std::string buildInventory() const;

        //Maps a served inventory-entry id back to its def id (or "" if unknown), used by the world_batch_update create path to fill item_definition.id.
        std::string defIdForInventoryEntry(const std::string& entryId) const;
        std::string buildItemDefs(const std::vector<std::string>& itemDefIds) const;

        // App/achievement tile metadata (GameBox / cartridge / achievement objects)
        std::string buildWorldsApps(const std::string& variablesJson) const; // non-guest: string("[{...},...]")
        std::string buildWorldsGuestApps(const std::string& variablesJson) const;// guest (no library apps discovered): string({"apps":[...],"achs":[...]})
    
        // Build a wire app-object (base64-encodes its raw URI fields) for def id, or Json() if the library has no such app. requestedIds filters the library to what the world actually placed.
        json11::Json buildAppNode(const json11::Json& libApp) const;

        // One OAF LIBRARY_UPDATE entitlement node (full ~60-field schema, per-app fields from libApp, rest constant defaults captured from a live library reply).
        json11::Json buildOafEntitlement(const json11::Json& libApp) const;

        // Requested app ids from the variables. base64Encoded handles the guest app_ids_encoded blob,else parses the non-guest app_ids_json string. Tolerant of the wire's non-standard JSON (unquoted keys / single quotes) by scanning for all-digit quoted tokens.
        static std::vector<std::string> parseRequestedAppIds(const std::string& variablesJson, bool base64Encoded);
        std::string buildCanned(const std::string& docId, const std::string& clientMutationId, const std::string& worldNodeId) const;
        std::string buildSetUserOptions(const std::string& clientMutationId, const std::string& base64UserOptions) const;
        void persistUserOptions(const json11::Json& userOptions) const;
        void noteGap(const std::string& itemDefId) const;

        // folder serve/mutation backend.
        void loadWorlds();
        void loadWorldUgc(const std::wstring& folder); // merge a world's ugc\ugc-hashes.json into ugcDefs
        void augmentInventoryFromWorldUgc(); // own each world's placed UGC objects (editable-UGC gate)
        void rebuildInventoryReverseMap(); // entry-id to def-id from final ownedItems (honors entry_id)
        const WorldEntry* findWorldLocked(const std::string& worldId) const; // caller holds worldsMutex
        std::string screenshotFileUri(const WorldEntry& entry) const;
        std::string cubemapUriBase64(const WorldEntry& entry) const;// "" if no cubemap.dds
        void noteCubemapServe(const std::string& worldId) const;
        json11::Json buildWorldNodeJson(const WorldEntry& entry, bool asWorldTypename) const;
        std::string buildWorldsList() const;
        std::string buildWorldContent(const std::string& worldNodeId, const std::string& clientMutationId) const;
        std::string resolveDefaultWorldId() const;// serve-time defensive, "" only if no valid world
        std::string buildDefaultWorld() const;
        std::string buildWorldCreate(const std::string& clientMutationId, const std::string& variablesJson) const;
        std::string buildSetDefaultWorld(const std::string& clientMutationId, const std::string& worldId) const;
        std::string buildUpdateNameWorld(const std::string& clientMutationId, const std::string& worldId, const std::string& nameBase64) const;
        std::string buildWorldBatchUpdate(const std::string& clientMutationId, const std::string& worldId, const std::string& worldCustomizationsB64, const json11::Json& createArr, const json11::Json& updateArr, const json11::Json& deleteArr) const;
        std::string buildWorldLikeToggle(const std::string& clientMutationId, const std::string& worldId) const;
        std::string buildWorldDelete(const std::string& clientMutationId, const std::string& worldId) const;
        std::string buildWorldSetLockedEdit(const std::string& clientMutationId, const std::string& worldId, bool newLockedEdit) const;
        json11::Json buildCanonicalConfig(const std::string& worldId, int creationIndex, int nameIndex) const;
        std::string mintNumericId() const; // 16-digit '8'-prefixed numeric string
        std::string mintWorldId() const;// mintNumericId collision-checked against folders
        bool writeFileAtomic(const std::wstring& path, const std::string& content) const;
        void persistPrefsField(const std::string& key, const json11::Json& value) const;
    };

    extern ResponseStore GStore;
}
