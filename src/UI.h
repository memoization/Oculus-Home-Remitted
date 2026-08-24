#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#define GLFW_EXPOSE_NATIVE_WIN32

#pragma once
#include <imgui.h>
#include "imgui_internal.h"
#include <picopng.h>
#include "misc/cpp/imgui_stdlib.h"
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <atomic>
#include <thread>
#include <future>
#include <chrono>
#include <windows.h>
#include <fstream>
#include <functional>
#include <map>
#include <vector>
#include <cstdint>
#include "TexLoader.h"
#include "BroadcastSource.h"
#include "Worlds.h"
#include "FetchWorlds.h"

struct ImScalers
{
    void Set(float s) { scale = s; }

    ImVec2 Vec2(float x, float y) const { return ImVec2(x * scale, y * scale); }
    ImVec2 Vec2(float size) const { return ImVec2(size * scale, size * scale); }

    // Scale a single float
    float F(float v) const { return v * scale; }

    float scale = 1.0f;
};

enum class PageType
{
    Profile,
    Worlds,
    ScreenSources,
    AppsLibrary
};

struct UIConst
{
    const ImVec4 GreyButtonFill = ImVec4(0.212f, 0.220f, 0.220f, 1);
    const ImVec4 GreyButtonHover = GreyButtonFill;
    const ImVec4 GreyButtonClick = ImVec4(0.14f, 0.14f, 0.14f, 1);
    const ImVec4 GreyButtonText = ImVec4(1, 1, 1, 1);

    const ImVec4 TextboxFill = ImVec4(0.212f, 0.220f, 0.220f, 1);
    const ImVec4 TextboxActive = ImVec4(0.501f, 0.317f, 1.78f, 1);
    const ImVec4 TextboxText = ImVec4(0.47f, 0.494f, 0.505f, 1);

    const ImVec4 SubText = ImVec4(0.47f, 0.494f, 0.505f, 1);
    const ImVec4 WarnText = ImVec4(0.98f, 0.76f, 0.22f, 1.0f);
    const ImVec4 ErrorText = ImVec4(1, 0, 0, 1);

    const ImVec4 ListItemHover = ImVec4(0.27f, 0.28f, 0.28f, 1.0f);
    const ImVec4 ListItemActive = ImVec4(0.33f, 0.34f, 0.34f, 1.0f);
    const ImVec4 ListItemHeader = ImVec4(0.30f, 0.31f, 0.31f, 1.0f);

    // Sidebar-navigation palette
    const ImVec4 SidebarFill = ImVec4(0.110f, 0.122f, 0.125f, 1);
    const ImVec4 LetterboxFill = ImVec4(0.122f, 0.129f, 0.137f, 1);
    const ImVec4 SourceListFill = ImVec4(0.212f, 0.220f, 0.220f, 1);
    const ImVec4 WorldCardFill = ImVec4(0.153f, 0.165f, 0.169f, 1);
    const ImVec4 NavItemActive = ImVec4(0.180f, 0.180f, 0.216f, 1);
    const ImVec4 NavItemHover = ImVec4(0.180f, 0.180f, 0.216f, 1);
    const ImVec4 LaunchButtonFill = ImVec4(0.204f, 0.451f, 0.965f, 1);
    const ImVec4 LaunchButtonHover = ImVec4(0.302f, 0.541f, 1.0f, 1);
    const ImVec4 LaunchButtonClick = ImVec4(0.153f, 0.353f, 0.800f, 1);
    const ImVec4 ContentOverlay = ImVec4(0.047f, 0.047f, 0.063f, 0.55f);
    const int SidebarWidth = 220;

    const int WindowWidth = 1100;
    const int WindowHeight = 663;
    const float SliderPadThickness = 4;
    const float CheckboxScale = 1.5f;

    const int PageContentPadding = 26;

    const std::string FontPath = "fonts/segoeui.ttf";
};

inline UIConst UIConsts;

struct Env
{
    PageType currentPage = PageType::Profile;
    std::string nextPopup;
    std::string noticeMessage;
    ImVec2 lastWinScale;
    bool doBuildFonts = false;
    uint64_t lastDPIUpdate = 0;

    int selectedWorld = 0;
    int selectedProfilePreset = -1;// staging selection for profile presets
    std::wstring selectedProfilePath = L"";// staging selection for profile icon browse
    int sourceKind = 0; // Screen Sources single-select: 0 = monitor, 1 = app
    uint64_t selectedSourceId = 0;// chosen HMONITOR/HWND (selection truth, survives list refresh)
};

// One selectable Screen-Sources row: a stable handle id & the display label. Both the Monitors and Apps columns build a vector of these from the enumerated sources.
struct SourceRowVM
{
    uint64_t id;
    const char* label;
};

struct UI
{
    GLFWwindow* window = nullptr;
    ImFont* font = nullptr;
    ImFont* fontHeader = nullptr;
    ImFont* fontTitle = nullptr;

    void Create();
    void Run();
    void Shutdown();
    void LoadFonts();
    ImVec2 UpdateScale();
    bool BringForeground(bool foregroundExternal);
    Env* GetEnv();

    void DrawSidebar();
    void DrawContent();
    bool NavItem(const char* label, const std::string& iconPath, PageType page);

    void DoProfile();
    void LoadProfileImage();
    void ApplyProfilePak(); // build icon profile .pak override from the current profile.png
    void DoWorlds();
    void DoScreens();
    void DoApps();
    void RefreshSources();
    void SourceColumn(const char* title, int kind, const std::vector<SourceRowVM>& items, ImVec2 size, int entryTextWidth);
    bool SourceRow(const char* label, bool selected, int textWidth);
    void DoPopups(Env& rs);

    void DoLaunchHome();
    void DoSetExecutable();
    void RebuildAppsLibrary(); // re-scan library roots into store\apps-library.json, refreshes appsFoundCount
    bool BrowseForFolder(std::string& outPath); // shell folder picker, true if the user chose a folder

    std::atomic<bool> keepAlive;
    std::atomic<bool> inactive;
    int pushedStyles = 0;
    std::string profileName = "Player";
    std::string profileImagePath; // abs path to the user's profile.png (from prefs), "" means default
    bool reloadProfileOnOpen = true; // re-read prefs when the Profile page is (re)opened (no poll)
    std::string home2ExePath; // Home2 exe for the Launch Home button (from preferences.json)
    std::string iconPakStatus;// last result of building the profile override pak (shown on Profile page)
    const char* appVersion = "0.0.0";

    // scanned per-world folders (list, select, Set Default). Re-scanned on page (re)open (one-shot, mirrors reloadProfileOnOpen, no background poll).
    std::vector<worlds::WorldCardInfo> worldList;
    bool reloadWorldsOnOpen = true;

    // Apps Library: user-added Oculus library roots
    // On page (re)open the library is re-scanned into store\apps-library.json and the found-count refreshed, and adding/removing a location also rebuilds it.
    std::vector<std::string> libraryPaths;
    int appsFoundCount = 0;
    bool reloadAppsOnOpen = true;

    // "Fetch My Homes": download the user's remote worlds from graph.oculus.com into store\worlds.
    std::string fetchToken;
    std::string fetchUserId;
    std::string fetchResultMsg;// shown in the popup after completion
    bool fetchResultOk = false;
    bool fetchRunning = false;
    fetchworlds::Progress fetchProgress;// written by the worker, read by the UI
    std::future<fetchworlds::Result> fetchFuture;

    //Screen Sources enumeration cache (throttled re-enumeration while on the page).
    std::vector<broadcast::MonitorSource> monitorSources;
    std::vector<broadcast::AppSource> appSources;
    uint64_t sourcesLastRefresh = 0;
};

extern UI ui;
extern ImScalers iScale;