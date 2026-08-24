#include "Watcher.h"
#include "Injector.h"
#include "HomeLogger.h"
#include "Worlds.h"

HomeWatcher g_homeWatcher;

static const wchar_t* kHomeProcess = L"Home2-Win64-Shipping.exe";
static const DWORD kPollMs = 100;              // well inside the ~7s world_login window
static const int kMaxAttemptsPerInstance = 80; // ~8s of retries before giving up on an instance

void HomeWatcher::Start(const std::wstring& dllPath)
{
    if (running_.load())
    {
        return;
    }

    dllPath_ = dllPath;
    running_.store(true);
    thread_ = std::thread(&HomeWatcher::Loop, this);
    homeLogger.write() << "Watcher armed; watching for Home2-Win64-Shipping.exe ..." << std::endl;
}

void HomeWatcher::Stop()
{
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
}

void HomeWatcher::Loop()
{
    DWORD attemptPid = 0;
    int attempts = 0;
    bool warned = false;

    while (running_.load())
    {
        DWORD pid = injector::FindProcessId(kHomeProcess);

        if (pid == 0)
        {
            // Home is gone, re-arm for the next launch.
            if (homeRunning_.load() || injectedPid_.load() != 0)
            {
                homeRunning_.store(false);
                injectedPid_.store(0);
                attemptPid = 0;
                attempts = 0;
                warned = false;
                homeLogger.write() << "Home closed; re-armed..." << std::endl;
            }
        }
        else
        {
            if (injectedPid_.load() != pid)
            {
                // A new (not-yet-injected) instance
                if (pid != attemptPid)
                {
                    attemptPid = pid;
                    attempts = 0;
                    warned = false;
                }

                if (attempts < kMaxAttemptsPerInstance)
                {
                    ++attempts;
                    if (injector::InjectDll(pid, dllPath_))
                    {
                        injectedPid_.store(pid);
                        homeLogger.write() << "Injected home2backend.dll into Home (pid " << pid << ")." << std::endl;
                    }
                }
                else if (!warned) //too early / transient! Now retry on the next poll
                {
                    warned = true;
                    homeLogger.write() << "WARNING: could not inject into Home (pid " << pid << ") after " << kMaxAttemptsPerInstance << " attempts." << std::endl;
                }
            }

            if (!homeRunning_.exchange(true))
            {
                // Freshly-launched Home: seed WorldsCache with the downloaded UGC assets before the game loads a world and looks for them.
                worlds::PopulateUgcCache();
            }
        }

        Sleep(kPollMs);
    }
}
