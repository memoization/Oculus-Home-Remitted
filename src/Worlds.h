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

    // Auto-seed the default world. mint id, atomic config.json from the store's empty template, copy world-default.png to screenshot.png and set it default iff store\worlds\ has no world_* folder yet
    void SeedDefaultIfEmpty();

    // Keep preferences.defaultWorldId pointing at a world that still exists on disk. If store\worlds\ is empty, auto-seed the default or else if the recorded default is  blank or its folder is gone, adopt an existing world as the default.
    // Call before a scan so deleting the default (or all) world folders while the app runs self-heals.
    void EnsureValidDefault();

    // copy every world's downloaded UGC assets (store\worlds\world_*\ugc\*.zst) into %LOCALAPPDATA%\Home2\WorldsCache (skips any already present) so the game loads them locally instead of trying to re-download.
    // Mainly called at app startup & whenever Home launches.
    void PopulateUgcCache();
}
