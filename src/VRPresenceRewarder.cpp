#include "VRPresenceRewarder.h"

#include "Prefs.h"
#include "LaunchHandler.h"
#include "Watcher.h"

#include <chrono>
#include <ctime>
#include "HomeLogger.h"

VRPresenceRewarder g_vrPresenceRewarder;

long long DaysSinceEpoch()
{
	return (long long)(time(nullptr) / 86400);
}

long long WeeksSinceEpoch()
{
	return DaysSinceEpoch() / 7;
}

int ReadRootInt(const char* field)
{
	return (int)prefs.GetPrefFloat(field, "rewardState", 0.0f);
}

void VRPresenceRewarder::Start()
{
    if (running_.exchange(true))
    {
        return;
    }

    LoadState();
    thread_ = std::thread(&VRPresenceRewarder::Loop, this);
}

void VRPresenceRewarder::Stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    if (thread_.joinable())
    {
        thread_.join();
    }

    PersistState();
}

void VRPresenceRewarder::LoadState()
{
    tally_ = ReadRootInt("VRPresenceTally");
    boxesDay_ = ReadRootInt("rewardedBoxesThisDay");
    boxesWeek_ = ReadRootInt("rewardedBoxesThisWeek");
    dayStamp_ = ReadRootInt("rewardDayStamp");
    weekStamp_ = ReadRootInt("rewardWeekStamp");

    if (tally_ < 0)
    {
        tally_ = 0;
    }
}

void VRPresenceRewarder::PersistState() const
{
    prefs.SetPref<int>("VRPresenceTally", "rewardState", tally_, true);
    prefs.SetPref<int>("rewardedBoxesThisDay", "rewardState", boxesDay_, true);
    prefs.SetPref<int>("rewardedBoxesThisWeek", "rewardState", boxesWeek_, true);
    prefs.SetPref<int>("rewardDayStamp", "rewardState", (int)dayStamp_, true);
    prefs.SetPref<int>("rewardWeekStamp", "rewardState", (int)weekStamp_, true);
}

bool VRPresenceRewarder::GrantBox()
{
    // Roll the day and week counters over when their window has changed.
    long long nowDay = DaysSinceEpoch();
    if (nowDay != dayStamp_)
    {
        boxesDay_ = 0;
        dayStamp_ = nowDay;
        PersistState();
    }

    long long nowWeek = WeeksSinceEpoch();
    if (nowWeek != weekStamp_)
    {
        boxesWeek_ = 0;
        weekStamp_ = nowWeek;
        PersistState();
    }

    if (boxesDay_ < kMaxBoxesPerDay && boxesWeek_ < kMaxBoxesPerWeek)
    {
        int pending = ReadRootInt("pendingRewardBoxes");
        prefs.SetPref<int>("pendingRewardBoxes", "", pending + 1, true);

        ++boxesDay_;
        ++boxesWeek_;
        tally_ = 0;

        PersistState();
        homeLogger.write() << "VRPresenceRewarder: granted an reward box!" << std::endl;
        return true;
    }

    // At the daily or weekly cap
    return false;
}

void VRPresenceRewarder::Loop()
{
    bool rewardMode = false;
    int modeAge = kModeRecheckSec; // force a read on the first run
    int sincePersist = 0;

    while (running_.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_.load())
        {
            break;
        }

        if (++modeAge >= kModeRecheckSec)
        {
            rewardMode = prefs.GetPrefBool("rewardMode", "", false);
            modeAge = 0;
        }

        if (!rewardMode)
        {
            continue;
        }

        // A VR app other than Home counts. Home use does not earn rewards.
        bool vrAppPresent = g_launchHandler.HasFocusedApp() && !g_homeWatcher.HomeRunning();
        if (!vrAppPresent)
        {
            continue;
        }

        // Never let the tally grow past the threshold
        if (tally_ < kRewardThresholdS && (boxesDay_ < kMaxBoxesPerDay || dayStamp_ != DaysSinceEpoch()))
        {
            ++tally_;
        }

        if (tally_ >= kRewardThresholdS)
        {
            // GrantBox grants when under the caps, otherwise it holds.
            // While capped it is retried every second to catch a day or week reset, but the holding state
            // is written to prefs only once to avoid repeated pref writes.
            if (GrantBox())
            {
                sincePersist = 0;
            }
            else if (sincePersist != -1)
            {
                PersistState();
                sincePersist = -1;
            }
        }
        else if (++sincePersist >= kPersistEverySec)
        {
            PersistState();
            sincePersist = 0;
        }
    }
}
