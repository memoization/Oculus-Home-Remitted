#pragma once
#include <atomic>
#include <string>
#include <vector>

// "Fetch My Homes": pull the user's own worlds from the live graph.oculus.com backend (while it still answers lmao) and write them into store\worlds\world_<id>\ in the same format the backend serves, so they show in the app and load in-VR offline
//The UI polls Progress for the bar and the returned Result (via std::future) for success/failure
namespace fetchworlds
{
    struct Progress
    {
        std::atomic<int> total{ 0 }; // worlds discovered (0 until the list request returns)
        std::atomic<int> done{ 0 };// worlds written so far 
    };

    struct Result
    {
        bool ok = false;
        int worldsSaved = 0;
        int achievementsSaved = 0;
        std::string error; // human-readable err, shown in the modal on failure
    };

    // token is the FRL access token, userId is the user's numeric Oculus id
    Result FetchMyWorlds(std::string token, std::string userId, Progress* progress);

    // One achievement entry in store\achievements\app-achievements.json. The plaque shows title, the app square thumbnail, and the achievement icon.
    struct AchievementInfo
    {
        std::string id;
        std::string title;
        std::string description;
        long long unlockTime = 0;
        std::string iconPath;
        std::string appId;
        std::string appTitle;
        std::string appCanonical;
        std::string appSquarePath;
    };

    // Fetch achievements for every app in store\apps-library.json via worlds_apps_and_achievements then download the icons and app thumbnails into store\achievements\icons, and write store\achievements\app-achievements.json.
    Result FetchMyAchievements(std::string token, Progress* progress);

    // Load the saved achievement index for the Achievements page. Returns an empty list when the file is missing.
    std::vector<AchievementInfo> LoadAchievements();

    // Count the app total count in store\apps-library.json
    int CountAppsInLibrary();
}
