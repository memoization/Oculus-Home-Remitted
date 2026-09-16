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
//   1 - Fetching all owned apps. They are sourced from the Oculus offline cache %APPDATA%\Oculus\sessions\_oaf\data.sqlite (Objects rows typename Application), which provides the appId, canonical name and display name. Every cached app becomes a library entry whether or not it is installed, so the user can point a portal at any app they own. The store art (square, landscape, portrait and screenshots) is fetched per app from the live backend.
//   2 - Installed apps are the owned apps that also have a local manifest under a library root's Manifests folder. The manifest inserts crucial launch details such as launchFile, launchParameters and the install folder so the backend can launch the app manually. An owned app with no matching manifest stays in the library but has no launch fields.
// The default location "C:\Program Files\Oculus\Software" is always scanned plus any user-added library folders. Manifests for apps not present in the oaf cache are ignored.
namespace applibraries {

    // One resolved Oculus app. It mirrors the schema an online session would serve. URIs are stored as relative paths and the backend resolves them to absolute file:// urls at serve time.
    // Square, landscape, portrait and the two screenshots each come from their own backend query. During failure, portrait falls back to the square cover and each screenshot falls back to the landscape cover, which is enough to fill every game box face.
    struct AppEntry {
        std::string id;            // numeric Oculus appId
        std::string canonical;     // base canonical ("_assets" suffix stripped)
        std::string title;         // real display title from the oaf cache
        long long   acquiredTime;  // synthetic acquire time (manifest mtime, unix seconds), the game reads it as a string
        std::string squareUri;     // square cover
        std::string portraitUri;   // portrait cover for game boxes
        std::string landscapeUri;  // landscape cover
        std::string iconUri;       // square cover reused as the icon
        std::string screenshot0Uri;
        std::string screenshot1Uri;
        std::string launchFile;       // manifest launchFile being the exe name
        std::string launchParameters; // manifest launchParameters, empty for some apps
        std::string installDir;       // <root>\Software\<canonical title>, the folder the exe lives in, empty when not installed
    };

    // The graph.oculus.com queries for an app's store art. All need a valid access_token and take the numeric app id.
    static const char* kDocAppSquare = "9909078855809275";  // expected square cover 720x720, response node.cover_square_image.uri, var applicationID
    static const char* kDocAppLandscape = "24245537131714421"; // expected landscape cover 720x405, response node.cover_landscape_image.uri, var applicationID
    static const char* kDocAppCover = "2562966287070020";  // portrait cover, response node.images.uri, vars size and app_id
    static const char* kDocAppScreenshots = "2841468849260027";  // screenshots, response node.images is a list of uri, vars size and app_id

    // oculuscdn urls for an app's covers and first two screenshots. Any field left empty on a failure falls back later.
    struct AppArt
    {
        std::string squareUrl;
        std::string landscapeUrl;
        std::string portraitUrl;
        std::string shot0Url;
        std::string shot1Url;
    };

    // Define specific pieces of art to request. A piece already cached on disk is not asked for again.
    struct ArtWants
    {
        bool square = false;
        bool landscape = false;
        bool portrait = false;
        bool shots = false;
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

    // The default Oculus download location (in UTF-8)
    static std::string DefaultRoot = "C:\\Program Files\\Oculus\\Software";

    // Build the app list. Every app in the oaf offline cache becomes an entry, then manifests under DefaultRoot and userRoots key launch details into the ones that are installed.
    std::vector<AppEntry> Scan(const std::vector<std::string>& userRoots, Progress* progress = nullptr, int* imageFailures = nullptr, std::string* oafError = nullptr);

    // Count the apps in the current store\apps\apps-library.json without rescanning. Zeroes uf the file is missing or empty.
    LibraryCounts CountApps();

    // True when store\apps\apps-library.json exists, telling missing (never built) apart from a built but empty library. The Apps page uses it to auto-build on first visit.
    bool LibraryFileExists();

    // Scan and rewrite <AppDir>\store\apps\apps-library.json ( "{"apps":[...]}" )
    RebuildResult Rebuild(const std::vector<std::string>& userRoots, Progress* progress = nullptr);

}
