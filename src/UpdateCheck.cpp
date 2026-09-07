#define WIN32_LEAN_AND_MEAN
#include "UpdateCheck.h"
#include "HomeLogger.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <thread>

#include <cpr/cpr.h>
#include "json11.hpp"

#pragma comment(lib, "shell32.lib")

namespace update
{
    // The GitHub repository the release check targets
    static const char* targetRepo = "";

    static std::atomic<bool> available{ false };
    static std::atomic<bool> started{ false };
    static std::mutex tagMutex;
    static std::string latestTag;

    // Normalize so that "v1.10" and "1.10" compare equally.
    static std::string Normalize(std::string s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();

        s = s.substr(a, b - a + 1);
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
        return s;
    }

    void StartCheck(const std::string& currentVersion)
    {
        bool expected = false;
        if (!started.compare_exchange_strong(expected, true)) return;

        std::thread([currentVersion]()
        {
            std::string url = std::string("https://api.github.com/repos/") + targetRepo + "/releases/latest";
            cpr::Response r = cpr::Get(
                cpr::Url{ url },
                cpr::Header{ { "User-Agent", "Home2Client" }, { "Accept", "application/vnd.github+json" } },
                cpr::Timeout{ 8000 });

            if (r.error || r.status_code != 200)
            {
                homeLogger.write() << "UpdateCheck: request failed (HTTP " << r.status_code << ")." << std::endl;
                return;
            }

            std::string perr;
            json11::Json j = json11::Json::parse(r.text, perr);
            if (!perr.empty())
            {
                homeLogger.write() << "UpdateCheck: could not parse the response." << std::endl;
                return;
            }

            std::string tag = j["tag_name"].string_value();
            if (tag.empty())
            {
                homeLogger.write() << "UpdateCheck: no tag_name in the latest release." << std::endl;
                return;
            }

            {
                std::lock_guard<std::mutex> lk(tagMutex);
                latestTag = tag;
            }

            bool differs = Normalize(tag) != Normalize(currentVersion);
            available.store(differs);
        }).detach();
    }

    bool IsUpdateAvailable()
    {
        return available.load();
    }

    std::string LatestTag()
    {
        std::lock_guard<std::mutex> lk(tagMutex);
        return latestTag;
    }

    void OpenReleasesPage()
    {
        std::string url = std::string("https://github.com/") + targetRepo + "/releases";
        std::wstring wurl(url.begin(), url.end());
        ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
