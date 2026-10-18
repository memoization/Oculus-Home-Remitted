#define WIN32_LEAN_AND_MEAN
#include "AppLibraries.h"
#include "Prefs.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include "json11.hpp"
#include <cpr/cpr.h>
#include "HomeLogger.h"
#include "FetchWorlds.h"
#include "ImageConv.h"
#include "SteamLibrary.h"
#include "UI.h"

namespace fs = std::filesystem;

namespace applibraries {

    // ---- Extract library display names from the Oculus cache ----
    // %APPDATA%\Oculus\sessions\_oaf\data.sqlite (Objects table) caches the full app catalog
    // A string-scalar field in the blob is <u32 nameLen><name>\x01\x01<u64 valLen><value>. Return the value for `name`, or "" if not present as a string scalar.
    static std::string BlobStringField(const unsigned char* blob, size_t len, const char* name, size_t start = 0)
    {
        size_t nlen = std::strlen(name);
        std::string needle;
        needle.push_back((char)(nlen & 0xFF));
        needle.push_back((char)((nlen >> 8) & 0xFF));
        needle.push_back((char)((nlen >> 16) & 0xFF));
        needle.push_back((char)((nlen >> 24) & 0xFF));
        needle.append(name, nlen);
        needle.push_back(0x01);
        needle.push_back(0x01);

        for (size_t i = start; i + needle.size() + 8 <= len; ++i)
        {
            if (std::memcmp(blob + i, needle.data(), needle.size()) != 0) continue;

            size_t p = i + needle.size();
            unsigned long long vlen = 0;
            for (int k = 0; k < 8; ++k)
            {
                vlen |= (unsigned long long)blob[p + k] << (8 * k);
            }

            p += 8;
            if (vlen == 0 || vlen > 4096 || p + vlen > len) return std::string();

            return std::string((const char*)blob + p, (size_t)vlen); // values are UTF-8
        }
        return std::string();
    }

    // Per-app info pulled from the oaf cache: canonical name and display name
    struct OafApp
    {
        std::string canonicalName;
        std::string displayName;
    };

    static std::map<std::string, OafApp> LoadOafApps(std::string* errorOut = nullptr)
    {
        std::map<std::string, OafApp> out;

        wchar_t appdata[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
        {
            if (errorOut) *errorOut = "Could not locate the Oculus data folder.";
            return out;
        }

        std::wstring path = std::wstring(appdata) + L"\\Oculus\\sessions\\_oaf\\data.sqlite";
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
        {
            if (errorOut) *errorOut = "Oculus app cache not found. Open Meta Link at least once so it can build the cache then try again.";
            return out;
        }

        HMODULE h = LoadLibraryW(L"winsqlite3.dll");
        if (!h)
        {
            if (errorOut) *errorOut = "Could not load winsqlite3 to read the Oculus app cache.";
            return out;
        }

        struct sqlite3; struct sqlite3_stmt;
        auto open_v2   = (int(*)(const char*, sqlite3**, int, const char*))GetProcAddress(h, "sqlite3_open_v2");
        auto prepare   = (int(*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**))GetProcAddress(h, "sqlite3_prepare_v2");
        auto step      = (int(*)(sqlite3_stmt*))GetProcAddress(h, "sqlite3_step");
        auto col_text  = (const unsigned char*(*)(sqlite3_stmt*, int))GetProcAddress(h, "sqlite3_column_text");
        auto col_blob  = (const void*(*)(sqlite3_stmt*, int))GetProcAddress(h, "sqlite3_column_blob");
        auto col_bytes = (int(*)(sqlite3_stmt*, int))GetProcAddress(h, "sqlite3_column_bytes");
        auto finalize  = (int(*)(sqlite3_stmt*))GetProcAddress(h, "sqlite3_finalize");
        auto close_db  = (int(*)(sqlite3*))GetProcAddress(h, "sqlite3_close");

        if (open_v2 && prepare && step && col_text && col_blob && col_bytes && finalize && close_db)
        {
            // file: URI with immutable=1 so there is no locking against a live Oculus writer
            std::string uri = "file:///";
            for (char c : prefs.Narrow(path))
            {
                unsigned char u = (unsigned char)c;
                if (c == '\\' || c == '/') uri += '/';
                else if (std::isalnum(u) || c == '.' || c == '-' || c == '_' || c == ':') uri += c;
                else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", u); uri += b; }
            }
            uri += "?immutable=1";

            const int SQLITE_OK = 0, SQLITE_ROW = 100;
            const int OPEN_READONLY = 0x00000001, OPEN_URI = 0x00000040;
            sqlite3* db = nullptr;
            if (open_v2(uri.c_str(), &db, OPEN_READONLY | OPEN_URI, nullptr) == SQLITE_OK && db)
            {
                sqlite3_stmt* st = nullptr;
                const char* sql = "SELECT hashkey,value FROM Objects WHERE typename='Application'";

                if (prepare(db, sql, -1, &st, nullptr) == SQLITE_OK)
                {
                    while (step(st) == SQLITE_ROW)
                    {
                        const unsigned char* hk = col_text(st, 0);
                        const unsigned char* blob = (const unsigned char*)col_blob(st, 1);
                        int blen = col_bytes(st, 1);

                        if (!hk || !blob || blen <= 0) continue;

                        OafApp a;
                        a.canonicalName = BlobStringField(blob, (size_t)blen, "canonical_name");
                        a.displayName = BlobStringField(blob, (size_t)blen, "display_name");
                        out[std::string((const char*)hk)] = a;

                        homeLogger.write() << "AppLibraries: Found app: " << a.displayName.c_str() << std::endl;
                    }
                    finalize(st);
                }
                close_db(db);
            }
            else
            {
                if (errorOut) *errorOut = "Could not open the Oculus app cache.";
            }
        }

        FreeLibrary(h);
        return out;
    }

    std::string ReadFileUtf8(const fs::path& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::string();

        std::ostringstream ss;
        ss << f.rdbuf();

        return ss.str();
    }

    // appId is a num in the full <canonical>.json manifest and a string in <canonical>_assets.json.
    // But return the digits either way and empty when null/absent. Real Oculus/Facebook app ids for json11 is lossless.
    static std::string AppIdString(const json11::Json& m)
    {
        const json11::Json& a = m["appId"];
        if (a.is_string()) 
        {
            return a.string_value();
        }

        if (a.is_number())
        {
            return std::to_string((long long)a.number_value());
        }

        return std::string();
    }

    static std::string Prettify(const std::string& canon)
    {
        std::string out;
        out.reserve(canon.size());
        bool newWord = true;
        for (char c : canon)
        {
            if (c == '-')
            {
                out += ' ';
                newWord = true;
            }
            else if (newWord)
            {
                out += (char)std::toupper((unsigned char)c);
                newWord = false;
            }
            else
            {
                out += (char)std::tolower((unsigned char)c);
            }
        }
        return out;
    }

    static std::string BaseCanonical(std::string c)
    {
        static const std::string suffix = "_assets";
        if (c.size() >= suffix.size() && c.compare(c.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            c.resize(c.size() - suffix.size());
        }

        return c;
    }

    // Manifest last-write time as unix seconds. A value for "AcquiredTime" field.
    // The game reads AcquiredTime and throws a warning "Null used as a String" if it is absent.
    long long FileUnixTime(const fs::path& p)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &fad))
        {
            ULARGE_INTEGER u;
            u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fad.ftLastWriteTime.dwHighDateTime;

            // Time is 100ns ticks since 1601-01-01 | unix epoch offset = 11644473600 s.
            if (u.QuadPart >= 116444736000000000ULL)
            {
                return (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
            }
        }
        return 1451606400LL; // 2016-01-01 fallback
    }

    // Absolute path to a file:/// URI. Backslashes to forward slashes, spaces percent-encoded
    static std::string ToFileUri(const fs::path& absPath)
    {
        std::string p = prefs.Narrow(absPath.wstring());
        std::string enc = "file:///";
        for (char c : p)
        {
            if (c == '\\') enc += '/';
            else if (c == ' ') enc += "%20";
            else enc += c;
        }
        return enc;
    }

    // Image extension from a url's path (before the query), defaults to .png.
    std::string ImageExtFromUrl(const std::string& url)
    {
        size_t q = url.find('?');
        std::string path = (q == std::string::npos) ? url : url.substr(0, q);
        size_t dot = path.find_last_of('.');
        size_t slash = path.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash) && path.size() - dot <= 5)
            return path.substr(dot);
        return ".png";
    }

    // Download url to dest, atomically. The fetch is skipped when dest already exists and either maxAgeSec is 0 or the file is younger than maxAgeSec.
    // A stale file is re-fetched, but kept as-is if the fetch fails
    bool DownloadImage(const std::string& url, const fs::path& dest, long long maxAgeSec = 0)
    {
        std::error_code ec;
        bool exists = fs::is_regular_file(dest, ec);
        if (exists)
        {
            if (maxAgeSec <= 0) return true;
            if ((long long)std::time(nullptr) - FileUnixTime(dest) < maxAgeSec) return true; // fresh enough
        }
        if (url.empty()) return exists;

        std::string downloadUrl = url;

        constexpr std::string_view badHost = "scontent-atl3-3.oculuscdn.com"; // This endpoint tends to NXDOMAIN fail
        constexpr std::string_view replacementHost = "scontent-atl3-1.oculuscdn.com";

        if (const size_t pos = downloadUrl.find(badHost); pos != std::string::npos)
        {
            downloadUrl.replace(pos, badHost.length(), replacementHost);
        }

        homeLogger.write() << "AppLibraries: Getting image from url: " << downloadUrl.c_str() << std::endl;

        cpr::Response r = cpr::Get(cpr::Url{ downloadUrl }, cpr::Timeout{ 15000 });
        if (r.status_code != 200 || r.text.empty())
        {
            homeLogger.write() << "AppLibraries: Failed to download image from url: " << downloadUrl.c_str() << " | err: " << r.error.message.c_str() << std::endl;
            return exists; // keep any current file
        }

        // Write a temp file then atomically replace so a stale cover is swapped only on a successful fetch.
        fs::path tmp = dest;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return exists;
            f.write(r.text.data(), (std::streamsize)r.text.size());
            if (!f.good())
            {
                f.close();
                std::error_code e2;
                fs::remove(tmp, e2);
                return exists;
            }
        }

        if (!MoveFileExW(tmp.wstring().c_str(), dest.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code e2;
            fs::remove(tmp, e2);
            return exists;
        }
        return true;
    }

    // True when a image path points to a file that exists and is younger than ImageMaxAgeSec.
    bool CachedImageFresh(const fs::path& imagesDir, const std::string& storeRel)
    {
        if (storeRel.empty()) return false;

        std::string fn = storeRel.substr(storeRel.find_last_of('/') + 1);
        fs::path p = imagesDir / prefs.Widen(fn);

        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return false;
        return ((long long)std::time(nullptr) - FileUnixTime(p)) < ImageMaxAgeSec;
    }

    // Relative store path of a downloaded app image named "<id><suffix>.<ext>", or "" when none is cached.
    std::string FindCachedImage(const std::vector<std::string>& files, const std::string& id, const std::string& suffix)
    {
        std::string prefix = id + suffix + ".";
        for (const auto& name : files)
        {
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
                return "apps/images/" + name;
        }
        return std::string();
    }

    // Query the live backend for an app's covers and screenshots.
    // token is the user's cached access_token, appId is the numeric id.
    // The want flags control each request so cached art is not fetched again. It returns empty urls if the token is absent or a query fails.
    static AppArt FetchAppArt(const std::string& token, const std::string& appId, const ArtWants& want)
    {
        AppArt art;
        if (token.empty() || appId.empty()) return art;

        std::string err;

        if (want.square)
        {
            std::string vars = json11::Json(json11::Json::object{ { "applicationID", appId } }).dump();
            json11::Json r = fetchworlds::GraphQL(token, kDocAppSquare, vars, err);
            if (err.empty())
            {
                const json11::Json& uri = r["data"]["node"]["cover_square_image"]["uri"];
                if (uri.is_string()) art.squareUrl = uri.string_value();
            }
        }

        if (want.landscape)
        {
            err.clear();
            std::string vars = json11::Json(json11::Json::object{ { "applicationID", appId } }).dump();
            json11::Json r = fetchworlds::GraphQL(token, kDocAppLandscape, vars, err);
            if (err.empty())
            {
                const json11::Json& uri = r["data"]["node"]["cover_landscape_image"]["uri"];
                if (uri.is_string()) art.landscapeUrl = uri.string_value();
            }
        }

        if (want.portrait)
        {
            err.clear();
            std::string coverVars = json11::Json(json11::Json::object{ { "size", "252x360" }, { "app_id", appId } }).dump();
            json11::Json cover = fetchworlds::GraphQL(token, kDocAppCover, coverVars, err);
            if (err.empty())
            {
                const json11::Json& uri = cover["data"]["node"]["images"]["uri"];
                if (uri.is_string()) art.portraitUrl = uri.string_value();
            }
        }

        if (want.shots)
        {
            err.clear();
            std::string shotVars = json11::Json(json11::Json::object{ { "size", "1280x720" }, { "app_id", appId } }).dump();
            json11::Json shots = fetchworlds::GraphQL(token, kDocAppScreenshots, shotVars, err);
            if (err.empty())
            {
                const json11::Json& imgs = shots["data"]["node"]["images"];
                if (imgs.is_array())
                {
                    if (imgs.array_items().size() >= 1) art.shot0Url = imgs[0]["uri"].string_value();
                    if (imgs.array_items().size() >= 2) art.shot1Url = imgs[1]["uri"].string_value();
                }
            }
        }
        return art;
    }

    std::vector<AppEntry> ScanOculus(const std::vector<std::string>& userRoots, Progress* progress, int* imageFailures, std::string* oafError)
    {
        int failures = 0; // covers with a uri that failed to download

        // Default path first, then each user root. Dedup roots case-insensitive.
        std::vector<std::wstring> roots;
        auto addRoot = [&](const std::wstring& r)
        {
            if (r.empty()) return;

            for (const auto& e : roots)
            {
                if (_wcsicmp(e.c_str(), r.c_str()) == 0) return;
            }
            roots.push_back(r);
        };

        addRoot(prefs.Widen(DefaultRoot));
        for (const auto& u : userRoots)
        {
            addRoot(prefs.Widen(u));
        }

        // Owned apps come from the Oculus offline cache. Every cached Application becomes an entry installed or not so that any owned app can be a portal destination.
        std::string oafErr;
        std::map<std::string, OafApp> oafApps = LoadOafApps(&oafErr);
        if (oafError) *oafError = oafErr;

        // Read the user's cached access_token once. When found, each app's covers and screenshots are fetched from the live backend. A dead backend or no login just leaves the art empty and the app keeps its title only.
        fetchworlds::LocalCreds creds = fetchworlds::LoadLocalCreds();

        // Cover images download into store\apps\images and the stored field is the store-relative path. The backend resolves it to an absolute file:// at serve time so the library remains portable.
        fs::path imagesDir = fs::path(prefs.AppDir()) / L"store" / L"apps" / L"images";
        std::error_code ecdir;
        fs::create_directories(imagesDir, ecdir);

        // Snapshot what art is already downloaded so an app whose cover and screenshots are cached is not requested again from the backend at all.
        std::vector<std::string> cachedImageFiles;
        {
            std::error_code ec;
            for (fs::directory_iterator it(imagesDir, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;
                if (it->is_regular_file(ec)) cachedImageFiles.push_back(it->path().filename().string());
            }
        }

        const long long kUnknownAcquire = 1451606400LL;

        // Owned count is known now, before the slow cover downloads, so the progress bar has a total to count toward.
        if (progress) progress->total.store((int)oafApps.size());

        // One entry per owned app straight from the oaf cache. Launch fields stay empty here.
        std::map<std::string, AppEntry> byId; // appId to entry
        for (const auto& kv : oafApps)
        {
            const std::string& id = kv.first;
            const OafApp& oaf = kv.second;

            std::string base = BaseCanonical(oaf.canonicalName);

            std::string squareUri    = FindCachedImage(cachedImageFiles, id, "_square");
            std::string landscapeUri = FindCachedImage(cachedImageFiles, id, "_landscape");
            std::string portraitUri  = FindCachedImage(cachedImageFiles, id, "_portrait");
            std::string shot0Uri     = FindCachedImage(cachedImageFiles, id, "_shot0");
            std::string shot1Uri     = FindCachedImage(cachedImageFiles, id, "_shot1");

            ArtWants want;
            want.square    = !CachedImageFresh(imagesDir, squareUri);
            want.landscape = !CachedImageFresh(imagesDir, landscapeUri);
            want.portrait  = !CachedImageFresh(imagesDir, portraitUri);
            want.shots     = !CachedImageFresh(imagesDir, shot0Uri) || !CachedImageFresh(imagesDir, shot1Uri);

            if (!creds.token.empty() && (want.square || want.landscape || want.portrait || want.shots))
            {
                AppArt art = FetchAppArt(creds.token, id, want);

                if (want.square && !art.squareUrl.empty())
                {
                    std::string fn = id + "_square" + ImageExtFromUrl(art.squareUrl);
                    if (DownloadImage(art.squareUrl, imagesDir / fn, ImageMaxAgeSec)) squareUri = "apps/images/" + fn;
                    else ++failures;
                }
                if (want.landscape && !art.landscapeUrl.empty())
                {
                    std::string fn = id + "_landscape" + ImageExtFromUrl(art.landscapeUrl);
                    if (DownloadImage(art.landscapeUrl, imagesDir / fn, ImageMaxAgeSec)) landscapeUri = "apps/images/" + fn;
                    else ++failures;
                }
                if (want.portrait && !art.portraitUrl.empty())
                {
                    std::string fn = id + "_portrait" + ImageExtFromUrl(art.portraitUrl);
                    if (DownloadImage(art.portraitUrl, imagesDir / fn, ImageMaxAgeSec)) portraitUri = "apps/images/" + fn;
                    else ++failures;
                }
                if (want.shots && !art.shot0Url.empty())
                {
                    std::string fn = id + "_shot0" + ImageExtFromUrl(art.shot0Url);
                    if (DownloadImage(art.shot0Url, imagesDir / fn, ImageMaxAgeSec)) shot0Uri = "apps/images/" + fn;
                    else ++failures;
                }
                if (want.shots && !art.shot1Url.empty())
                {
                    std::string fn = id + "_shot1" + ImageExtFromUrl(art.shot1Url);
                    if (DownloadImage(art.shot1Url, imagesDir / fn, ImageMaxAgeSec)) shot1Uri = "apps/images/" + fn;
                    else ++failures;
                }
            }

            // Fall back to the square and landscape covers so every GameBox face still has an image.
            if (portraitUri.empty()) portraitUri = squareUri;
            if (shot0Uri.empty()) shot0Uri = landscapeUri;
            if (shot1Uri.empty()) shot1Uri = landscapeUri;

            AppEntry entry;
            entry.id = id;
            entry.canonical = base;
            entry.title = !oaf.displayName.empty() ? oaf.displayName : (!base.empty() ? Prettify(base) : id);
            entry.acquiredTime = kUnknownAcquire; // an installed manifest overwrites this later
            entry.squareUri = squareUri; // square cover
            entry.portraitUri = portraitUri; // portrait cover, square as fallback
            entry.landscapeUri = landscapeUri; // landscape cover
            entry.iconUri = squareUri; // square cover reused as the icon
            entry.screenshot0Uri = shot0Uri; // first screenshot, landscape as fallback
            entry.screenshot1Uri = shot1Uri; // second screenshot, landscape as fallback
            byId[id] = entry;

            if (progress) progress->done.fetch_add(1);
        }

        // Scan for manifests under the library roots for inserting launch details into the owned apps that are installed.
        // A manifest for an app not found in the oaf cache is skipped.
        for (const auto& root : roots)
        {
            std::error_code ec;
            fs::path maniDir = fs::path(root) / L"Manifests";

            if (!fs::is_directory(maniDir, ec)) continue;

            for (fs::directory_iterator it(maniDir, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;

                const fs::path& p = it->path();
                if (p.extension() != L".json") continue; // skips the <name>.json.mini files

                // Skip the _assets.json manifests. The catalog and its art come from the oaf database
                std::string stem = p.filename().string(); // canonical titles should be ASCII
                const std::string tail = "_assets.json";
                if (stem.size() >= tail.size() && stem.compare(stem.size() - tail.size(), tail.size(), tail) == 0)
                    continue;

                std::string text = ReadFileUtf8(p);
                if (text.empty()) continue;

                std::string err;
                json11::Json m = json11::Json::parse(text, err);
                if (!err.empty() || !m.is_object()) continue;

                std::string id = AppIdString(m);
                if (id.empty()) continue; // null appId, a 2D "unknown source" desktop app to ignore

                auto found = byId.find(id);
                if (found == byId.end()) continue; // owned apps only, this manifest is for something not in the oaf cache

                AppEntry& entry = found->second;
                if (!entry.launchFile.empty()) continue; // already inserted from an earlier root

                std::string canon = m["canonicalName"].string_value();
                if (canon.empty()) continue;
                std::string base = BaseCanonical(canon);

                if (entry.canonical.empty()) entry.canonical = base; // fill the canonical title if the oaf cache lacked one

                // Launch info lives in the full <canonical title>.json manifest.
                entry.launchFile = m["launchFile"].is_string() ? m["launchFile"].string_value() : std::string();
                entry.launchParameters = m["launchParameters"].is_string() ? m["launchParameters"].string_value() : std::string();
                if (!entry.launchFile.empty())
                {
                    // the exe folder is <root>\Software\<canonical title>
                    entry.installDir = prefs.Narrow((fs::path(root) / L"Software" / prefs.Widen(canon)).wstring());
                    entry.acquiredTime = FileUnixTime(p); // real acquire time now that a manifest exists
                }
            }
        }

        std::vector<AppEntry> out;
        out.reserve(byId.size());
        for (auto& kv : byId)
        {
            out.push_back(std::move(kv.second));
        }

        std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b)
        {
            return _stricmp(a.title.c_str(), b.title.c_str()) < 0;
        });

        if (imageFailures) *imageFailures = failures;
        return out;
    }

    std::vector<AppEntry> ScanSteam(Progress* progress, int* imageFailures)
    {
        int failures = 0;
        std::vector<AppEntry> out;

        std::string steamRoot = steamLib.SteamRoot();
        if (steamRoot.empty())
        {
            homeLogger.write() << "AppLibraries: Steam install not found, skipping Steam scan." << std::endl;
            if (imageFailures) *imageFailures = 0;
            return out;
        }

        std::vector<SteamLibrary::SteamInstalled> installed = steamLib.EnumerateSteamInstalled(steamLib.SteamLibraryRoots(steamRoot));
        homeLogger.write() << "AppLibraries: Steam reports " << installed.size() << " installed app(s)." << std::endl;

        fs::path imagesDir = fs::path(prefs.AppDir()) / L"store" / L"apps" / L"images";
        std::error_code ecdir;
        fs::create_directories(imagesDir, ecdir);

        std::vector<std::string> cachedImageFiles;
        {
            std::error_code ec;
            for (fs::directory_iterator it(imagesDir, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;
                if (it->is_regular_file(ec)) cachedImageFiles.push_back(it->path().filename().string());
            }
        }

        bool cacheDirty = false;
        for (const auto& s : installed)
        {
            SteamLibrary::SteamDetails d;
            std::string idKey = "steam_" + s.appId;

            d = steamLib.FetchSteamDetails(s.appId);

            // Gentle throttle so a large library does not trip the storefront rate limit.
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
 
            if (d.known)
            {
                SteamLibrary::SteamCovers cov = steamLib.BuildSteamCovers(s.appId, d, imagesDir, cachedImageFiles, failures);

                AppEntry e;
                e.id = s.appId;
                e.canonical = s.name.empty() ? s.appId : s.name; // never empty, it is the /library/launch key and gates PACKAGE_INFO inclusion
                e.title = s.name.empty() ? s.appId : s.name;
                e.acquiredTime = s.acquiredTime;
                e.squareUri = cov.square;
                e.portraitUri = cov.portrait;
                e.landscapeUri = cov.landscape;
                e.iconUri = cov.square;
                e.screenshot0Uri = cov.shot0;
                e.screenshot1Uri = cov.shot1;
                e.launchFile = "steam://rungameid/" + s.appId; // a steam url, launched differently than an oculus exe
                e.installDir = prefs.Narrow(s.installDir.wstring());
                e.source = "steam";

                homeLogger.write() << "AppLibraries: Steam VR app: " << e.title.c_str() << " " << e.id.c_str() << std::endl;
                out.push_back(std::move(e));
            }

            if (progress) progress->done.fetch_add(1);
        }

        std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b)
        {
            return _stricmp(a.title.c_str(), b.title.c_str()) < 0;
        });

        if (imageFailures) *imageFailures = failures;
        return out;
    }

    SteamAchievementsResult ScanSteamAchievements(fetchworlds::Progress* progress)
    {
        SteamAchievementsResult res;

        // The steam achievements scanned for are the ones currently installed
        std::string libText = ReadFileUtf8(fs::path(prefs.AppDir()) / L"store" / L"apps" / L"apps-library.json");
        std::string perr;
        json11::Json lib = json11::Json::parse(libText, perr);
        const json11::Json& steamApps = lib["steamapps"];

        std::string steamRoot = steamLib.SteamRoot();
        fs::path statsDir = fs::path(prefs.Widen(steamRoot)) / L"appcache" / L"stats";

        std::error_code ec;
        fs::path iconDir = fs::path(prefs.AppDir()) / L"store" / L"achievements" / L"icons";
        fs::create_directories(iconDir, ec);

        json11::Json::array index;

        // One achievement schema collected in a parse pass
        struct Gathered
        {
            std::string appId, appTitle, appCanon, appSquareRel;
            std::string bitIndex, title, desc, iconFile;
            long long unlockTime = 0;
        };
        std::vector<Gathered> gathered;

        // Read each app's schema plus user stats once and collect its earned achievements.
        for (const auto& app : steamApps.array_items())
        {
            std::string appId = app["ID"].string_value();
            if (appId.empty()) continue;

            std::string appTitle = app["Title"].string_value();
            std::string appCanon = app["Canonical"].string_value();
            std::string appSquare = app["SquareURI"].string_value(); // stored relative: "apps/images/..."
            std::string appSquareRel = appSquare.empty() ? std::string() : ("store/" + appSquare); // achievements index paths are appdir relative

            std::string schemaBytes = ReadFileUtf8(statsDir / prefs.Widen("UserGameStatsSchema_" + appId + ".bin"));
            if (schemaBytes.empty()) continue; // no schema cached for this app

            SteamLibrary::KvNode schemaRoot;
            if (!steamLib.KvParseBinary(schemaBytes, schemaRoot)) continue;

            const SteamLibrary::KvNode* appNode = schemaRoot.child(appId.c_str());
            if (!appNode && !schemaRoot.children.empty()) appNode = &schemaRoot.children[0];
            if (!appNode) continue;

            const SteamLibrary::KvNode* stats = appNode->child("stats");
            if (!stats) continue;

            // The unlock times are optional. cache[statId].AchievementTimes[bitIndex] = unix seconds.
            SteamLibrary::KvNode userRoot;
            const SteamLibrary::KvNode* cacheNode = nullptr;
            {
                fs::path userPath = steamLib.FindUserStatsFile(statsDir, appId);
                if (!userPath.empty())
                {
                    std::string ub = ReadFileUtf8(userPath);
                    if (!ub.empty() && steamLib.KvParseBinary(ub, userRoot))
                    {
                        cacheNode = userRoot.child("cache");
                        if (!cacheNode)
                        {
                            const SteamLibrary::KvNode* uapp = userRoot.child(appId.c_str()); // some files wrap under the appid
                            if (!uapp && userRoot.children.size() == 1)
                            {
                                uapp = &userRoot.children[0];
                            }
                            if (uapp)
                            {
                                cacheNode = uapp->child("cache");
                            }
                        }
                    }
                }
            }

            for (const auto& statNode : stats->children)
            {
                // Achievements live in the stat that carries a "bits" group (the type 4 bitfield stat). Relying on bits is robust to how the type is stored.
                const SteamLibrary::KvNode* bits = statNode.child("bits");
                if (!bits) continue;

                const SteamLibrary::KvNode* times = nullptr;
                if (cacheNode)
                {
                    const SteamLibrary::KvNode* grp = cacheNode->child(statNode.name.c_str());
                    if (grp) times = grp->child("AchievementTimes");
                }

                for (const auto& bit : bits->children)
                {
                    // Get only the achievements a user actually earned. The unlock time comes from the user stats file, a positive value means unlocked
                    long long unlockTime = 0;
                    if (times)
                    {
                        const SteamLibrary::KvNode* t = times->child(bit.name.c_str());
                        if (t)
                        {
                            try { unlockTime = std::stoll(t->value); }
                            catch (...) { unlockTime = 0; }
                        }
                    }
                    if (unlockTime <= 0) continue;

                    const SteamLibrary::KvNode* nameN = bit.child("name"); // the api name
                    const SteamLibrary::KvNode* disp = bit.child("display");

                    Gathered g;
                    g.appId = appId;
                    g.appTitle = appTitle;
                    g.appCanon = appCanon;
                    g.appSquareRel = appSquareRel;
                    g.bitIndex = bit.name;
                    g.unlockTime = unlockTime;
                    if (disp)
                    {
                        g.title = steamLib.LocalizedValue(disp->child("name"));
                        g.desc = steamLib.LocalizedValue(disp->child("desc"));
                        const SteamLibrary::KvNode* ic = disp->child("icon"); // the unlocked icon
                        if (ic)
                        {
                            g.iconFile = ic->value;
                        }
                    }
                    if (g.title.empty()) g.title = nameN ? nameN->value : std::string();

                    gathered.push_back(std::move(g));
                }
            }
        }

        if (progress)
        {
            progress->total.store((int)gathered.size());
        }

        // Download the icon and build the index entry for each collected achievement.
        for (const auto& g : gathered)
        {
            std::string iconRel;
            if (!g.iconFile.empty())
            {
                std::string url = "https://cdn.cloudflare.steamstatic.com/steamcommunity/public/images/apps/" + g.appId + "/" + g.iconFile;
                std::string name = "steam_ach_" + g.appId + "_" + g.bitIndex + ImageExtFromUrl(g.iconFile);
                if (DownloadImage(url, iconDir / prefs.Widen(name))) iconRel = "store/achievements/icons/" + name;
            }

            index.push_back(json11::Json::object{
                { "id", steamLib.SyntheticAchievementId(g.appId, g.bitIndex) }, // unique numeric id
                { "title", g.title },
                { "description", g.desc },
                { "unlock_time", (double)g.unlockTime },
                { "icon", iconRel },
                { "app_id", g.appId },
                { "app_title", g.appTitle },
                { "app_canonical", g.appCanon },
                { "app_square", g.appSquareRel },
            });

            if (progress)
            {
                progress->done.fetch_add(1);
            }
        }

        // No steam achievements remain as every steam app was removed from the library, so remove the gathered steam achievement icons
        if (index.empty())
        {
            std::error_code e2;
            for (fs::directory_iterator it(iconDir, e2), end; it != end; it.increment(e2))
            {
                if (e2) break;
                if (!it->is_regular_file(e2)) continue;

                if (it->path().filename().string().rfind("steam_ach_", 0) == 0)
                {
                    fs::remove(it->path(), e2);
                }
            }
        }

        fs::path achDir = fs::path(prefs.AppDir()) / L"store" / L"achievements";
        fs::create_directories(achDir, ec);
        fs::path outPath = achDir / L"app-achievements.json";

        // Merge into app-achievements.json under the "steam" key and preserving the "oculus" entries
        json11::Json::object doc;
        doc["steam"] = json11::Json(index);
        {
            std::string existing = ReadFileUtf8(outPath);
            if (!existing.empty())
            {
                std::string eerr;
                json11::Json ej = json11::Json::parse(existing, eerr);
                if (eerr.empty())
                {
                    if (ej["oculus"].is_array())
                    {
                        doc["oculus"] = ej["oculus"];
                    }
                    else if (ej["achievements"].is_array())
                    {
                        doc["oculus"] = ej["achievements"]; // migrate legacy file key
                    }
                }
            }
        }

        fs::path tmp = outPath;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                res.error = "Could not write app-achievements.json.";
                return res;
            }
            f << json11::Json(doc).dump();
        }

        if (!MoveFileExW(tmp.wstring().c_str(), outPath.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            res.error = "Could not write app-achievements.json.";
            return res;
        }

        res.success = true;
        res.saved = (int)index.size();
        homeLogger.write() << "AppLibraries: saved " << res.saved << " Steam achievement(s)." << std::endl;
        return res;
    }

    // Clear Home's own image cache for one file extension
    static int ClearHomeImageCache(const std::wstring& targetExtension)
    {
        wchar_t localAppData[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return 0;

        fs::path dir = fs::path(localAppData) / L"Home2" / L"ImageCache";
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return 0;

        int deleted = 0;
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec))
        {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;

            if (it->path().extension() == targetExtension)
            {
                std::error_code rm;
                if (fs::remove(it->path(), rm))
                {
                    ++deleted;
                }
            }
        }
        return deleted;
    }

    RebuildResult Rebuild(const std::vector<std::string>& userRoots, Progress* progress)
    {
        RebuildResult res;

        int failures = 0;
        std::string oafError;
        if (progress) progress->phase.store(ScanPhaseOculus);
        std::vector<AppEntry> apps = ScanOculus(userRoots, progress, &failures, &oafError);

        int installed = 0;

        json11::Json::array arr;
        arr.reserve(apps.size());
        for (const auto& a : apps)
        {
            if (!a.launchFile.empty()) ++installed;

            arr.push_back(json11::Json::object{
                { "ID", a.id },
                { "Canonical", a.canonical },
                { "Title", a.title },
                { "AcquiredTime", (double)a.acquiredTime }, // serialized as an integer number
                { "SquareURI", a.squareUri },
                { "PortraitURI", a.portraitUri },
                { "LandscapeURI", a.landscapeUri },
                { "IconURI", a.iconUri },
                { "Screenshot0URI", a.screenshot0Uri },
                { "Screenshot1URI", a.screenshot1Uri },
                { "LaunchFile", a.launchFile },
                { "LaunchParameters", a.launchParameters },
                { "InstallDir", a.installDir },
                { "Source", a.source },
            });
        }

        // Optional Steam VR apps branch
        json11::Json::array steamArr;
        int steamCount = 0;
        int steamInstalledCount = 0;
        if (ui.loadSteamVRApps)
        {
            if (progress)
            {
                // The scanning status tracks the installed Steam apps
                std::string sr = steamLib.SteamRoot();
                int steamTotal = sr.empty() ? 0 : (int)steamLib.EnumerateSteamInstalled(steamLib.SteamLibraryRoots(sr)).size();

                progress->done.store(0);
                progress->total.store(steamTotal);
                progress->phase.store(ScanPhaseSteam);
            }

            int steamFailures = 0;
            std::vector<AppEntry> steamApps = ScanSteam(progress, &steamFailures);
            failures += steamFailures;

            std::vector<std::string> vrIds;
            vrIds.reserve(steamApps.size());
            for (const auto& a : steamApps)
            {
                vrIds.push_back(a.id);
            }

            steamArr.reserve(steamApps.size());
            for (const auto& a : steamApps)
            {
                steamArr.push_back(json11::Json::object{
                    { "ID", a.id },
                    { "Canonical", a.canonical },
                    { "Title", a.title },
                    { "AcquiredTime", (double)a.acquiredTime },
                    { "SquareURI", a.squareUri },
                    { "PortraitURI", a.portraitUri },
                    { "LandscapeURI", a.landscapeUri },
                    { "IconURI", a.iconUri },
                    { "Screenshot0URI", a.screenshot0Uri },
                    { "Screenshot1URI", a.screenshot1Uri },
                    { "LaunchFile", a.launchFile },
                    { "LaunchParameters", a.launchParameters },
                    { "InstallDir", a.installDir },
                    { "Source", a.source },
                });
            }
            steamCount = (int)steamApps.size();
            for (const auto& a : steamApps)
            {
                ++steamInstalledCount;
            }
        }

        // Prune any orphaned steam cover images. Includes any store/apps/images/steam_<id>_* whose id is no longer in the steam library
        std::set<std::string> keepSteamIds;
        for (const auto& e : steamArr) keepSteamIds.insert(e["ID"].string_value());

        fs::path imagesDir = fs::path(prefs.AppDir()) / L"store" / L"apps" / L"images";
        std::error_code ec2;
        for (fs::directory_iterator it(imagesDir, ec2), end; it != end; it.increment(ec2))
        {
            if (ec2) break;
            if (!it->is_regular_file(ec2)) continue;

            std::string fn = it->path().filename().string();
            if (fn.rfind("steam_", 0) != 0) continue;

            size_t p = fn.find('_', 6); // the appid sits between "steam_" and the next underscore
            if (p == std::string::npos) continue;

            std::string id = fn.substr(6, p - 6);
            if (!keepSteamIds.count(id)) fs::remove(it->path(), ec2);
        }

        json11::Json doc = json11::Json::object{ { "apps", arr }, { "steamapps", steamArr } };

        res.ocOwned = (int)apps.size();
        res.ocInstalled = installed;
        res.steamInstalled = steamInstalledCount;
        res.imageFailures = failures;

        fs::path out = fs::path(prefs.AppDir()) / L"store" / L"apps" / L"apps-library.json";
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);

        fs::path tmp = out;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                res.error = "Could not write the apps library file.";
                return res;
            }
            f << doc.dump();
        }

        if (!MoveFileExW(tmp.wstring().c_str(), out.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            res.error = "Could not write the apps library file.";
            return res;
        }

        res.success = true;

        // Home caches cover textures itself, so after rebuilding the library clear its image cache for the cover extensions
        int cleared = ClearHomeImageCache(L".jpg") + ClearHomeImageCache(L".png");
        if (cleared > 0)
        {
            homeLogger.write() << "AppLibraries: cleared " << cleared << " Home image cache file(s)." << std::endl;
        }

        // Surface any errors.
        // A missing oaf cache branches since it means no apps at all compared to failed cover downloads.
        if (!oafError.empty())
        {
            res.error = oafError;
        }
        else if (failures > 0)
        {
            res.error = std::to_string(failures) + " app image(s) could not be downloaded.";
        }

        return res;
    }

    LibraryCounts CountApps()
    {
        LibraryCounts counts;

        fs::path p = fs::path(prefs.AppDir()) / L"store" / L"apps" / L"apps-library.json";
        std::string text = ReadFileUtf8(p);
        if (text.empty()) return counts;

        std::string err;
        json11::Json j = json11::Json::parse(text, err);
        if (!err.empty()) return counts;

        const json11::Json& apps = j["apps"];
        if (apps.is_array())
        {
            counts.oculusOwned = (int)apps.array_items().size();
            for (const auto& a : apps.array_items())
            {
                // an installed app is one a manifest gave a launch exe to
                if (!a["LaunchFile"].string_value().empty()) ++counts.oculusInstalled;
            }
        }

        const json11::Json& steam = j["steamapps"];
        if (steam.is_array())
        {
            counts.steamInstalled = (int)steam.array_items().size();
        }

        return counts;
    }

    bool LibraryFileExists()
    {
        fs::path p = fs::path(prefs.AppDir()) / L"store" / L"apps" / L"apps-library.json";
        std::error_code ec;
        return fs::is_regular_file(p, ec);
    }

}
