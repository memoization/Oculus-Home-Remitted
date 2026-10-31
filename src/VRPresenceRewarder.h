#pragma once
#include <atomic>
#include <thread>

// Earns reward loot boxes from VR app playtime while reward mode is on.
// A per second tally ticks up only while a VR app other than Oculus Home is running
class VRPresenceRewarder
{
public:
    void Start();
    void Stop();

private:
    void Loop();
    void LoadState(); // read the persisted tally, counts, and day and week stamps
    void PersistState() const; // write the tally, counts, and stamps back to the preferences
    bool GrantBox(); // grant one box when under the daily and weekly caps

    std::thread thread_;
    std::atomic<bool> running_{ false };
	
	const int kRewardThresholdS = 1800; // 30 minutes of VR presence earns one box
    const int kMaxBoxesPerDay = 2;
    const int kMaxBoxesPerWeek = 8;
    const int kPersistEverySec = 30;// update the tally to prefs this often
    const int kModeRecheckSec = 5; // how often the live rewardMode toggle is checked

    // persisted state
    int tally_ = 0; // seconds accumulated toward the next box
    int boxesDay_ = 0; // boxes granted during the current day
    int boxesWeek_ = 0;// boxes granted during the current week
    long long dayStamp_ = 0; // the current day as days since epoch
    long long weekStamp_ = 0; // the current week as weeks since epoch
};

extern VRPresenceRewarder g_vrPresenceRewarder;
