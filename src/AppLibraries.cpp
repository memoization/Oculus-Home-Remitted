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

namespace fs = std::filesystem;

namespace applibraries {

    std::string DefaultRoot()
    {
        return "C:\\Program Files\\Oculus\\CoreData";
    }

    // ---- Extract library display names from the Oculus cache ----
    // %APPDATA%\Oculus\sessions\_oaf\data.sqlite (Objects table) caches the full app catalog
    // A string-scalar field in the blob is <u32 nameLen><name>\x01\x01<u64 valLen><value>. Return the value for `name`, or "" if not present as a string scalar.
    static std::string BlobStringField(const unsigned char* blob, size_t len, const char* name)
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

        for (size_t i = 0; i + needle.size() + 8 <= len; ++i)
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

    static std::map<std::string, std::string> LoadOafDisplayNames()
    {
        std::map<std::string, std::string> out;

        wchar_t appdata[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return out;

        std::wstring path = std::wstring(appdata) + L"\\Oculus\\sessions\\_oaf\\data.sqlite";
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return out;

        HMODULE h = LoadLibraryW(L"winsqlite3.dll");
        if (!h) return out;

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
                        std::string title = BlobStringField(blob, (size_t)blen, "display_name");
                        
                        if (!title.empty())
                        {
                            out[std::string((const char*)hk)] = title;
                        }
                    }
                    finalize(st);
                }
                close_db(db);
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

    std::vector<AppEntry> Scan(const std::vector<std::string>& userRoots)
    {
        // Default CoreData first, then each user root. Dedup roots case-insensitively.
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

        // Real display names from the Oculus offline cache (appId to title). Prettified canonical is the fallback.
        std::map<std::string, std::string> displayNames = LoadOafDisplayNames();

        std::map<std::string, AppEntry> byId; // appId to entry (dedup across roots)
        for (const auto& root : roots)
        {
            std::error_code ec;
            fs::path maniDir = fs::path(root) / L"Manifests";

            if (!fs::is_directory(maniDir, ec)) continue;

            fs::path assetsDir = fs::path(root) / L"Software" / L"StoreAssets";

            for (fs::directory_iterator it(maniDir, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;

                const fs::path& p = it->path();
                if (p.extension() != L".json") continue; // skips <name>.json.mini files

                bool isAssets = false;
                {
                    std::string stem = p.filename().string(); // canonicals should be ASCII
                    const std::string tail = "_assets.json";
                    isAssets = stem.size() >= tail.size() && stem.compare(stem.size() - tail.size(), tail.size(), tail) == 0;
                }

                std::string text = ReadFileUtf8(p);
                if (text.empty()) continue;

                std::string err;
                json11::Json m = json11::Json::parse(text, err);
                if (!err.empty() || !m.is_object()) continue;

                std::string id = AppIdString(m);
                if (id.empty()) continue; // null appId, a 2D "unknown source" desktop app to ignore

                std::string canon = m["canonicalName"].string_value();
                if (canon.empty()) continue;

                std::string base = BaseCanonical(canon);
                fs::path folder = assetsDir / (base + "_assets");
                auto uri = [&](const char* name) -> std::string
                {
                    std::error_code e2;
                    fs::path fp = folder / name;
                    if (!fs::is_regular_file(fp, e2)) return std::string();
                    return ToFileUri(fp);
                };

                std::string square = uri("cover_square_image.jpg");
                std::string landscape = uri("small_landscape_image.jpg");
                std::string screenshot = uri("cover_landscape_image.jpg");

                auto dn = displayNames.find(id);

                AppEntry entry;
                entry.id = id;
                entry.canonical = base;
                entry.title = (dn != displayNames.end() && !dn->second.empty()) ? dn->second : Prettify(base);
                entry.acquiredTime = FileUnixTime(p);
                entry.squareUri = square;
                entry.portraitUri = square;
                entry.landscapeUri = landscape;
                entry.iconUri = uri("icon_image.jpg");
                entry.screenshot0Uri = screenshot;// no local screenshots so reuse the landscape cover
                entry.screenshot1Uri = screenshot;

                // A full <canonical>.json wins over a bare _assets entry, otherwise fill in a missing thumbnail from a later root that has the store art.
                auto existing = byId.find(id);
                if (existing == byId.end() || !isAssets || (existing->second.squareUri.empty() && !entry.squareUri.empty()))
                {
                    byId[id] = entry;
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
        return out;
    }

    int Rebuild(const std::vector<std::string>& userRoots)
    {
        std::vector<AppEntry> apps = Scan(userRoots);

        json11::Json::array arr;
        arr.reserve(apps.size());
        for (const auto& a : apps)
        {
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
            });
        }
        json11::Json doc = json11::Json::object{ { "apps", arr } };

        fs::path out = fs::path(prefs::AppDir()) / L"store" / L"apps-library.json";
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);

        // Atomic replace so the running backend read won't see a torn file
        fs::path tmp = out;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return -1;
            f << doc.dump();
        }

        if (!MoveFileExW(tmp.wstring().c_str(), out.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return -1;
        }

        return (int)apps.size();
    }

}
