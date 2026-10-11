#pragma once
#include <string>
#include <vector>

// An offline-supported worlds model (portable-folder store\worlds\world_<id>\).
// The app is list, select, and "Set Default" only the backend owns a config.json and the app writes only preferences.defaultWorldId
namespace worlds
{
    struct WorldCardInfo
    {
        std::string worldId;
        std::string name;   // config.json "name" ("" means the UI shows "Home #<nameIndex>")
        std::string screenshotPng;// narrow abs path to a picopng-loadable PNG ("" if none)
        int objectCount = 0;
        int creationIndex = 0;// config.json "creation_index" (monotonic, ordering/default pick)
        int nameIndex = 0;    // config.json "name_index" (the "Home #NN" display title)
        bool isDefault = false;

        // UGC (user-generated content) traces
        int ugcObjectCount = 0;
        bool ugcBase = false;
    };

    // Enumerate store\worlds\world_*, read each config.json, resolve the display screenshot to a PNG (transcoding the folder JPG to a screenshot.png when missing/stale). Must be called on the render thread when it will feed the texLoader
    std::vector<WorldCardInfo> Scan();

    // Rewrite the "name" field of a world config.json to a new name. Returns false when the world folder or its config.json is missing or unparseable. An empty newName reverts the card to the "Home #<nameIndex>" title.
    bool RenameWorld(const std::string& worldId, const std::string& newName);

    // Auto-seed the default world. mint id, atomic config.json from the store's empty template, copy world-default.png to screenshot.png and set it default iff store\worlds\ has no world_* folder yet
    void SeedDefaultIfEmpty();

    // Keep preferences.defaultWorldId pointing at a world that still exists on disk. If store\worlds\ is empty, auto-seed the default or else if the recorded default is  blank or its folder is gone, adopt an existing world as the default.
    // Call before a scan so deleting the default (or all) world folders while the app runs self-heals.
    void EnsureValidDefault();

    // copy every world's downloaded UGC assets (store\worlds\world_*\ugc\*.zst) into %LOCALAPPDATA%\Home2\WorldsCache (skips any already present) so the game loads them locally instead of trying to re-download.
    // Mainly called at app startup & whenever Home launches.
    void PopulateUgcCache();

    // An imported UGC definition from store\uploaded-ugc\import-hashes-global.json
    struct ImportInfo
    {
        std::string defId; // the UGC def id
        std::string typeName; // "WorldsUGCItemDefinition" (object) or "WorldsUGCPlaceDefinition" (place template)
        std::string name; // display name
        std::string hash;// hash_from_client, which gets tied to the .zst in uploaded-ugc
        unsigned long long zstBytes = 0; // size of store\uploaded-ugc\<hash>.zst, 0 if missing
        unsigned long long createdTime = 0; // json created_time (unix seconds), 0 if the entry has none
    };

    // Read the import hashes json and return every imported def
    std::vector<ImportInfo> ScanImports();

    // Package a home folder into a single portable .ochome file at outPath as a zip renamed extension.
    // The world's placed UGC objects and any UGC base map are resolved to their blobs from store\uploaded-ugc and collected into the home archive's own ugc folder along with an updated ugc-hashes.json, so the exported home is self contained.
    bool ExportHome(const std::string& worldId, const std::wstring& outPath);

    // Extract a .ochome file into store\worlds.
    bool ImportHome(const std::wstring& inPath, std::string* importedWorldId = nullptr);

    // Remove an imported UGC def by id everywhere, same as the backend salvage. Only safe while Home is not running, since clashes can happen w/ the backend
    // Drops placed instances from every world config, for a place template, it also clears customizations.UGCBase and the entry-point object so the world reverts to the default room.
    // 
    // Removes the def from each world's ugc-hashes.json and deletes the per-world blob, deletes the blob from WorldsCache and uploaded-ugc, and drops the def from the global import hashes json.
    bool DeleteImport(const std::string& defId);
}
