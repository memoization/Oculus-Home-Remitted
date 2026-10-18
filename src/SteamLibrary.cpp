#include "SteamLibrary.h"
#include "AppLibraries.h"
#include "ImageConv.h"
#include "Prefs.h"
#include <cpr/cpr.h>
#include "json11.hpp"

SteamLibrary steamLib;

std::string SteamLibrary::SteamRoot()
{
    wchar_t buf[MAX_PATH];
    DWORD cb = sizeof(buf);

    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS)
    {
        return prefs.Narrow(buf);
    }

    cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS)
    {
        return prefs.Narrow(buf);
    }

    return std::string();
}

std::vector<std::string> SteamLibrary::SteamLibraryRoots(const std::string& steamRoot)
{
    std::vector<std::string> roots;
    if (steamRoot.empty()) return roots;

    auto addRoot = [&](const std::string& r)
        {
            if (r.empty()) return;

            std::string norm = r;
            for (char& c : norm)
            {
                if (c == '/') c = '\\';
                c = (char)std::tolower((unsigned char)c);
            }
            for (const auto& e : roots)
            {
                std::string en = e;
                for (char& c : en)
                {
                    if (c == '/') c = '\\';
                    c = (char)std::tolower((unsigned char)c);
                }
                if (en == norm) return;
            }
            roots.push_back(r);
        };

    addRoot(steamRoot);

    fs::path vdf = fs::path(prefs.Widen(steamRoot)) / L"steamapps" / L"libraryfolders.vdf";
    std::string text = applibraries::ReadFileUtf8(vdf);
    if (!text.empty())
    {
        for (const auto& p : VdfValues(VdfQuotedTokens(text), "path"))
        {
            addRoot(p);
        }
    }

    return roots;
}


std::vector<SteamLibrary::SteamInstalled> SteamLibrary::EnumerateSteamInstalled(const std::vector<std::string>& roots)
{
    std::vector<SteamInstalled> out;

    for (const auto& r : roots)
    {
        std::error_code ec;
        fs::path steamApps = fs::path(prefs.Widen(r)) / L"steamapps";
        if (!fs::is_directory(steamApps, ec)) continue;

        for (fs::directory_iterator it(steamApps, ec), end; it != end; it.increment(ec))
        {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;

            std::string fn = it->path().filename().string();
            if (fn.rfind("appmanifest_", 0) != 0) continue;
            if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".acf") != 0) continue;

            std::string text = applibraries::ReadFileUtf8(it->path());
            if (text.empty()) continue;

            std::vector<std::string> toks = VdfQuotedTokens(text);
            std::string id = VdfValue(toks, "appid");
            std::string installdir = VdfValue(toks, "installdir");
            std::string stateFlags = VdfValue(toks, "StateFlags");
            if (id.empty()) continue;
            if (IsExcludedSteamApp(id)) continue;

            // Require the fully installed flag (4). Skip apps still downloading or staged.
            long long flags = 0;
            try { flags = std::stoll(stateFlags); }
            catch (...) { flags = 0; }
            if ((flags & 4) == 0) continue;

            SteamInstalled s;
            s.appId = id;
            s.name = VdfValue(toks, "name");
            s.installDir = steamApps / L"common" / prefs.Widen(installdir);
            s.acquiredTime = applibraries::FileUnixTime(it->path());
            out.push_back(std::move(s));
        }
    }

    return out;
}

SteamLibrary::SteamDetails SteamLibrary::FetchSteamDetails(const std::string& appId)
{
    SteamDetails d;

    std::string url = "https://store.steampowered.com/api/appdetails?appids=" + appId;
    cpr::Response r = cpr::Get(cpr::Url{ url }, cpr::Timeout{ 15000 });
    if (r.status_code != 200 || r.text.empty()) return d;

    std::string err;
    json11::Json j = json11::Json::parse(r.text, err);
    if (!err.empty()) return d;

    const json11::Json& entry = j[appId];
    if (!entry["success"].bool_value())
    {
        d.known = false; // no details for the app, so mark as a skip
        return d;
    }

    const json11::Json& data = entry["data"];

    d.known = true;
    d.name = data["name"].string_value();

    // Art is captured for every app. The portrait is not in appdetails, but is resolved separately via FetchSteamPortraitUrl.
    // Landscape from the small capsule, the header image as the fallback.
    d.headerUrl = data["header_image"].string_value();
    d.landscapeUrl = data["capsule_image"].string_value();
    if (d.landscapeUrl.empty()) d.landscapeUrl = d.headerUrl;

    const json11::Json& shots = data["screenshots"];
    if (shots.is_array())
    {
        if (shots.array_items().size() >= 1) d.shot0Url = shots[0]["path_full"].string_value();
        if (shots.array_items().size() >= 2) d.shot1Url = shots[1]["path_full"].string_value();
    }

    return d;
}

std::string SteamLibrary::FetchSteamPortraitUrl(const std::string& appId)
{
    std::string input = "{\"ids\":[{\"appid\":" + appId + "}],\"context\":{\"language\":\"english\",\"country_code\":\"US\",\"steam_realm\":1},\"data_request\":{\"include_assets\":true}}";

    cpr::Response r = cpr::Get(
        cpr::Url{ "https://api.steampowered.com/IStoreBrowseService/GetItems/v1/" },
        cpr::Parameters{ { "input_json", input } },
        cpr::Timeout{ 15000 });
    if (r.status_code != 200 || r.text.empty()) return std::string();

    std::string err;
    json11::Json j = json11::Json::parse(r.text, err);
    if (!err.empty()) return std::string();

    const json11::Json& items = j["response"]["store_items"];
    if (!items.is_array() || items.array_items().empty()) return std::string();

    const json11::Json& assets = items[0]["assets"];
    std::string fmt = assets["asset_url_format"].string_value(); // "steam/apps/<id>/${FILENAME}?t=..."
    std::string cap = assets["library_capsule_2x"].string_value(); // 600x900
    if (cap.empty()) cap = assets["library_capsule"].string_value(); // 300x450 as fallback
    if (fmt.empty() || cap.empty()) return std::string();

    const std::string token = "${FILENAME}";
    size_t pos = fmt.find(token);
    if (pos == std::string::npos) return std::string();
    fmt.replace(pos, token.size(), cap);

    return "https://shared.akamai.steamstatic.com/store_item_assets/" + fmt;
}

SteamLibrary::SteamCovers SteamLibrary::BuildSteamCovers(const std::string& appId, const SteamDetails& d, const fs::path& imagesDir, std::vector<std::string>& cachedImageFiles, int& failures)
{
    std::string idKey = "steam_" + appId;
    std::string landscapeUri = applibraries::FindCachedImage(cachedImageFiles, idKey, "_landscape");
    std::string portraitUri = applibraries::FindCachedImage(cachedImageFiles, idKey, "_portrait");
    std::string shot0Uri = applibraries::FindCachedImage(cachedImageFiles, idKey, "_shot0");
    std::string shot1Uri = applibraries::FindCachedImage(cachedImageFiles, idKey, "_shot1");

    // Each cover refetches when missing or if its file is older than a day
    if (!applibraries::CachedImageFresh(imagesDir, landscapeUri) && !d.landscapeUrl.empty())
    {
        std::string fn = idKey + "_landscape" + applibraries::ImageExtFromUrl(d.landscapeUrl);
        if (applibraries::DownloadImage(d.landscapeUrl, imagesDir / prefs.Widen(fn), applibraries::ImageMaxAgeSec)) landscapeUri = "apps/images/" + fn;
        else if (landscapeUri.empty()) ++failures;
    }

    // The header backs both the square cover and the portrait fallback so fetch it first
    fs::path headerPath;
    if (!d.headerUrl.empty())
    {
        fs::path hp = imagesDir / prefs.Widen(idKey + "_header" + applibraries::ImageExtFromUrl(d.headerUrl));
        if (applibraries::DownloadImage(d.headerUrl, hp, applibraries::ImageMaxAgeSec)) headerPath = hp;
    }

    // Prefer the real portrait (library capsule). Its url is not in appdetails, so resolve it from GetItems
    if (!applibraries::CachedImageFresh(imagesDir, portraitUri))
    {
        std::string portraitUrl = FetchSteamPortraitUrl(appId);
        if (!portraitUrl.empty())
        {
            std::string fn = idKey + "_portrait" + applibraries::ImageExtFromUrl(portraitUrl);
            if (applibraries::DownloadImage(portraitUrl, imagesDir / prefs.Widen(fn), applibraries::ImageMaxAgeSec)) portraitUri = "apps/images/" + fn;
        }
    }
    // No portrait capsule available at all. Fallback to rotating the header 90 degrees as alternative
    if (portraitUri.empty() && !headerPath.empty())
    {
        std::string rotFn = idKey + "_portraitrot.png";
        fs::path rotPath = imagesDir / prefs.Widen(rotFn);
        if (!applibraries::CachedImageFresh(imagesDir, "apps/images/" + rotFn))
        {
            imageconv::Rotate90PngFile(headerPath.wstring(), rotPath.wstring()); // atomic & keeps the old file on failure
        }
        std::error_code rec;
        if (fs::is_regular_file(rotPath, rec)) portraitUri = "apps/images/" + rotFn;
    }

    if (!applibraries::CachedImageFresh(imagesDir, shot0Uri) && !d.shot0Url.empty())
    {
        std::string fn = idKey + "_shot0" + applibraries::ImageExtFromUrl(d.shot0Url);
        if (applibraries::DownloadImage(d.shot0Url, imagesDir / prefs.Widen(fn), applibraries::ImageMaxAgeSec)) shot0Uri = "apps/images/" + fn;
    }
    if (!applibraries::CachedImageFresh(imagesDir, shot1Uri) && !d.shot1Url.empty())
    {
        std::string fn = idKey + "_shot1" + applibraries::ImageExtFromUrl(d.shot1Url);
        if (applibraries::DownloadImage(d.shot1Url, imagesDir / prefs.Widen(fn), applibraries::ImageMaxAgeSec)) shot1Uri = "apps/images/" + fn;
    }

    // Screenshots fall back to the landscape so some are not missing
    if (portraitUri.empty()) portraitUri = landscapeUri;
    if (shot0Uri.empty()) shot0Uri = landscapeUri;
    if (shot1Uri.empty()) shot1Uri = landscapeUri;

    // Steam apps have no square covers, so build one from the header centered on a blurred fill of itself
    std::string squareFn = idKey + "_square.png";
    fs::path squarePath = imagesDir / prefs.Widen(squareFn);
    if (!headerPath.empty() && !applibraries::CachedImageFresh(imagesDir, "apps/images/" + squareFn))
    {
        imageconv::SquareWithBlurFillPngFile(headerPath.wstring(), squarePath.wstring());
    }
    std::error_code sqec;
    std::string squareUri = fs::is_regular_file(squarePath, sqec) ? ("apps/images/" + squareFn) : landscapeUri; // welp, no header or build failed. now fall back to the landscape

    SteamCovers c;
    c.square = squareUri;
    c.portrait = portraitUri;
    c.landscape = landscapeUri;
    c.shot0 = shot0Uri;
    c.shot1 = shot1Uri;
    return c;
}

std::vector<std::string> SteamLibrary::VdfQuotedTokens(const std::string& text)
{
    std::vector<std::string> tokens;
    bool inStr = false;
    std::string cur;

    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];

        if (!inStr)
        {
            if (c == '"')
            {
                inStr = true;
                cur.clear();
            }
            continue;
        }

        if (c == '\\' && i + 1 < text.size())
        {
            char n = text[i + 1];
            if (n == '\\') { cur += '\\'; ++i; continue; }
            if (n == '"') { cur += '"'; ++i; continue; }
            cur += c;
            continue;
        }

        if (c == '"')
        {
            tokens.push_back(cur);
            inStr = false;
            continue;
        }

        cur += c;
    }

    return tokens;
}

std::string SteamLibrary::VdfValue(const std::vector<std::string>& toks, const char* key)
{
    for (size_t i = 0; i + 1 < toks.size(); ++i)
    {
        if (toks[i] == key) return toks[i + 1];
    }
    return std::string();
}

std::vector<std::string> SteamLibrary::VdfValues(const std::vector<std::string>& toks, const char* key)
{
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < toks.size(); ++i)
    {
        if (toks[i] == key) out.push_back(toks[i + 1]);
    }
    return out;
}

bool SteamLibrary::KvReadCStr(const unsigned char* buf, size_t len, size_t& pos, std::string& out)
{
    size_t start = pos;
    while (pos < len && buf[pos] != 0) ++pos;
    if (pos >= len) return false;

    out.assign((const char*)buf + start, pos - start);
    ++pos; // skip the terminator
    return true;
}

bool SteamLibrary::KvParseObject(const unsigned char* buf, size_t len, size_t& pos, std::vector<KvNode>& children, int depth)
{
    if (depth > 64) return false; // guard malformed while deep nesting

    while (pos < len)
    {
        unsigned char type = buf[pos++];
        if (type == 0x08 || type == 0x0B) return true; // end of this object

        KvNode node;
        if (!KvReadCStr(buf, len, pos, node.name)) return false;

        switch (type)
        {
        case 0x00:
            if (!KvParseObject(buf, len, pos, node.children, depth + 1)) return false;
            break;
        case 0x01:
            if (!KvReadCStr(buf, len, pos, node.value)) return false;
            break;
        case 0x02:
        case 0x04:
        case 0x06:
        {
            if (pos + 4 > len) return false;
            int32_t v;
            std::memcpy(&v, buf + pos, 4);
            pos += 4;
            node.value = std::to_string(v);
            break;
        }
        case 0x03:
        {
            if (pos + 4 > len) return false;
            float v;
            std::memcpy(&v, buf + pos, 4);
            pos += 4;
            node.value = std::to_string(v);
            break;
        }
        case 0x07:
        {
            if (pos + 8 > len) return false;
            uint64_t v;
            std::memcpy(&v, buf + pos, 8);
            pos += 8;
            node.value = std::to_string(v);
            break;
        }
        case 0x0A:
        {
            if (pos + 8 > len) return false;
            int64_t v;
            std::memcpy(&v, buf + pos, 8);
            pos += 8;
            node.value = std::to_string(v);
            break;
        }
        case 0x05:
        {
            // widestring UTF-16LE double-null terminated
            size_t start = pos;
            while (pos + 1 < len && !(buf[pos] == 0 && buf[pos + 1] == 0)) pos += 2;
            if (pos + 1 >= len) return false;

            std::wstring w((size_t)((pos - start) / 2), L'\0');
            for (size_t i = 0; i < w.size(); ++i)
            {
                w[i] = (wchar_t)(buf[start + i * 2] | (buf[start + i * 2 + 1] << 8));
            }
            node.value = prefs.Narrow(w);
            pos += 2; // skip the double null
            break;
        }
        default:
            return false; // unknown type, bail
        }

        children.push_back(std::move(node));
    }

    return true;
}

bool SteamLibrary::KvParseBinary(const std::string& bytes, KvNode& root)
{
    root = KvNode();
    size_t pos = 0;
    return KvParseObject((const unsigned char*)bytes.data(), bytes.size(), pos, root.children, 0);
}

std::string SteamLibrary::LocalizedValue(const KvNode* n)
{
    if (!n) return std::string();
    if (!n->children.empty())
    {
        const KvNode* en = n->child("english");
        if (en) return en->value;
        return n->children[0].value;
    }
    return n->value;
}

fs::path SteamLibrary::FindUserStatsFile(const fs::path& statsDir, const std::string& appId)
{
    std::string prefix = "UserGameStats_";
    std::string suffix = "_" + appId + ".bin";

    std::error_code ec;
    for (fs::directory_iterator it(statsDir, ec), end; it != end; it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;

        std::string fn = it->path().filename().string();
        if (fn.size() > prefix.size() + suffix.size() &&
            fn.compare(0, prefix.size(), prefix) == 0 &&
            fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            return it->path();
        }
    }
    return fs::path();
}

std::string SteamLibrary::SyntheticAchievementId(const std::string& appId, const std::string& bitIndex)
{
    uint64_t h = 1469598103934665603ULL; // FNV-1a 64 offset basis
    auto mix = [&](const std::string& s)
        {
            for (unsigned char c : s)
            {
                h ^= c;
                h *= 1099511628211ULL;
            }
        };
    mix(appId);
    mix("/");
    mix(bitIndex);

    h &= 0x7FFFFFFFFFFFFFFFULL; // keep positive if the game parses it as a signed int64
    if (h == 0) h = 1;
    return std::to_string(h);
}
