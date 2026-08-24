#pragma once
#include <string>
#include <vector>

// Oculus app-library reconstruction for the offline "Apps Library" page.
//
// The game asks the backend to resolve the app tiles placed in a world: GameBox, game cartridge and achievement objects (graphql worlds_apps_and_achievements).
// While offline, that list is empty, and the game can crash reading a missing field as a string. This module rebuilds the list purely from the local Oculus install so those tiles resolve to real titles and cover art.
//
// Sources (each library root holds Manifests\ and Software\StoreAssets\):
//   * Manifests\<canonical>.json / <canonical>_assets.json for appId and canonicalName
//   * Software\StoreAssets\<canonical>_assets\cover_square_image.jpg (with landscape and icon) for thumbnails
// The default download location "C:\Program Files\Oculus\CoreData" is always scanned plus any user-added roots. Manifests with a null appId are ignored
namespace applibraries {

    // One resolved Oculus app. Matches the expected schema the real backend would serv. URIs are raw file:// urls here, the backend base64-encodes them at serve time
    // No portrait/screenshot images exist locally since screenshots are CDN-only and not easily retrievable
    // PortraitURI reuses cover_square and the two Screenshot URIs reuse cover_landscape. That's enough to fill every GameBox face.
    struct AppEntry {
        std::string id;            // numeric Oculus appId
        std::string canonical;     // base canonical ("_assets" suffix stripped)
        std::string title;         // real title naming (LoadOafDisplayNames)
        long long   acquiredTime;  // synthetic acquire time (manifest mtime, unix seconds), the game reads it as a string
        std::string squareUri;     // file:/// cover_square (may be empty)
        std::string portraitUri;   // file:/// cover_square (front cover, no local portrait asset)
        std::string landscapeUri;  // file:/// cover_landscape (may be empty)
        std::string iconUri;       // file:/// icon (may be empty)
        std::string screenshot0Uri;
        std::string screenshot1Uri;
    };

    // The always-scanned default Oculus download location (in UTF-8)
    std::string DefaultRoot();

    // Scan the default CoreData location and userRoots, dedup unique apps by appId, drop null-appId manifests. Sorted by title (case-insensitive).
    std::vector<AppEntry> Scan(const std::vector<std::string>& userRoots);

    // Scan and rewrite <AppDir>\store\apps-library.json ( "{"apps":[...]}" ). Returns the number of apps written, or -1 if the file could not be written.
    int Rebuild(const std::vector<std::string>& userRoots);

}
