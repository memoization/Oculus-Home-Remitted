#define WIN32_LEAN_AND_MEAN
#include "AppLibraries.h"
#include "Prefs.h"
#include <windows.h>
#include <filesystem>
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

namespace fs = std::filesystem;

namespace applibraries {

    std::string DefaultRoot()
    {
        return "C:\\Program Files\\Oculus\\Software";
    }

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

    // Offset just past the <u32 nameLen><name> marker for `name`, or npos.
    static size_t BlobFieldNamePos(const unsigned char* blob, size_t len, const char* name)
    {
        size_t nlen = std::strlen(name);
        std::string needle;
        needle.push_back((char)(nlen & 0xFF));
        needle.push_back((char)((nlen >> 8) & 0xFF));
        needle.push_back((char)((nlen >> 16) & 0xFF));
        needle.push_back((char)((nlen >> 24) & 0xFF));
        needle.append(name, nlen);

        for (size_t i = 0; i + needle.size() <= len; ++i)
        {
            if (std::memcmp(blob + i, needle.data(), needle.size()) == 0) return i + needle.size();
        }
        return (size_t)-1;
    }

    // A cover field (cover_square_image / cover_landscape_image) holds a nested Image object whose uri is the oculuscdn url. Return that uri.
    static std::string BlobCoverUri(const unsigned char* blob, size_t len, const char* coverField)
    {
        size_t pos = BlobFieldNamePos(blob, len, coverField);
        if (pos == (size_t)-1) return std::string();

        return BlobStringField(blob, len, "uri", pos);
    }

    // Per-app info pulled from the oaf cache: canonical name, display name and the two oculuscdn cover uris.
    struct OafApp
    {
        std::string canonicalName;
        std::string displayName;
        std::string coverSquareUri;
        std::string coverLandscapeUri;
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
            for (char c : prefs::Narrow(path))
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
                        a.coverSquareUri = BlobCoverUri(blob, (size_t)blen, "cover_square_image");
                        a.coverLandscapeUri = BlobCoverUri(blob, (size_t)blen, "cover_landscape_image");
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

    static std::string ReadFileUtf8(const fs::path& path)
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
    static long long FileUnixTime(const fs::path& p)
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
        std::string p = prefs::Narrow(absPath.wstring());
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
    static std::string ImageExtFromUrl(const std::string& url)
    {
        size_t q = url.find('?');
        std::string path = (q == std::string::npos) ? url : url.substr(0, q);
        size_t dot = path.find_last_of('.');
        size_t slash = path.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash) && path.size() - dot <= 5)
            return path.substr(dot);
        return ".png";
    }

    // Download url to dest. Skips the fetch if the file already exists. Returns true if the file is present afterward.
    static bool DownloadImage(const std::string& url, const fs::path& dest)
    {
        std::error_code ec;
        if (fs::is_regular_file(dest, ec)) return true;
        if (url.empty()) return false;

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
            return false;
        }

        std::ofstream f(dest, std::ios::binary);
        if (!f) return false;
        f.write(r.text.data(), (std::streamsize)r.text.size());
        return f.good();
    }

    // The two graph.oculus.com persisted queries for an app's store art. Both need a valid access_token.
    static const char* kDocAppCover       = "2562966287070020"; // portrait cover, response node.images is a single {uri}
    static const char* kDocAppScreenshots = "2841468849260027"; // screenshots, response node.images is a list of {uri}

    // oculuscdn urls for an app's portrait cover and first two screenshots. Empty fields on any failure should fall back to any existing covers.
    struct AppArt
    {
        std::string portraitUrl;
        std::string shot0Url;
        std::string shot1Url;
    };

    // Relative store path of a downloaded app image named "<id><suffix>.<ext>", or "" when none is cached.
    static std::string FindCachedImage(const std::vector<std::string>& files, const std::string& id, const std::string& suffix)
    {
        std::string prefix = id + suffix + ".";
        for (const auto& name : files)
        {
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
                return "apps/images/" + name;
        }
        return std::string();
    }

    // Query the live backend for an app's portrait cover and screenshots.
    // token is the user's cached access_token, appId is the numeric id.
    // wantCover and wantShots permit each request so cached art is not fetched again. Returns empty urls when the token is absent or a query fails.
    static AppArt FetchAppArt(const std::string& token, const std::string& appId, bool wantCover, bool wantShots)
    {
        AppArt art;
        if (token.empty() || appId.empty()) return art;

        std::string err;

        if (wantCover)
        {
            std::string coverVars = json11::Json(json11::Json::object{ { "size", "252x360" }, { "app_id", appId } }).dump();
            json11::Json cover = fetchworlds::GraphQL(token, kDocAppCover, coverVars, err);
            if (err.empty())
            {
                const json11::Json& uri = cover["data"]["node"]["images"]["uri"];
                if (uri.is_string()) art.portraitUrl = uri.string_value();
            }
        }

        if (wantShots)
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

    std::vector<AppEntry> Scan(const std::vector<std::string>& userRoots, Progress* progress, int* imageFailures, std::string* oafError)
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

        addRoot(prefs::Widen(DefaultRoot()));
        for (const auto& u : userRoots)
        {
            addRoot(prefs::Widen(u));
        }

        // Owned apps come from the Oculus offline cache. Every cached Application becomes an entry installed or not so that any owned app can be a portal destination.
        std::string oafErr;
        std::map<std::string, OafApp> oafApps = LoadOafApps(&oafErr);
        if (oafError) *oafError = oafErr;

        // Read the user's cached access_token once. When present, each app is enriched with real portrait and screenshot art from the live backend, otherwise the local covers are reused as before. Best-effort, a dead backend or no login just keeps the fallback.
        fetchworlds::LocalCreds creds = fetchworlds::LoadLocalCreds();

        // Cover images download into store\apps\images and the stored field is the store-relative path. The backend resolves it to an absolute file:// at serve time so the library remains portable.
        fs::path imagesDir = fs::path(prefs::AppDir()) / L"store" / L"apps" / L"images";
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

            std::string squareUri, landscapeUri;
            if (!oaf.coverSquareUri.empty())
            {
                std::string fn = id + "_square" + ImageExtFromUrl(oaf.coverSquareUri);
                if (DownloadImage(oaf.coverSquareUri, imagesDir / fn))
                {
                    squareUri = "apps/images/" + fn;
                }
                else
                {
                    ++failures; // a uri was present but the fetch failed, likely an expired oculuscdn link?
                } 
            }
            if (!oaf.coverLandscapeUri.empty())
            {
                std::string fn = id + "_landscape" + ImageExtFromUrl(oaf.coverLandscapeUri);
                if (DownloadImage(oaf.coverLandscapeUri, imagesDir / fn))
                {
                    landscapeUri = "apps/images/" + fn;
                }
                else
                {
                    ++failures;
                }
            }

            // Real portrait cover and screenshots when a token is available
            // reuse the square and landscape covers as fallback.
            std::string portraitUri = squareUri;
            std::string shot0Uri = landscapeUri;
            std::string shot1Uri = landscapeUri;

            std::string cachedPortrait = FindCachedImage(cachedImageFiles, id, "_portrait");
            std::string cachedShot0    = FindCachedImage(cachedImageFiles, id, "_shot0");
            std::string cachedShot1    = FindCachedImage(cachedImageFiles, id, "_shot1");
            if (!cachedPortrait.empty()) portraitUri = cachedPortrait;
            if (!cachedShot0.empty())    shot0Uri = cachedShot0;
            if (!cachedShot1.empty())    shot1Uri = cachedShot1;

            bool wantCover = cachedPortrait.empty();
            bool wantShots = cachedShot0.empty() || cachedShot1.empty();
            if (!creds.token.empty() && (wantCover || wantShots))
            {
                AppArt art = FetchAppArt(creds.token, id, wantCover, wantShots);
                if (wantCover && !art.portraitUrl.empty())
                {
                    std::string fn = id + "_portrait" + ImageExtFromUrl(art.portraitUrl);
                    if (DownloadImage(art.portraitUrl, imagesDir / fn)) portraitUri = "apps/images/" + fn;
                    else ++failures;
                }
                if (wantShots && !art.shot0Url.empty())
                {
                    std::string fn = id + "_shot0" + ImageExtFromUrl(art.shot0Url);
                    if (DownloadImage(art.shot0Url, imagesDir / fn)) shot0Uri = "apps/images/" + fn;
                    else ++failures;
                }
                if (wantShots && !art.shot1Url.empty())
                {
                    std::string fn = id + "_shot1" + ImageExtFromUrl(art.shot1Url);
                    if (DownloadImage(art.shot1Url, imagesDir / fn)) shot1Uri = "apps/images/" + fn;
                    else ++failures;
                }
            }

            AppEntry entry;
            entry.id = id;
            entry.canonical = base;
            entry.title = !oaf.displayName.empty() ? oaf.displayName : (!base.empty() ? Prettify(base) : id);
            entry.acquiredTime = kUnknownAcquire; // an installed manifest overwrites this below
            entry.squareUri = squareUri; // cover_square_image
            entry.portraitUri = portraitUri;// portrait cover, square as fallback
            entry.landscapeUri = landscapeUri;// cover_landscape_image
            entry.iconUri = squareUri; // cover_square_image
            entry.screenshot0Uri = shot0Uri; // 1280x720 screenshot, landscape as fallback
            entry.screenshot1Uri = shot1Uri; // 1280x720 screenshot, landscape as fallback
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
                    entry.installDir = prefs::Narrow((fs::path(root) / L"Software" / prefs::Widen(canon)).wstring());
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

    RebuildResult Rebuild(const std::vector<std::string>& userRoots, Progress* progress)
    {
        RebuildResult res;

        int failures = 0;
        std::string oafError;
        std::vector<AppEntry> apps = Scan(userRoots, progress, &failures, &oafError);

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
            });
        }
        json11::Json doc = json11::Json::object{ { "apps", arr } };

        res.owned = (int)apps.size();
        res.installed = installed;
        res.imageFailures = failures;

        fs::path out = fs::path(prefs::AppDir()) / L"store" / L"apps" / L"apps-library.json";
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);

        // Atomic replace so the running backend read won't see a torn file
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

        // Surface any errors.
        // A missing oaf cache branches since it means no apps at all compared to failed cover downloads.
        if (!oafError.empty())
        {
            res.error = oafError;
        }
        else if (failures > 0)
        {
            res.error = std::to_string(failures) + " app image(s) could not be downloaded. Open Meta Link and view your apps library then try \"Refresh Apps\" again.";
        }

        return res;
    }

    LibraryCounts CountApps()
    {
        LibraryCounts counts;

        fs::path p = fs::path(prefs::AppDir()) / L"store" / L"apps" / L"apps-library.json";
        std::string text = ReadFileUtf8(p);
        if (text.empty()) return counts;

        std::string err;
        json11::Json j = json11::Json::parse(text, err);
        if (!err.empty()) return counts;

        const json11::Json& apps = j["apps"];
        if (!apps.is_array()) return counts;

        counts.owned = (int)apps.array_items().size();
        for (const auto& a : apps.array_items())
        {
            // an installed app is one a manifest gave a launch exe to
            if (!a["LaunchFile"].string_value().empty()) ++counts.installed;
        }
        return counts;
    }

    bool LibraryFileExists()
    {
        fs::path p = fs::path(prefs::AppDir()) / L"store" / L"apps" / L"apps-library.json";
        std::error_code ec;
        return fs::is_regular_file(p, ec);
    }

}
