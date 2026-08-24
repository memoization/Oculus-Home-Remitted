#include <windows.h>
#include <winnls.h>
#include <shobjidl.h>
#include <objbase.h>
#include <Shlwapi.h>
#include <objidl.h>
#include <shlguid.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <thread>
#include "HomeLogger.h"
#include "UI.h"
#include "Watcher.h"
#include "Prefs.h"
#include "Tray.h"

// APP_VERSION comes from src\version.gen.h generated before each build by the GenerateVersionHeader target in the vcxproj, which runs "git describe" for the remote repo tag
#if __has_include("version.gen.h")
    #include "version.gen.h"
#endif


int main(int argc, char* argv[])
{
    // Find the current executable path
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);

    if (len != 0)
    {
#ifndef _DEBUG
        // Force working directory to the executable
        char exeDir[MAX_PATH];
        strcpy_s(exeDir, exePath);

        if (PathRemoveFileSpecA(exeDir))
        {
            SetCurrentDirectoryA(exeDir);
        }
#endif
    }

    // Enforce single process only
    LPCWSTR namedMutex = L"home2_remitted_impl";
    HANDLE programMutex = CreateMutex(NULL, TRUE, namedMutex);
    if (ERROR_ALREADY_EXISTS == GetLastError())
    {
        // Restore window minimized / background
        if (!ui.BringForeground(true))
        {
            ui.Create();
            MessageBox(glfwGetWin32Window(ui.window), L"This app is already running!", L"Error", MB_OK);
        }
        
        return 0;
    }

    homeLogger.open("remitted.log");
    homeLogger.flush();

    // Create the app window
    ui.Create();

#ifndef _DEBUG
    #ifdef APP_VERSION
    ui.appVersion = APP_VERSION; // from src\version.gen.h (latest git tag), generated at build time
    #endif
#endif

    // Start the injector: it watches for Home2-Win64-Shipping.exe and injects home2backend.dll, re-arms on home process exit
    {
        std::wstring dllPath = prefs::AppDir() + L"home2backend.dll";
        if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            homeLogger.write() << "WARNING: home2backend.dll not found beside the frontend ("
                               << dllPath.c_str()
                               << "), offline injection will fail until it is placed here." << std::endl;
        }
        g_homeWatcher.Start(dllPath);
    }

    // Sys tray presence so closing the window keeps the frontend in background
    tray::Install(glfwGetWin32Window(ui.window));

    ui.keepAlive.store(true);
    ui.Run(); // Blocks the main() thread until execution stops.

    tray::Remove();
    g_homeWatcher.Stop();
    ui.Shutdown(); // Cleanup after closure.

    CoUninitialize();

    ReleaseMutex(programMutex);
    CloseHandle(programMutex);

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}