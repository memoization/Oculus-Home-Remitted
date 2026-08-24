// Prevent Windows headers from defining `min`/`max` macros which break C++ std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <shlwapi.h>
#include "resource.h"
#include "UI.h"
#include <filesystem>
#include <regex>
#include <cstdio>
#include "HomeLogger.h"
#include <CommCtrl.h>
#include <commdlg.h>
#include "Prefs.h"
#include "IconPak.h"
#include "AppLibraries.h"
#include "BroadcastSource.h"
#include <shobjidl.h>
#include <cpr/cpr.h>

#pragma comment(lib, "ole32.lib") // IFileOpenDialog folder picker (CoCreateInstance/CoTaskMemFree)

UI ui;
ImScalers iScale;
Env env;
bool windowClosed;

/// Styles
int PushSubTextStyle()
{
    ImGui::PushStyleColor(ImGuiCol_Text, UIConsts.SubText);
    return 1;
}

int PushTextInputStyle()
{
    ImGui::PushStyleColor(ImGuiCol_FrameBg, UIConsts.TextboxFill);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, UIConsts.TextboxActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, UIConsts.TextboxActive);
    ImGui::PushStyleColor(ImGuiCol_Text, UIConsts.TextboxText);
    return 4;
}

int PushTextInputHintStyle()
{
    ImGui::PushStyleColor(ImGuiCol_FrameBg, UIConsts.TextboxFill);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, UIConsts.TextboxActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, UIConsts.TextboxActive);
    ImGui::PushStyleColor(ImGuiCol_Text, UIConsts.TextboxText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    return 5;
}

int PushButtonStyleGrey()
{
    ImGui::PushStyleColor(ImGuiCol_Button, UIConsts.GreyButtonFill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIConsts.GreyButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIConsts.GreyButtonClick);
    ImGui::PushStyleColor(ImGuiCol_Text, UIConsts.GreyButtonText);
    return 4;
}

int PushLaunchButtonStyle()
{
    ImGui::PushStyleColor(ImGuiCol_Button, UIConsts.LaunchButtonFill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIConsts.LaunchButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIConsts.LaunchButtonClick);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    return 4;
}

int PushNavItemStyle(bool active)
{
    ImGui::PushStyleColor(ImGuiCol_Header, UIConsts.NavItemActive);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, active ? UIConsts.NavItemActive : UIConsts.NavItemHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, UIConsts.NavItemActive);
    ImGui::PushStyleColor(ImGuiCol_Text, UIConsts.GreyButtonText);
    return 4;
}

void CenteredCursor(float width)
{
    auto windowWidth = ImGui::GetWindowSize().x;
    ImGui::SetCursorPosX((windowWidth - width) * 0.5f);
}

void CenteredText(const std::string& text, bool adjustToPadding = false)
{
    auto windowWidth = ImGui::GetWindowSize().x;
    auto textWidth = ImGui::CalcTextSize(text.c_str()).x;
    ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f - (adjustToPadding ? UIConsts.PageContentPadding : 0));
    ImGui::Text(text.c_str());
}

void CenteredTextWrapped(const std::string& text)
{
    auto windowWidth = ImGui::GetWindowSize().x;
    auto textWidth = ImGui::CalcTextSize(text.c_str(), nullptr, false, windowWidth).x;
    ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
    ImGui::TextWrapped(text.c_str());
}

bool CenteredButton(const std::string& text, ImVec2 size = iScale.Vec2(0, 0))
{
    auto windowWidth = ImGui::GetWindowSize().x;
    float buttonWidth = size.x > 0 ? size.x : ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX((windowWidth - buttonWidth) * 0.5f);
    return ImGui::Button(text.c_str(), size);
}

void ShowTooltip(const char* msg)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, iScale.Vec2(10, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, iScale.F(5));

    float maxWidth = ImGui::GetWindowSize().x - iScale.F(150);

    ImGui::BeginTooltip();

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + maxWidth);
    ImGui::Text(msg);
    ImGui::PopTextWrapPos();

    ImGui::EndTooltip();

    ImGui::PopStyleVar(2);
}

static ImTextureID ToTexId(GLuint tex)
{
    return (ImTextureID)(intptr_t)tex;
}

// Center-crop ("cover") UVs so a texW x texH image fills a dst rect without stretching. The overflowing axis is trimmed equally on both sides
static void CoverUV(int texW, int texH, float dstW, float dstH, ImVec2& uv0, ImVec2& uv1)
{
    uv0 = ImVec2(0.0f, 0.0f);
    uv1 = ImVec2(1.0f, 1.0f);
    if (texW <= 0 || texH <= 0 || dstW <= 0.0f || dstH <= 0.0f)
        return;

    float imgAspect = (float)texW / (float)texH;
    float dstAspect = dstW / dstH;
    if (imgAspect > dstAspect)
    {
        float w = dstAspect / imgAspect; // trim left/right
        uv0.x = (1.0f - w) * 0.5f;
        uv1.x = 1.0f - uv0.x;
    }
    else
    {
        float h = imgAspect / dstAspect; // trim top/bottom
        uv0.y = (1.0f - h) * 0.5f;
        uv1.y = 1.0f - uv0.y;
    }
}

// Letterbox band sizes (unscaled px, run through iScale.F). Shared by DrawContent.
static const float LetterboxTopBand = 70.0f;
static const float LetterboxBottomBand = 116.0f;

// #1f2123 letterbox color at a 0..1 opacity (clamped). IM_COL32's alpha is a single 0..255 byte, so raw values above 255 overflow (e.g. 1024 becomes 0) and the gradient can vanish. 1.0 here is fully opaque, so there is nothing heavier than that.
static ImU32 LetterboxShade(float opacity)
{
    opacity = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
    return IM_COL32(31, 33, 35, (int)(opacity * 255.0f + 0.5f));
}

// A world card for the Worlds page right-pane: cover-cropped thumbnail, name label, selection ring
static bool WorldCard(const char* name, GLuint thumb, int thumbW, int thumbH, ImVec2 size, bool selected, bool isDefault)
{
    // Card frame margin around the world image, and the height of the background band above the image that holds the world name
    float imagePad = iScale.F(6);
    float titleAreaH = iScale.F(24);

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
    bool clicked = ImGui::InvisibleButton(name, size);
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = iScale.F(6);

    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(isDefault ? UIConsts.ListItemActive : UIConsts.WorldCardFill), rounding);

    // World name in the background band above the image
    float textH = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(p0.x + imagePad, p0.y + (titleAreaH - textH) * 0.5f), IM_COL32_WHITE, name);

    // World image below the title band, inset by imagePad on the other sides
    ImVec2 imgP0 = ImVec2(p0.x + imagePad, p0.y + titleAreaH);
    ImVec2 imgP1 = ImVec2(p1.x - imagePad, p1.y - imagePad);
    float imgRounding = iScale.F(4);
    if (thumb)
    {
        ImVec2 uv0, uv1;
        CoverUV(thumbW, thumbH, imgP1.x - imgP0.x, imgP1.y - imgP0.y, uv0, uv1);
        dl->AddImageRounded(ToTexId(thumb), imgP0, imgP1, uv0, uv1, IM_COL32_WHITE, imgRounding);
    }
    else
    {
        dl->AddRectFilled(imgP0, imgP1, IM_COL32(30, 30, 38, 255), imgRounding);
    }

    ImU32 border = selected ? IM_COL32(120, 140, 255, 255) : (hovered ? IM_COL32(255, 255, 255, 130) : IM_COL32(255, 255, 255, 45));
    dl->AddRect(p0, p1, border, rounding, 0, iScale.F(selected ? 2.5f : 1.0f));
    return clicked;
}

void WindowCloseCallback(GLFWwindow* window)
{
    // Close to tray: hide the window and keep the process (and the injector watcher) running
    windowClosed = true;
    glfwSetWindowShouldClose(window, GLFW_FALSE);
    ShowWindow(glfwGetWin32Window(window), SW_HIDE);
}

bool UI::BringForeground(bool foregroundExternal)
{
    HWND hwnd = foregroundExternal ? FindWindow(NULL, L"Oculus Home Remitted") : glfwGetWin32Window(window);
    if (hwnd)
    {
        // If the window is minimized, restore it
        if (IsIconic(hwnd))
        {
            ShowWindow(hwnd, SW_RESTORE);
        }
        else
        {
            ShowWindow(hwnd, SW_SHOW);
        }

        SetForegroundWindow(hwnd);
        return true;
    }

    return false;
}

Env* UI::GetEnv()
{
    return &env;
}

void UI::LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig mergeConfig;
    mergeConfig.MergeMode = true;
    mergeConfig.PixelSnapH = true;
    mergeConfig.GlyphMinAdvanceX = 0.0f;

    if (std::filesystem::exists(UIConsts.FontPath))
    {
        io.Fonts->ClearFonts();
        io.FontDefault = nullptr;

        font = io.Fonts->AddFontFromFileTTF(UIConsts.FontPath.c_str(), iScale.F(23));
        fontHeader = io.Fonts->AddFontFromFileTTF(UIConsts.FontPath.c_str(), iScale.F(29));
        fontTitle = io.Fonts->AddFontFromFileTTF(UIConsts.FontPath.c_str(), iScale.F(48));

        if (font)
        {
            io.FontDefault = font;
        }
    }
    // When falling back to imgui default
    else
    {
        io.FontGlobalScale = iScale.scale;
    }
}

ImVec2 UI::UpdateScale()
{
    float xscale, yscale;
    glfwGetWindowContentScale(window, &xscale, &yscale);

    xscale = std::clamp(xscale, 0.0f, 2.0f);
    yscale = std::clamp(yscale, 0.0f, 2.0f);

    iScale.Set(xscale);

    return ImVec2(xscale, yscale);
}

void UI::Create()
{
    if (glfwInit() == GLFW_FALSE)
    {
        keepAlive.store(false);
        return;
    }

    // Create window.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    window = glfwCreateWindow(UIConsts.WindowWidth, UIConsts.WindowHeight, "Oculus Home Remitted", NULL, NULL);
    if (!window)
    {
        keepAlive.store(false);
        return;
    }

    // Intercept close calls to just minimize to systray.
    glfwSetWindowCloseCallback(window, WindowCloseCallback);

    // Center window on-screen.
    int maxWidth = GetSystemMetrics(SM_CXSCREEN);
    int maxHeight = GetSystemMetrics(SM_CYSCREEN);

    ImVec2 winScale = UpdateScale();

    glfwSetWindowMonitor(window, NULL,
        (maxWidth / 2) - (UIConsts.WindowWidth / 2),
        (maxHeight / 2) - (UIConsts.WindowHeight / 2),
        UIConsts.WindowWidth * winScale.x, UIConsts.WindowHeight * winScale.y, GLFW_DONT_CARE);

    // Setup framebuffer.
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);

    // Setup GUI.
    ImGui::CreateContext();
    ImGuiIO& guiIo = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Load fonts
    LoadFonts();

    // Style scaling
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(winScale.x);

    // Sidebar navigation art and default-world content background
    texLoader.LoadPng("images/profile.png");
    texLoader.LoadPng("images/home.png");
    texLoader.LoadPng("images/desktop.png");
    texLoader.LoadPng("images/library.png");
    texLoader.LoadPng("images/world-default.png");

    // Profile-picture presets for the selector modal (images/profiles/1.png .. 32.png)
    for (int i = 1; i <= 32; ++i)
    {
        texLoader.LoadPng("images/profiles/" + std::to_string(i) + ".png");
    }

    // Write the default preferences.json on first run
    prefs::SeedDefaultsIfMissing();

    // Auto-create the default world if store\worlds\ folder is empty (also set preferences.defaultWorldId), then scan the folders for the Worlds page and screenshot background.
    // The backend reads the folders/prefs
    worlds::EnsureValidDefault();
    worldList = worlds::Scan();
    for (int i = 0; i < (int)worldList.size(); ++i)
    {
        if (worldList[i].isDefault)
        {
            env.selectedWorld = i;
            break;
        }
    }

    // Home2 exe path for the Launch Home button
    home2ExePath = prefs::GetHome2ExePath();

    // The profile identity and sidebar image (backend reads the same file)
    profileName = prefs::GetDisplayName();
    profileImagePath = prefs::GetProfileImagePath();
    LoadProfileImage();

    // Screen Sources live channel: create the shared block and default to the primary monitor. The selection is in memory, this session only
    broadcast::Init();
    env.sourceKind = 0;
    env.selectedSourceId = broadcast::PrimaryMonitor();

    // Attempt to scan the user's Oculus app library
    RebuildAppsLibrary();
}

#pragma region Rendered Pages
void UI::LoadProfileImage()
{
    // Load the user's profile.png by absolute path.
    // Reload (not LoadPng) because the path key is fixed and only the file's bytes change between selections
    if (!profileImagePath.empty() && std::filesystem::exists(std::filesystem::path(prefs::Widen(profileImagePath))))
    {
        texLoader.Reload(profileImagePath);
    }
}

void UI::ApplyProfilePak()
{
    std::string err;
    std::string png = prefs::Narrow(prefs::AppDir() + L"profile.png");
    if (iconpak::BuildProfilePak(home2ExePath, png, err))
    {
        homeLogger.write() << "iconpak: wrote the icon profile .pak file" << std::endl;
    }
    else
    {
        iconPakStatus = "Unable to build icon for Home: " + err;
        homeLogger.write() << "iconpak: failed, " << err.c_str() << std::endl;
    }
}

void UI::DoProfile()
{
    ImVec2 avail = ImGui::GetContentRegionAvail();

    // Re-read prefs on (re)open so any on-disk change (hand edit, or the file this page wrote earlier) is reflected
    if (reloadProfileOnOpen)
    {
        profileName = prefs::GetDisplayName();
        profileImagePath = prefs::GetProfileImagePath();
        LoadProfileImage();
        reloadProfileOnOpen = false;
    }

    ImGui::Dummy(iScale.Vec2(0, 25));

    // Big header profile name
    ImGui::PushFont(fontTitle);
    ImGui::TextUnformatted(profileName.empty() ? "Profile" : profileName.c_str());
    ImGui::PopFont();

    ImGui::Dummy(iScale.Vec2(0, 25));

    // Profile Name field
    ImGui::PushFont(fontHeader);
    ImGui::TextUnformatted("Profile Name");
    ImGui::PopFont();
    ImGui::SetNextItemWidth(iScale.F(320));
    pushedStyles = PushTextInputStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, iScale.Vec2(10, 8));
    ImGui::InputText("##profileName", &profileName);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(pushedStyles);

    // Persist on edit-commit (Enter / focus loss). The probe reads this at the next Home launch no restart tracking here
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        prefs::SetDisplayName(profileName);
    }

    ImGui::Dummy(iScale.Vec2(0, 14));

    // Profile Image opens the profile-picture selector popup
    ImGui::PushFont(fontHeader);
    ImGui::TextUnformatted("Profile Image");
    ImGui::PopFont();
    pushedStyles = PushButtonStyleGrey();
    if (ImGui::Button("Browse", iScale.Vec2(130, 36)))
    {
        env.selectedProfilePreset = -1;
        env.selectedProfilePath = L"";
        env.nextPopup = "Choose Your Profile Picture";
    }
    ImGui::PopStyleColor(pushedStyles);

    // Result of the icon pak build
    if (!iconPakStatus.empty())
    {
        env.noticeMessage = iconPakStatus;
        env.nextPopup = "Notice";
        iconPakStatus = "";
    }

    ImGui::SetCursorPosY(avail.y - iScale.F(55));
    pushedStyles = PushSubTextStyle();
    CenteredText("Profile changes will show in Home after it restarts.", true);
    ImGui::PopStyleColor(pushedStyles);
}

// The label for a world (home) as shown in the list: its custom name or "Home #<name_index>" if the name is empty
// the field "name_index" is the display num the backend assigns as (worlds.size plus 1) at creation, so it stays bounded to the total number of worlds, unlike creation_index
static std::string WorldLabel(const worlds::WorldCardInfo& info)
{
    return info.name.empty() ? ("Home #" + std::to_string(1 + info.nameIndex)) : info.name;
}

void UI::DoWorlds()
{
    ImGui::Dummy(iScale.Vec2(0, 25));

    // Re-scan the folders on (re)open so an in-VR create/rename/screenshot or a hand edit is reflected
    if (reloadWorldsOnOpen)
    {
        worlds::EnsureValidDefault(); // self-heal a deleted default before rescanning
        worldList = worlds::Scan();
        if (env.selectedWorld >= (int)worldList.size())
        {
            env.selectedWorld = 0;
        }

        for (int i = 0; i < (int)worldList.size(); ++i)
        {
            if (worldList[i].isDefault)
            {
                env.selectedWorld = i;
                break;
            }
        }

        // Decode screenshots async off the UI thread
        for (const auto& info : worldList)
        {
            if (!info.screenshotPng.empty()) 
            {
                texLoader.RequestAsync(info.screenshotPng);
            }
        }
                
        reloadWorldsOnOpen = false;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float railWidth = iScale.F(210);
    float gap = iScale.F(16);
    float mainWidth = avail.x - railWidth - gap;

    bool hasWorlds = !worldList.empty();
    int sel = env.selectedWorld;

    if (sel < 0 || sel >= (int)worldList.size())
    {
        sel = 0;
    }

    std::string worldName = hasWorlds ? WorldLabel(worldList[sel]) : "No Homes Available";
    int objectCount = hasWorlds ? worldList[sel].objectCount : 0;

    // Main column: selected-world title, object count, and "Set Default"
    ImGui::BeginChild("##worldMain", ImVec2(mainWidth, avail.y), false);
    {
        ImGui::PushFont(fontTitle);
        ImGui::TextUnformatted(worldName.c_str());
        ImGui::PopFont();
        ImGui::Dummy(iScale.Vec2(0, 25));

        if (hasWorlds)
        {
            // reflects 500 = world_login: max_objects_in_worlds
            ImGui::PushFont(fontHeader);
            int shownObjectCount = objectCount > 0 ? objectCount - 1 : 0;
            ImGui::Text("Objects   %d / 500", shownObjectCount);
            ImGui::PopFont();
        }

        // UGC notice: the selected world references custom content the user uploaded (detected in worlds::Scan via the item_definition typename / customizations.UGCBase)
        if (hasWorlds && (worldList[sel].ugcBase || worldList[sel].ugcObjectCount > 0))
        {
            const worlds::WorldCardInfo& uw = worldList[sel];
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));

            ImGui::BeginChild("NoticeBox", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

            ImGui::PushStyleColor(ImGuiCol_Text, UIConsts.WarnText);
            ImGui::PushTextWrapPos(0.0f);

            if (uw.ugcBase)
            {
                ImGui::TextWrapped("This home uses a custom (UGC) map. The UGC assets must be present in the home's \"ugc\" folder or the app will stall while loading.");
            }

            if (uw.ugcObjectCount > 0)
            {
                ImGui::TextWrapped(
                    "This home includes %d custom (UGC) item%s.",
                    uw.ugcObjectCount,
                    uw.ugcObjectCount == 1 ? "" : "s"
                );
            }

            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            ImGui::EndChild();

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }

        // Footer: Fetch My Homes (left) and Set Default (right), centered as a pair
        ImGui::SetCursorPosY(avail.y - iScale.F(66));
        float btnH = iScale.F(40);
        float fetchW = iScale.F(180);
        float setW = iScale.F(160);
        float btnGap = iScale.F(12);
        float colW = ImGui::GetWindowSize().x;
        float pairStartX = (colW - (fetchW + btnGap + setW) - UIConsts.PageContentPadding) * 0.5f;
        if (pairStartX < 0.0f)
        {
            pairStartX = 0.0f;
        }
            
        ImGui::SetCursorPosX(pairStartX);

        pushedStyles = PushButtonStyleGrey();
        if (ImGui::Button("Fetch My Homes", ImVec2(fetchW, btnH)))
        {
            fetchResultMsg.clear();
            fetchResultOk = false;
            env.nextPopup = "Fetch My Homes";
        }
        ImGui::PopStyleColor(pushedStyles);

        ImGui::SameLine(0.0f, btnGap);

        pushedStyles = PushButtonStyleGrey();
        if (!hasWorlds) ImGui::BeginDisabled();
        if (ImGui::Button("Set Default", ImVec2(setW, btnH)))
        {
            // Takes effect in-VR on the next Home launch (the probe reads preferences.defaultWorldId at load)
            prefs::SetDefaultWorldId(worldList[sel].worldId);
            for (auto& w : worldList)
            {
                w.isDefault = (w.worldId == worldList[sel].worldId);
            }

            homeLogger.write() << "Set default home: " << worldList[sel].worldId.c_str() << "." << std::endl;
        }
        if (!hasWorlds) ImGui::EndDisabled();
            
        ImGui::PopStyleColor(pushedStyles);
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);

    float railPadX = 12.0f; // definable left/right padding of the cards inside the rail background
    ImGui::PushStyleColor(ImGuiCol_ChildBg, UIConsts.SidebarFill); // same color as the left nav
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, iScale.Vec2(0, 26));
    ImGui::BeginChild("##worldRail", ImVec2(railWidth, avail.y), false);
    {
        auto [fallback, fallbackW, fallbackH] = texLoader.GetPng("images/world-default.png");
        float pad = iScale.F(railPadX);
        float cardW = ImGui::GetContentRegionAvail().x - pad * 2.0f;
        float cardH = iScale.F(90);
        for (int i = 0; i < (int)worldList.size(); ++i)
        {
            const worlds::WorldCardInfo& info = worldList[i];
            GLuint thumb = fallback;
            int thumbW = fallbackW, thumbH = fallbackH;
            if (!info.screenshotPng.empty())
            {
                // Async-decoded (RequestAsync on open, PumpUploads each frame), 0 until ready, so the default thumbnail shows in the meantime instead of blocking the UI render thread
                auto [t, tw, th] = texLoader.GetPng(info.screenshotPng);
                if (t)
                {
                    thumb = t;
                    thumbW = tw;
                    thumbH = th;
                }
            }
            std::string label = WorldLabel(info);
            ImGui::SetCursorPosX(pad);
            ImGui::PushID(i); // duplicate imgui labels (e.g. two "Home #0") must not collide
            if (WorldCard(label.c_str(), thumb, thumbW, thumbH, ImVec2(cardW, cardH), i == sel, info.isDefault))
            {
                env.selectedWorld = i;
            }
            ImGui::PopID();
            ImGui::Dummy(iScale.Vec2(0, 8));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Re-enumerate the live monitor / app-window sources (rated at 1/sec while on the page)
// If the current selection has vanished (monitor unplugged, window closed), fall back to the primary monitor and rewrite the shared channel so the UI and the backend stay consistent
void UI::RefreshSources()
{
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!monitorSources.empty() && now - sourcesLastRefresh < 1)
    {
        return;
    }

    sourcesLastRefresh = now;

    monitorSources = broadcast::EnumMonitorSources();
    appSources = broadcast::EnumAppWindows();

    bool present = false;
    if (env.sourceKind == 0)
    {
        for (auto& m : monitorSources)
        {
            if (m.handle == env.selectedSourceId)
            {
                present = true;
                break;
            }
        }
    }
    else
    {
        for (auto& a : appSources)
        {
            if (a.hwnd == env.selectedSourceId)
            {
                present = true;
                break;
            }
        }
    }

    if (!present)
    {
        env.sourceKind = 0;
        env.selectedSourceId = broadcast::PrimaryMonitor();
        broadcast::SetMonitor(env.selectedSourceId);
    }
}

void UI::DoScreens()
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(iScale.Vec2(0, 25));

    ImGui::PushFont(fontTitle);
    ImGui::TextUnformatted("Screen Sources");
    ImGui::PopFont();
    ImGui::Dummy(iScale.Vec2(0, 25));

    RefreshSources();

    float colGap = iScale.F(24);
    float colW = (iScale.F(420) - colGap);

    // Keep the list panels above the footer
    float listH = iScale.F(300);

    // Build the list column view-models from the sources (labels live in the caches, so the c_str() pointers stay valid for this frame)
    std::vector<SourceRowVM> monitorRows;
    monitorRows.reserve(monitorSources.size());
    for (auto& m : monitorSources)
    {
        monitorRows.push_back({ m.handle, m.label.c_str() });
    }

    std::vector<SourceRowVM> appRows;
    appRows.reserve(appSources.size());
    for (auto& a : appSources)
    {
        appRows.push_back({ a.hwnd, a.title.c_str() });
    }

    SourceColumn("Monitors", 0, monitorRows, ImVec2(colW, listH), iScale.F(360));
    ImGui::SameLine(0.0f, colGap);
    SourceColumn("Apps", 1, appRows, ImVec2(colW, listH), iScale.F(360));

    ImGui::SetCursorPosY(avail.y - iScale.F(55));
    pushedStyles = PushSubTextStyle();
    CenteredText("Change the display source for broadcasting on-screen objects.", true);
    ImGui::PopStyleColor(pushedStyles);
}

void UI::DoApps()
{
    ImVec2 avail = ImGui::GetContentRegionAvail();

    // On page (re)open, reload the user's locations from prefs and rebuild store\apps-library.json so the count and backend feed reflect any apps installed since last time
    if (reloadAppsOnOpen)
    {
        libraryPaths = prefs::GetOculusLibraryPaths();
        RebuildAppsLibrary();
        reloadAppsOnOpen = false;
    }

    ImGui::Dummy(iScale.Vec2(0, 25));

    ImGui::PushFont(fontTitle);
    ImGui::TextUnformatted("Apps Library");
    ImGui::PopFont();

    ImGui::Dummy(iScale.Vec2(0, 25));

    ImGui::PushFont(fontHeader);
    ImGui::Text("Found Apps: %d", appsFoundCount);
    ImGui::PopFont();

    ImGui::Dummy(iScale.Vec2(0, 14));

    // Library Locations list, rendered with the shared Screen-Sources list widget
    const int kLibraryKind = 2;
    std::vector<SourceRowVM> rows;
    rows.reserve(libraryPaths.size());
    for (size_t i = 0; i < libraryPaths.size(); ++i)
    {
        rows.push_back({ (uint64_t)(i + 1), libraryPaths[i].c_str() });
    }

    float listW = iScale.F(600);
    float listH = iScale.F(240);
    SourceColumn("Library Locations", kLibraryKind, rows, ImVec2(listW, listH), (int)iScale.F(560));

    // Footer: Add and Remove (Remove acts on the selected location row).
    bool hasSelection = env.sourceKind == kLibraryKind && env.selectedSourceId >= 1 && env.selectedSourceId <= libraryPaths.size();

    ImGui::SetCursorPosY(avail.y - iScale.F(85));
    float btnH = iScale.F(40);
    float btnW = iScale.F(180);
    float gap = iScale.F(14);
    ImGui::SetCursorPosX((avail.x - (btnW * 2 + gap)) / 2 - UIConsts.PageContentPadding);

    pushedStyles = PushButtonStyleGrey();
    if (ImGui::Button("Add Location", ImVec2(btnW, btnH)))
    {
        std::string folder;
        if (BrowseForFolder(folder) && !folder.empty())
        {
            bool exists = false;
            for (const auto& p : libraryPaths)
            {
                if (_stricmp(p.c_str(), folder.c_str()) == 0)
                {
                    exists = true;
                    break;
                }
            }

            if (!exists && _stricmp(folder.c_str(), applibraries::DefaultRoot().c_str()) != 0)
            {
                libraryPaths.push_back(folder);
                prefs::SetOculusLibraryPaths(libraryPaths);
                RebuildAppsLibrary();
            }
        }
    }
    ImGui::PopStyleColor(pushedStyles);

    ImGui::SameLine(0, gap);

    ImGui::BeginDisabled(!hasSelection);
    pushedStyles = PushButtonStyleGrey();
    if (ImGui::Button("Remove Selected", ImVec2(btnW, btnH)))
    {
        libraryPaths.erase(libraryPaths.begin() + (size_t)(env.selectedSourceId - 1));
        prefs::SetOculusLibraryPaths(libraryPaths);
        env.selectedSourceId = 0; // clear the stale selection
        RebuildAppsLibrary();
    }
    ImGui::PopStyleColor(pushedStyles);
    ImGui::EndDisabled();

    pushedStyles = PushSubTextStyle();
    CenteredText("Add an \"Oculus Apps\" folder that contains \"Manifests\" and \"Software\\StoreAssets\".", true);
    ImGui::PopStyleColor(pushedStyles);
}

// Re-scan the default CoreData location and the user's added roots into store\apps-library.json
// The backend feeds this to the worlds_apps_and_achievements graphql request on next launch.
void UI::RebuildAppsLibrary()
{
    int n = applibraries::Rebuild(libraryPaths);
    appsFoundCount = (n < 0) ? 0 : n;
    homeLogger.write() << "Apps Library rebuilt: " << appsFoundCount << " app(s)." << std::endl;
}

// Modern shell folder picker (IFileOpenDialog with FOS_PICKFOLDERS). Returns true and the chosen filesystem path if the user confirmed a folder.
bool UI::BrowseForFolder(std::string& outPath)
{
    bool ok = false;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool didInit = SUCCEEDED(hrInit);

    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Select an Oculus library folder (contains Manifests and Software\\StoreAssets)");

        if (SUCCEEDED(dlg->Show(glfwGetWin32Window(window))))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)))
            {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz)
                {
                    outPath = prefs::Narrow(psz);
                    ok = true;
                    CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    if (didInit)
    {
        CoUninitialize();
    }

    return ok;
}

// A Screen-Sources column ("Monitors" / "Apps"): a titled, scrollable list panel driven by the enumerated sources. Selection is keyed on the source's handle id not a list index,
// so it survives list refreshes, and a click publishes the pick into the shared broadcast channel.
void UI::SourceColumn(const char* title, int kind, const std::vector<SourceRowVM>& items, ImVec2 size, int entryTextWidth)
{
    ImGui::BeginGroup();

    ImGui::PushFont(fontHeader); 
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(iScale.Vec2(0, 4));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, UIConsts.SourceListFill);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, iScale.F(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, iScale.Vec2(8, 8));
    ImGui::BeginChild(title, size, true);

    for (size_t i = 0; i < items.size(); ++i)
    {
        ImGui::PushID((int)i); // Distinct ImGui id per row so duplicate labels don't collide

        bool selected = env.sourceKind == kind && env.selectedSourceId == items[i].id;
        if (SourceRow(items[i].label, selected, entryTextWidth))
        {
            env.sourceKind = kind;
            env.selectedSourceId = items[i].id;

            // Screen-Sources kinds publish to the broadcast channel. Other lists (Apps Library locations) reuse the same list widget for selection only.
            if (kind == 0)
                broadcast::SetMonitor(items[i].id);
            else if (kind == 1)
                broadcast::SetApp(items[i].id);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::EndGroup();
}

// Shorten `text` with a trailing ellipsis so it fits within maxWidth px in the current font.
// Returns the input unchanged when it already fits.
static std::string TruncateToWidth(const char* text, float maxWidth)
{
    if (maxWidth <= 0.0f) return std::string();
    if (ImGui::CalcTextSize(text).x <= maxWidth) return std::string(text);

    const char* ellipsis = "\xE2\x80\xA6"; // U+2026
    float budget = maxWidth - ImGui::CalcTextSize(ellipsis).x;
    std::string in(text);
    size_t cut = 0;
    for (size_t i = 0; i < in.size(); )
    {
        size_t next = i + 1; // advance one UTF-8 codepoint
        while (next < in.size() && ((unsigned char)in[next] & 0xC0) == 0x80)
        {
            next++;
        }

        if (ImGui::CalcTextSize(in.c_str(), in.c_str() + next).x > budget) break;
        cut = next;
        i = next;
    }
    return in.substr(0, cut) + ellipsis;
}

// One selectable source row with a checkmark on the single active source.
bool UI::SourceRow(const char* label, bool selected, int textWidth)
{
    float rowH = iScale.F(36);
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p0 = ImGui::GetCursorScreenPos();

    // Highlights sit above the panel, so they read a touch lighter, not darker.
    ImGui::PushStyleColor(ImGuiCol_Header, UIConsts.ListItemHeader);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, UIConsts.ListItemHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, UIConsts.ListItemActive);
    std::string id = "##src_" + std::string(label);
    bool clicked = ImGui::Selectable(id.c_str(), selected, 0, ImVec2(w, rowH));
    ImGui::PopStyleColor(3);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float th = ImGui::GetTextLineHeight();

    float leftPad = iScale.F(10);
    std::string shown = TruncateToWidth(label, textWidth);

    dl->AddText(ImVec2(p0.x + leftPad, p0.y + (rowH - th) * 0.5f), IM_COL32_WHITE, shown.c_str());
    if (shown != label && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", label); // full text on hover when shortened
    }

    return clicked;
}
#pragma endregion

#pragma region Sidebar + Content
bool UI::NavItem(const char* label, const std::string& iconPath, PageType page)
{
    bool active = env.currentPage == page;
    float rowHeight = iScale.F(38);
    float iconSize = iScale.F(20);
    float padX = iScale.F(8);
    float availWidth = ImGui::GetContentRegionAvail().x;
    ImVec2 start = ImGui::GetCursorPos();

    pushedStyles = PushNavItemStyle(active);
    std::string id = "##nav_" + std::string(label);
    bool clicked = ImGui::Selectable(id.c_str(), active, 0, ImVec2(availWidth, rowHeight));
    ImGui::PopStyleColor(pushedStyles);

    if (clicked)
    {
        // Re-read the Profile page's prefs the next time it renders after a switch to it.
        if (page == PageType::Profile && env.currentPage != PageType::Profile)
        {
            reloadProfileOnOpen = true;
        }

        // Re-scan the Worlds folder on switch-to (picks up in-VR create/rename/screenshot).
        if (page == PageType::Worlds && env.currentPage != PageType::Worlds)
        {
            reloadWorldsOnOpen = true;
        }

        // Re-scan the Oculus library on switch-to (rebuilds store\apps-library.json).
        if (page == PageType::AppsLibrary && env.currentPage != PageType::AppsLibrary)
        {
            reloadAppsOnOpen = true;
        }

        env.currentPage = page;
    }

    auto [icon, iconW, iconH] = texLoader.GetPng(iconPath);
    if (icon)
    {
        ImGui::SetCursorPos(ImVec2(start.x + padX, start.y + (rowHeight - iconSize) * 0.5f));
        ImGui::Image(ToTexId(icon), ImVec2(iconSize, iconSize));
    }

    float textHeight = ImGui::GetTextLineHeight();
    ImGui::SetCursorPos(ImVec2(start.x + padX + iconSize + iScale.F(10), start.y + (rowHeight - textHeight) * 0.5f));
    ImGui::TextUnformatted(label);

    ImGui::SetCursorPos(ImVec2(start.x, start.y + rowHeight + iScale.F(4)));
    return clicked;
}

void UI::DrawSidebar()
{
    float sidebarWidth = iScale.F((float)UIConsts.SidebarWidth);
    float fullHeight = ImGui::GetContentRegionAvail().y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, UIConsts.SidebarFill);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, iScale.Vec2(14, 18));
    ImGui::BeginChild("##sidebar", ImVec2(sidebarWidth, fullHeight), false, ImGuiWindowFlags_NoScrollbar);

    float availWidth = ImGui::GetContentRegionAvail().x;

    // Avatar: user image if set and present, else the default identity picture "32.png"
    // Drawn as a circle to match the selector-modal cells: AddImageRounded (radius is half the box) with cover-crop.
    ImGui::Dummy(iScale.Vec2(0, 6));

    float avatarSize = iScale.F(72);
    GLuint avatar = 0;
    int avatarW = 0, avatarH = 0;
    std::string avatarKey = "images/profiles/32.png";
    if (!profileImagePath.empty() && std::filesystem::exists(std::filesystem::path(prefs::Widen(profileImagePath))))
    {
        avatarKey = profileImagePath;
    }
    std::tie(avatar, avatarW, avatarH) = texLoader.GetPng(avatarKey);
    if (!avatar)
    {
        std::tie(avatar, avatarW, avatarH) = texLoader.GetPng("images/profiles/32.png");
    }

    if (avatar)
    {
        CenteredCursor(avatarSize);
        ImVec2 aMin = ImGui::GetCursorScreenPos();
        ImVec2 aMax = ImVec2(aMin.x + avatarSize, aMin.y + avatarSize);
        ImVec2 uv0, uv1;

        CoverUV(avatarW, avatarH, avatarSize, avatarSize, uv0, uv1);
        ImGui::GetWindowDrawList()->AddImageRounded(ToTexId(avatar), aMin, aMax, uv0, uv1, IM_COL32_WHITE, avatarSize * 0.5f);
        ImGui::Dummy(ImVec2(avatarSize, avatarSize));
    }

    CenteredText(profileName);
    ImGui::Dummy(iScale.Vec2(0, 16));

    // Navigation
    NavItem("Profile", "images/profile.png", PageType::Profile);
    NavItem("Homes", "images/home.png", PageType::Worlds);
    NavItem("Screen Sources", "images/desktop.png", PageType::ScreenSources);
    NavItem("Apps Library", "images/library.png", PageType::AppsLibrary);

    // Pinned bottom of nav: Launch Home (blue) and Set Executable
    float launchHeight = iScale.F(44);
    float setExecHeight = iScale.F(30);
    float gap = iScale.F(5);
    float bottomPad = iScale.F(34);
    float launchTop = fullHeight - bottomPad - setExecHeight - gap - launchHeight;
    int btnWidth = availWidth - iScale.F(40);

    ImGui::SetCursorPos(ImVec2((sidebarWidth - btnWidth) / 2, launchTop));

    pushedStyles = PushLaunchButtonStyle();
    if (ImGui::Button("Launch Home", ImVec2(btnWidth, launchHeight)))
    {
        DoLaunchHome();
    }
    ImGui::PopStyleColor(pushedStyles);

    ImGui::Dummy(ImVec2(0, gap));

    ImGui::SetCursorPosX((sidebarWidth - btnWidth) / 2);
    pushedStyles = PushButtonStyleGrey();
    if (ImGui::Button("Set Executable", ImVec2(btnWidth, setExecHeight)))
    {
        DoSetExecutable();
    }
    ImGui::PopStyleColor(pushedStyles);

    // Version stamp
    ImGui::SetWindowFontScale(0.7f);
    pushedStyles = PushSubTextStyle();
    CenteredText(appVersion);
    ImGui::PopStyleColor(pushedStyles);
    ImGui::SetWindowFontScale(1.0f);

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void UI::DrawContent()
{
    int windowPaddingX = 28;

    // Fill exactly the region the sidebar left. Never use the design constants so nothing overhangs the real client (which can differ by a few px from WindowWidth/Height)
    ImVec2 region = ImGui::GetContentRegionAvail();
    float contentWidth = region.x;
    float fullHeight = region.y;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 p0 = origin;
    ImVec2 p1 = ImVec2(origin.x + contentWidth, origin.y + fullHeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Content-pane background = selected default world's screenshot the world image is cropped into the band between them
    float topBand = iScale.F(LetterboxTopBand);
    float bottomBand = iScale.F(LetterboxBottomBand);
    ImVec2 imgP0 = ImVec2(p0.x, p0.y + topBand);
    ImVec2 imgP1 = ImVec2(p1.x, p1.y - bottomBand);

    // Dark letterbox fill behind everything
    drawList->AddRectFilled(p0, p1, ImGui::GetColorU32(UIConsts.LetterboxFill));

    // fall back to the bundled world-default.png if a world screenshot is missing.
    GLuint background = 0;
    int backgroundW = 0, backgroundH = 0;
    for (const worlds::WorldCardInfo& w : worldList)
    {
        if (w.isDefault && !w.screenshotPng.empty())
        {
            texLoader.RequestAsync(w.screenshotPng); // async decode, 0 until uploaded
            std::tie(background, backgroundW, backgroundH) = texLoader.GetPng(w.screenshotPng);
            break;
        }
    }

    if (!background)
    {
        std::tie(background, backgroundW, backgroundH) = texLoader.GetPng("images/world-default.png");
    }
        
    if (background)
    {
        ImVec2 uv0, uv1;
        CoverUV(backgroundW, backgroundH, imgP1.x - imgP0.x, imgP1.y - imgP0.y, uv0, uv1);
        drawList->AddImage(ToTexId(background), imgP0, imgP1, uv0, uv1);
    }
    else
    {
        drawList->AddRectFilled(imgP0, imgP1, ImGui::GetColorU32(ImVec4(0.102f, 0.102f, 0.125f, 1)));
    }

    // Gradients in the letterbox color blending the bands into the image: one at the top then another at the bottom.
    // Two adjustment kinds here:
    //   -Opacity: 0..1 peak darkness (1 is fully opaque)
    //   -Height : how far the fade reaches into the image
    float topShadeOpacity = 1.0f;
    float topShadeHeight = 0.45f;
    float bottomShadeOpacity = 1.0f;
    float bottomShadeHeight = 0.26f;

    float bandH = imgP1.y - imgP0.y;
    ImU32 clear = LetterboxShade(0.0f);
    ImU32 gradTop = LetterboxShade(topShadeOpacity);
    ImU32 gradBottom = LetterboxShade(bottomShadeOpacity);
    drawList->AddRectFilledMultiColor(imgP0, ImVec2(imgP1.x, imgP0.y + bandH * topShadeHeight), gradTop, gradTop, clear, clear);
    drawList->AddRectFilledMultiColor(ImVec2(imgP0.x, imgP1.y - bandH * bottomShadeHeight), imgP1, clear, clear, gradBottom, gradBottom);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, iScale.Vec2(windowPaddingX, UIConsts.PageContentPadding));

    float navCursor = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(navCursor + iScale.F(windowPaddingX));

    ImGui::BeginChild("##content", ImVec2(contentWidth - windowPaddingX, fullHeight), false);

    switch (env.currentPage)
    {
    case PageType::Profile:       DoProfile(); break;
    case PageType::Worlds:        DoWorlds(); break;
    case PageType::ScreenSources: DoScreens(); break;
    case PageType::AppsLibrary:   DoApps(); break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void UI::DoLaunchHome()
{
    // Just launch Home executable normally. Not required for injection! Launching from explorer or shortcut can also work
    if (home2ExePath.empty())
    {
        env.noticeMessage = "Set the home's executable first using \"Set Executable\", then retry \"Launch Home\".";
        env.nextPopup = "Notice";
        return;
    }

    std::wstring exeW = prefs::Widen(home2ExePath);
    std::wstring dir = exeW;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        dir = dir.substr(0, slash);
    }

    std::wstring cmd = L"\"" + exeW;// +L"\"" + L" -windowed -HideAllWindows -UNATTENDED";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exeW.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        homeLogger.write() << "Launched Home! Injection happens automatically" << std::endl;
    }
    else
    {
        homeLogger.write() << "Launch failed (error " << GetLastError() << ")." << std::endl;
        env.noticeMessage = "Could not launch the Home2 executable. Check the path in Set Executable.";
        env.nextPopup = "Notice";
    }
}

void UI::DoSetExecutable()
{
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(window);
    ofn.lpstrFilter = L"Executable (Home2-Win64-Shipping.exe)\0Home2-Win64-Shipping.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select Home2-Win64-Shipping.exe (Located at \"Home2\\Binaries\\Win64\")";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn))
    {
        home2ExePath = prefs::Narrow(file);
        prefs::SetHome2ExePath(home2ExePath);
        homeLogger.write() << "Set Home2 executable." << std::endl;
    }
}
#pragma endregion

void UI::DoPopups(Env& env)
{
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImVec2 pivot = ImVec2(0.5f, 0.5f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, iScale.F(5));

    const float wrapWidth = iScale.F(490.0f);

    ImGui::SetNextWindowPos(center, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(ImVec2(wrapWidth + iScale.F(30.0f), 0.0f), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Notice", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        const float padding = iScale.F(10);
        const float wrapWidth = iScale.F(500);

        ImVec2 textSize = ImGui::CalcTextSize(env.noticeMessage.c_str(), nullptr, false, wrapWidth);

        // Top padding
        ImGui::Dummy(ImVec2(0, padding));

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padding);

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextWrapped(env.noticeMessage.c_str());
        ImGui::PopTextWrapPos();

        // Bottom padding
        ImGui::Dummy(ImVec2(0, padding));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetItemDefaultFocus();

        CenteredCursor(iScale.F(100));
        pushedStyles = PushButtonStyleGrey();

        if (ImGui::Button("Ok", iScale.Vec2(100, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::PopStyleColor(pushedStyles);
        ImGui::EndPopup();
    }

    // Fetch My Homes: download the user's remote worlds (homes) from graph.oculus.com into store\worlds
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(iScale.Vec2(780, 620), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Fetch My Homes", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::Dummy(iScale.Vec2(0, 10));
        float headerTop = ImGui::GetCursorPosY();

        // X close (top-right)
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowSize().x - iScale.F(38), headerTop));
        pushedStyles = PushButtonStyleGrey();
        if (ImGui::Button("X", iScale.Vec2(26, 26)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(pushedStyles);

        // Centered title on the same header row
        ImGui::SetCursorPos(ImVec2(0.0f, headerTop));
        ImGui::PushFont(fontHeader);
        CenteredText("Fetch My Homes");
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Poll the background worker for completion (runs every frame while the popup is open)
        // Close is disabled mid-fetch so the modal stays up until retrieves the result
        if (fetchRunning && fetchFuture.valid() && fetchFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            fetchworlds::Result r = fetchFuture.get();
            fetchRunning = false;
            fetchResultOk = r.ok && r.worldsSaved > 0;
            if (fetchResultOk)
            {
                fetchResultMsg = "Downloaded " + std::to_string(r.worldsSaved) + " home(s).";
                reloadWorldsOnOpen = true; // refresh the Worlds list to include them
            }
            else
            {
                fetchResultMsg = r.error.empty() ? std::string("No homes were downloaded.") : r.error;
            }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, iScale.Vec2(10, 10));

        ImGui::BeginChild("AccountFields", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        {
            ImGui::TextWrapped("Download your homes from the Oculus backend while they still exist. Any found homes are saved locally and can load offline afterward. A couple details about your Meta (Oculus) account are required.");
            ImGui::Spacing();

            ImGui::PushTextWrapPos(iScale.F(780));
            ImGui::TextColored(UIConsts.SubText, "1. Open your Meta Horizon Link app."
                "\n2. Press \"CTRL + SHIFT + I\" to show dev tools."
                "\n3. Go under \"Network\" tab."
                "\n4. In the link app, click on your profile page."
                "\n5. In the captured network list, look for requests titled with \"graphql\"."
                "\n6. Observe these requests and look for the fields \"access_token\" and \"userId\" in the \"Payload\" tab of each request."
                "\n7. Copy these values and paste into the respective fields below and click \"Submit\".");
            ImGui::Spacing();
            ImGui::TextColored(UIConsts.SubText, "Any fetched homes that contain UGC content will download the assets automatically into your world folder.");
            ImGui::TextColored(UIConsts.SubText, "Do not share your FRL token with anyone!");
            ImGui::PopTextWrapPos();

            ImGui::Spacing();

            ImGui::BeginDisabled(fetchRunning);

            ImGui::TextUnformatted("FRL Token");
            ImGui::SetNextItemWidth(iScale.F(430));
            pushedStyles = PushTextInputStyle();
            ImGui::InputText("##fetchToken", &fetchToken, ImGuiInputTextFlags_Password);
            ImGui::PopStyleColor(pushedStyles);

            ImGui::TextUnformatted("User ID");
            ImGui::SetNextItemWidth(iScale.F(430));
            pushedStyles = PushTextInputStyle();
            ImGui::InputText("##fetchUserId", &fetchUserId);
            ImGui::PopStyleColor(pushedStyles);

            ImGui::EndDisabled();

            ImGui::Spacing();

            if (fetchRunning)
            {
                int done = fetchProgress.done.load();
                int total = fetchProgress.total.load();
                if (total > 0)
                    ImGui::Text("Fetching homes...  %d / %d", done, total);
                else
                    ImGui::TextUnformatted("Contacting the backend...");
            }
            else if (!fetchResultMsg.empty())
            {
                ImGui::PushTextWrapPos(iScale.F(780));
                ImGui::TextColored(fetchResultOk ? ImVec4(0.42f, 0.85f, 0.42f, 1.0f) : UIConsts.ErrorText, "%s", fetchResultMsg.c_str());
                ImGui::PopTextWrapPos();
            }

            float windowHeight = ImGui::GetWindowSize().y;
            float footerOffset = windowHeight - iScale.F(58);

            ImGui::SetCursorPosY(footerOffset);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::BeginDisabled(fetchRunning || fetchToken.empty() || fetchUserId.empty());
            pushedStyles = PushButtonStyleGrey();
            if (CenteredButton("Submit", iScale.Vec2(140, 34)))
            {
                fetchProgress.total.store(0);
                fetchProgress.done.store(0);
                fetchResultMsg.clear();
                fetchResultOk = false;
                fetchRunning = true;
                // Copy token/userId into the task (no cross-thread read of the UI strings). The worker touches only fetchProgress (atomics) and "this"
                fetchFuture = std::async(std::launch::async,
                    [token = fetchToken, userId = fetchUserId, this]()
                    { return fetchworlds::FetchMyWorlds(token, userId, &fetchProgress); });
            }

            ImGui::PopStyleColor(pushedStyles);
            ImGui::EndDisabled();
        }
        ImGui::EndChild();

        ImGui::PopStyleVar();

        ImGui::SameLine();
        ImGui::EndPopup();
    }

    // Profile-picture selector
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(iScale.Vec2(780, 620), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Choose Your Profile Picture", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::Dummy(iScale.Vec2(0, 10));
        float headerTop = ImGui::GetCursorPosY();

        // X close (top-right)
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowSize().x - iScale.F(38), headerTop));
        pushedStyles = PushButtonStyleGrey();
        if (ImGui::Button("X", iScale.Vec2(26, 26)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(pushedStyles);

        // Centered title on the same header row
        ImGui::SetCursorPos(ImVec2(0.0f, headerTop));
        ImGui::PushFont(fontHeader);
        CenteredText("Choose Your Profile Picture");
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Scrollable 5-column circular preset grid. Cell 0 = silhouette (none / default),
        // cells 1..32 = images/profiles/N.png.
        float footerH = iScale.F(60);
        ImVec2 gridSize = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - footerH);
        ImGui::BeginChild("##presetGrid", gridSize, false);
        {
            const int cols = 5;
            float cellPad = iScale.F(2);
            float gridW = ImGui::GetContentRegionAvail().x;
            float cell = (gridW - cellPad * (cols - 1)) / cols;

            // Avatar size within its grid slot (1.0 = fills the slot). Lower it to shrink the pictures. The slot stays `cell` so the 5 columns stay evenly spaced and the circle is re-centered in its slot via dotInset.
            float pictureScale = 0.8f;
            float dotSize = cell * pictureScale;
            float dotInset = (cell - dotSize) * 0.5f;
            float radius = dotSize * 0.5f;

            // Row gap comes from ItemSpacing.y, column gap is the SameLine below, so the two axes tune independently.
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, cellPad));
            for (int p = 1; p <= 32; ++p)
            {
                if ((p - 1) % cols != 0)
                {
                    ImGui::SameLine(0.0f, cellPad);
                }

                ImVec2 cpos = ImGui::GetCursorScreenPos();
                std::string id = "##preset" + std::to_string(p);
                bool clicked = ImGui::InvisibleButton(id.c_str(), ImVec2(cell, cell));
                bool hovered = ImGui::IsItemHovered();

                std::string path = "images/profiles/" + std::to_string(p) + ".png";
                auto [tex, texW, texH] = texLoader.GetPng(path);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 dotMin = ImVec2(cpos.x + dotInset, cpos.y + dotInset);
                ImVec2 dotMax = ImVec2(dotMin.x + dotSize, dotMin.y + dotSize);
                ImVec2 dotCenter = ImVec2(dotMin.x + radius, dotMin.y + radius);
                if (tex)
                {
                    ImVec2 uv0, uv1;
                    CoverUV(texW, texH, dotSize, dotSize, uv0, uv1);
                    dl->AddImageRounded(ToTexId(tex), dotMin, dotMax, uv0, uv1, IM_COL32_WHITE, radius);
                }
                else
                {
                    dl->AddCircleFilled(dotCenter, radius, IM_COL32(40, 40, 48, 255));
                }

                bool sel = env.selectedProfilePreset == p;
                ImU32 ring = sel ? IM_COL32(90, 140, 255, 255) : (hovered ? IM_COL32(255, 255, 255, 150) : IM_COL32(255, 255, 255, 55));
                dl->AddCircle(dotCenter, radius - iScale.F(1), ring, 0, iScale.F(sel ? 3.0f : 1.5f));

                if (clicked)
                {
                    env.selectedProfilePath = L"";
                    env.selectedProfilePreset = p;
                }
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Spacing();

        float btnW = iScale.F(150);
        float windowWidth = ImGui::GetWindowSize().x;
        float btnOffsetX = (windowWidth - (btnW * 2)) * 0.5f;

        ImGui::SetCursorPosX(btnOffsetX);

        // Popup footer: Browse Image (left, grey) and Confirm (right, blue, disabled until a valid pick).
        pushedStyles = PushButtonStyleGrey();
        if (ImGui::Button("Browse Image", ImVec2(btnW, iScale.F(34))))
        {
            wchar_t file[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = glfwGetWin32Window(window);
            ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Select a profile image (PNG)";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

            if (GetOpenFileNameW(&ofn))
            {
                env.selectedProfilePreset = -1;
                env.selectedProfilePath = file;
                homeLogger.write() << "Selected profile icon: " << file << std::endl;
            }
        }
        ImGui::PopStyleColor(pushedStyles);

        ImGui::SameLine();

        bool hasSelection = env.selectedProfilePreset != -1 || !env.selectedProfilePath.empty();
        if (!hasSelection)
        {
            ImGui::BeginDisabled();
        }

        pushedStyles = PushLaunchButtonStyle();
        if (ImGui::Button("Confirm", ImVec2(btnW, iScale.F(34))))
        {
            int preset = env.selectedProfilePreset;
            if (hasSelection)
            {
                std::wstring src = !env.selectedProfilePath.empty() ? env.selectedProfilePath : prefs::Widen("images/profiles/" + std::to_string(preset) + ".png");
                std::wstring dst = prefs::AppDir() + L"profile.png";
                if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
                {
                    profileImagePath = prefs::Narrow(dst);
                    prefs::SetProfileImagePath(profileImagePath);
                    LoadProfileImage();
                    homeLogger.write() << "Profile picture set. Writing to .pak file.." << std::endl;
                    ApplyProfilePak();
                }
                else
                {
                    homeLogger.write() << "Profile copy failed (error " << GetLastError() << ")." << std::endl;
                }
            }

            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(pushedStyles);
        if (!hasSelection)
        {
            ImGui::EndDisabled();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(1);
}

void UI::Run()
{
    // Set new window scale here to avoid startup font rebuild
    env.lastWinScale = UpdateScale();
    homeLogger.write() << "New window scale: " << env.lastWinScale.x << " x " << env.lastWinScale.y << std::endl;

    // Interface loop
    while (ui.keepAlive.load())
    {
        glfwPollEvents(); // Required for processing input

        HWND hwnd = glfwGetWin32Window(window);
        bool windowIsMinimized = IsIconic(hwnd);

        // Outside process may have foregrounded the window
        if (windowClosed && IsWindowVisible(hwnd))
        {
            windowClosed = false;
        }

        ui.inactive.store(windowClosed || windowIsMinimized);

        // Skip UI updates when the app is closed to the system tray or minimized
        if (ui.inactive.load())
        {
            Sleep(80);
            continue;
        }

        // Flag to throttle the UI framerate if the cursor is not hovering over any elements
        bool windowIsActive = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_AllowWhenBlockedByPopup) || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        
        // Rebuild fonts from scale changes
        if (env.doBuildFonts)
        {
            LoadFonts();

            // Reset style scaling
            ImGuiStyle& style = ImGui::GetStyle();
            style = ImGuiStyle();
            ImGui::StyleColorsDark();
            style.ScaleAllSizes(iScale.scale);

            env.doBuildFonts = false;

            int cX, cY;
            glfwGetWindowPos(window, &cX, &cY);
            glfwSetWindowMonitor(window, NULL, cX, cY, UIConsts.WindowWidth * iScale.scale, UIConsts.WindowHeight * iScale.scale, GLFW_DONT_CARE);
        }

        // GUI window.
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();

        ImGui::NewFrame();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##nothing", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, iScale.F(6));
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, iScale.F(6));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, iScale.F(6));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, iScale.F(6));        

        glfwSwapInterval(windowIsActive ? 1 : 4);

        // Check window scaling every 3 seconds
        uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (now - env.lastDPIUpdate >= 3)
        {
            ImVec2 windowScale = UpdateScale();

            if (windowScale.x + windowScale.y != env.lastWinScale.x + env.lastWinScale.y)
            {
                env.lastWinScale = windowScale;
                homeLogger.write() << "Updated window scale: " << windowScale.x << " x " << windowScale.y << std::endl;

                // Queue font rebuild
                env.doBuildFonts = true;
            }

            env.lastDPIUpdate = now;
        }
       
        // Any calls to show a popup
        if (!env.nextPopup.empty())
        {
            ImGui::OpenPopup(env.nextPopup.c_str());
            env.nextPopup = "";
        }

        // Upload any world screenshots that finished decoding on the worker thread. It must run on the render/GL thread, covering every page's use of async thumbnails/backgrounds.
        texLoader.PumpUploads();

        // "Sidebar-navigation shell" as a persistent sidebar and paged content pane.
        DrawSidebar();
        ImGui::SameLine(0.0f, 0.0f);
        DrawContent();

        // Render any new popups
        DoPopups(env);

        ImGui::PopStyleVar(4);
        ImGui::End();
        ImGui::PopStyleVar(1);

        // Finish this frame
        ImGui::Render();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        Sleep(windowIsActive ? 1 : 100);
    }

    ui.keepAlive.store(false);
}

void UI::Shutdown()
{
    broadcast::Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}