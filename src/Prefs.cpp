#define WIN32_LEAN_AND_MEAN
#include "Prefs.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include "json11.hpp"

namespace prefs
{

    std::wstring Widen(const std::string& utf8)
    {
        if (utf8.empty()) return std::wstring();

        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], n);
        return w;
    }

    std::string Narrow(const std::wstring& wide)
    {
        if (wide.empty()) return std::string();

        int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring AppDir()
    {
        wchar_t path[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring w(path);
        size_t slash = w.find_last_of(L"\\/");

        return (slash == std::wstring::npos) ? std::wstring() : w.substr(0, slash + 1);
    }

    static std::wstring PrefsPath()
    {
        return AppDir() + L"preferences.json";
    }

    static std::string ReadFileUtf8(const std::wstring& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::string();

        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string GetHome2ExePath()
    {
        std::string text = ReadFileUtf8(PrefsPath());
        if (text.empty()) return std::string();

        std::string err;
        json11::Json j = json11::Json::parse(text, err);
        if (!err.empty()) return std::string();

        return j["wrapper"]["home2ExePath"].string_value();
    }

    void SetHome2ExePath(const std::string& path)
    {
        // Load-modify-write so any other fields already in preferences.json survive.
        std::string text = ReadFileUtf8(PrefsPath());
        std::string err;
        json11::Json existing = text.empty() ? json11::Json() : json11::Json::parse(text, err);

        json11::Json::object root = existing.is_object() ? existing.object_items() : json11::Json::object();
        json11::Json::object wrapper = root["wrapper"].is_object() ? root["wrapper"].object_items() : json11::Json::object();
        wrapper["home2ExePath"] = path;
        root["wrapper"] = wrapper;

        std::ofstream f(PrefsPath(), std::ios::binary | std::ios::trunc);
        if (f) f << json11::Json(root).dump();
    }

    std::string GetDisplayName()
    {
        std::string text = ReadFileUtf8(PrefsPath());
        if (text.empty()) return "Player";

        std::string err;
        json11::Json j = json11::Json::parse(text, err);
        if (!err.empty()) return "Player";

        std::string name = j["identity"]["displayName"].string_value();
        return name.empty() ? "Player" : name;
    }

    void SetDisplayName(const std::string& name)
    {
        std::string text = ReadFileUtf8(PrefsPath());
        std::string err;
        json11::Json existing = text.empty() ? json11::Json() : json11::Json::parse(text, err);

        json11::Json::object root = existing.is_object() ? existing.object_items() : json11::Json::object();
        json11::Json::object identity = root["identity"].is_object() ? root["identity"].object_items() : json11::Json::object();
        identity["displayName"] = name;
        root["identity"] = identity;

        std::ofstream f(PrefsPath(), std::ios::binary | std::ios::trunc);
        if (f) f << json11::Json(root).dump();
    }

    std::string GetProfileImagePath()
    {
        std::string text = ReadFileUtf8(PrefsPath());
        if (text.empty()) return std::string();
        std::string err;
        json11::Json j = json11::Json::parse(text, err);
        if (!err.empty()) return std::string();

        return j["wrapper"]["profileImagePath"].string_value();
    }

    void SetProfileImagePath(const std::string& path)
    {
        std::string text = ReadFileUtf8(PrefsPath());
        std::string err;
        json11::Json existing = text.empty() ? json11::Json() : json11::Json::parse(text, err);

        json11::Json::object root = existing.is_object() ? existing.object_items() : json11::Json::object();
        json11::Json::object wrapper = root["wrapper"].is_object() ? root["wrapper"].object_items() : json11::Json::object();
        wrapper["profileImagePath"] = path;
        root["wrapper"] = wrapper;

        std::ofstream f(PrefsPath(), std::ios::binary | std::ios::trunc);
        if (f) f << json11::Json(root).dump();
    }

    std::string GetDefaultWorldId()
    {
        std::string text = ReadFileUtf8(PrefsPath());
        if (text.empty()) return std::string();
        std::string err;
        json11::Json j = json11::Json::parse(text, err);
        if (!err.empty()) return std::string();

        return j["defaultWorldId"].string_value();
    }

    void SetDefaultWorldId(const std::string& id)
    {
        // Load-modify-write preserving every other field (the backend may concurrently write userOptions), then replace so the file never tears
        std::string text = ReadFileUtf8(PrefsPath());
        std::string err;
        json11::Json existing = text.empty() ? json11::Json() : json11::Json::parse(text, err);

        json11::Json::object root = existing.is_object() ? existing.object_items() : json11::Json::object();
        root["defaultWorldId"] = id;

        std::wstring path = PrefsPath();
        std::wstring tmp = path + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << json11::Json(root).dump();
        }

        MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    std::vector<std::string> GetOculusLibraryPaths()
    {
        std::vector<std::string> out;
        std::string text = ReadFileUtf8(PrefsPath());
        if (text.empty()) return out;

        std::string err;
        json11::Json j = json11::Json::parse(text, err);
        if (!err.empty()) return out;

        for (const auto& item : j["wrapper"]["oculusLibraryPaths"].array_items())
        {
            if (item.is_string() && !item.string_value().empty())
            {
                out.push_back(item.string_value());
            }
        }

        return out;
    }

    void SetOculusLibraryPaths(const std::vector<std::string>& paths)
    {
        // Load-modify-write so every other field (identity, userOptions, ...) survives.
        std::string text = ReadFileUtf8(PrefsPath());
        std::string err;
        json11::Json existing = text.empty() ? json11::Json() : json11::Json::parse(text, err);

        json11::Json::object root = existing.is_object() ? existing.object_items() : json11::Json::object();
        json11::Json::object wrapper = root["wrapper"].is_object() ? root["wrapper"].object_items() : json11::Json::object();

        json11::Json::array arr;
        for (const auto& p : paths)
        {
            if (!p.empty())
            {
                arr.push_back(p);
            }
        }
            
        wrapper["oculusLibraryPaths"] = arr;
        root["wrapper"] = wrapper;

        std::ofstream f(PrefsPath(), std::ios::binary | std::ios::trunc);
        if (f)
        {
            f << json11::Json(root).dump();
        }
    }

    void SeedDefaultsIfMissing()
    {
        // Do not clobber an existing file (preserves user edits and the backend's userOptions writes)
        std::ifstream existing(PrefsPath(), std::ios::binary);
        if (existing.good()) return;

        existing.close();

        json11::Json::object wrapper;
        wrapper["home2ExePath"] = std::string();
        wrapper["profileImagePath"] = std::string();

        json11::Json::object identity;
        identity["userId"] = std::string("111111111111111");
        identity["oculusId"] = std::string("Player");
        identity["displayName"] = std::string("Player");

        // Scrubbed default user_options. All values are JSON strings. The backend round-trips this block via world_login and set_user_options.
        json11::Json::object userOptions{
            {"TeleportDirection", std::string("0")},
            {"ShowPlayArea", std::string("true")},
            {"AutoGrouping", std::string("false")},
            {"SnappingOn", std::string("true")},
            {"volume_music", std::string("1.000000")},
            {"ShowOverlay", std::string("false")},
            {"HandsSharePrefs", std::string("true")},
            {"TranslationSpeed", std::string("1.000000")},
            {"SnapTurnAngle", std::string("45.000000")},
            {"SmoothTurnSpeed", std::string("1.000000")},
            {"LeftMovementMode", std::string("Teleport")},
            {"LeftTurnAroundEnabled", std::string("false")},
            {"LeftTurningMode", std::string("SnapTurning")},
            {"LeftForwardMode", std::string("HeadForward")},
            {"RightMovementMode", std::string("Teleport")},
            {"RightTurnAroundEnabled", std::string("false")},
            {"RightTurningMode", std::string("SnapTurning")},
            {"RightForwardMode", std::string("HeadForward")},
            {"SafetyBubbleEnabled", std::string("false")},
            {"ShowExampleHomes", std::string("false")},
        };

        json11::Json::object root;
        root["wrapper"] = wrapper;
        root["identity"] = identity;
        root["userOptions"] = userOptions;
        // The default-world pointer (worlds::SeedDefaultIfEmpty fills it right after this)
        root["defaultWorldId"] = std::string();

        std::ofstream f(PrefsPath(), std::ios::binary | std::ios::trunc);
        if (f) f << json11::Json(root).dump();
    }

}
