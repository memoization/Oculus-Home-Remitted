#pragma once
#include <string>
#include <vector>
#include <atomic>

// Oculus app-library reconstruction for the offline "Apps Library" page.
//
// The game asks the backend to resolve app tiles placed in a world, GameBox, game cartridge and achievement objects and to populate the portal destination selector's App Library category.
// While offline that list is empty, and the game can crash reading a missing field as a string. This module rebuilds the list from what the local machine already knows.
//
// Two tiers:
//   Owned apps are the source of truth. They come from the Oculus offline cache %APPDATA%\Oculus\sessions\_oaf\data.sqlite (Objects rows typename Application), which yields the appId, canonical_name, display name and the two oculuscdn cover uris. Every cached app becomes a library entry whether or not it is installed, so the user can point a portal at any app they own.
//   Installed apps are the owned apps that also have a local manifest under a library root's Manifests folder. The manifest inserts crucial launch details such as launchFile, launchParameters and the install folder so the backend can launch the app manually. An owned app with no matching manifest stays in the library but has no launch fields.
// The default location "C:\Program Files\Oculus\Software" is always scanned plus any user-added library folders. Manifests for apps not present in the oaf cache are ignored.
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
        std::string launchFile;       // manifest launchFile, the exe name
        std::string launchParameters; // manifest launchParameters, may be empty for some apps
        std::string installDir;       // <root>\Software\<canonical title>, the folder the exe lives in, empty when not installed
    };

    // Define owned vs installed split for the Apps Library page
    struct LibraryCounts
    {
        int owned = 0;
        int installed = 0;
    };

    // Live progress for the app builder
    struct Progress
    {
        std::atomic<int> total{ 0 };
        std::atomic<int> done{ 0 };
    };

    struct RebuildResult
    {
        bool success = false; // the apps-library.json file was written
        int owned = 0; // apps written
        int installed = 0; // apps that also have launch fields
        int imageFailures = 0; // covers with a uri that failed to download
        std::string error;
    };

    // The always-scanned default Oculus download location (in UTF-8)
    std::string DefaultRoot();

    // Build the app list. Every app in the oaf offline cache becomes an entry, then manifests under DefaultRoot and userRoots key launch details into the ones that are installed.
    std::vector<AppEntry> Scan(const std::vector<std::string>& userRoots, Progress* progress = nullptr, int* imageFailures = nullptr, std::string* oafError = nullptr);

    // Count the apps in the current store\apps\apps-library.json without rescanning. Zeroes uf the file is missing or empty.
    LibraryCounts CountApps();

    // True when store\apps\apps-library.json exists, telling missing (never built) apart from a built but empty library. The Apps page uses it to auto-build on first visit.
    bool LibraryFileExists();

    // Scan and rewrite <AppDir>\store\apps\apps-library.json ( "{"apps":[...]}" )
    RebuildResult Rebuild(const std::vector<std::string>& userRoots, Progress* progress = nullptr);

}
