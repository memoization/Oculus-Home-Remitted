#include "ProbePrefs.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "json11.hpp"

namespace home2backend {

    static bool ReadFileText(const std::wstring& path, std::string& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;

        std::ostringstream buffer;
        buffer << file.rdbuf();
        out = buffer.str();
        return true;
    }

    bool LoadIdentity(const std::wstring& prefsPath, Identity& out)
    {
        std::string text;
        if (!ReadFileText(prefsPath, text) || text.empty()) return false;

        std::string err;
        json11::Json j = json11::Json::parse(text, err);
        if (!err.empty() || !j.is_object()) return false;

        const json11::Json& identity = j["identity"];
        if (!identity.is_object()) return false;

        Identity parsed;
        parsed.userId = identity["userId"].string_value();
        parsed.oculusId = identity["oculusId"].string_value();
        parsed.displayName = identity["displayName"].string_value();
        if (parsed.userId.empty()) return false;

        // owner_id must equal the decimal string of the uint64 login id. A leading-zero or non-numeric userId that can't get through strtoull is treated as unusable.
        parsed.userId64 = std::strtoull(parsed.userId.c_str(), nullptr, 10);
        if (parsed.userId64 == 0) return false;

        out = parsed;
        return true;
    }

}
