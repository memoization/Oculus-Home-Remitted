#pragma once
#include <string>

// Startup check comparing the running app version against the latest release tag of a GitHub repository.
namespace update
{
    // Start the background check once. currentVersion is the running build's version, ui.appVersion
    void StartCheck(const std::string& currentVersion);

    // True once the check finished and the latest release tag differs from the running version
    bool IsUpdateAvailable();

    // The latest tag the check found
    std::string LatestTag();

    // Open the repository releases page in the default browser
    void OpenReleasesPage();
}
