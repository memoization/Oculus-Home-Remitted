#pragma once
#include <atomic>
#include <string>

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
        std::string error; // human-readable err, shown in the modal on failure
    };

    // token is the FRL access token, userId is the user's numeric Oculus id
    Result FetchMyWorlds(std::string token, std::string userId, Progress* progress);
}
