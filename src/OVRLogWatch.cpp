#include "OVRLogWatch.h"
#include <filesystem>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include "HomeLogger.h"
#include "Prefs.h"

namespace fs = std::filesystem;

OVRLogWatch g_ovrLogWatch;

namespace
{
    const DWORD kPollMs = 1000; // cadence parsing of the log tail
    const DWORD kRescanMs = 5000; // how often to look for a newer service log, which appears when the server runtime restarts

    // The run of digits immediately after marker, or "" if the marker is absent.
    std::string DigitsAfter(const std::string& s, const char* marker)
    {
        size_t p = s.find(marker);
        if (p == std::string::npos) return {};

        p += std::strlen(marker);
        size_t start = p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        return (p > start) ? s.substr(start, p - start) : std::string();
    }

    std::string LowerAscii(std::string s)
    {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool EqualsNoCase(const std::string& a, const std::string& b)
    {
        return LowerAscii(a) == LowerAscii(b);
    }

    // The value after "file: " to the end of the line. The connection line ends with the client exe name.
    std::string FileNameAfter(const std::string& line)
    {
        size_t p = line.find("file: ");
        if (p == std::string::npos) return {};
        p += 6; // past "file: "
        size_t end = line.size();
        while (end > p && (line[end - 1] == ' ' || line[end - 1] == '\r' || line[end - 1] == '\t')) --end;
        return line.substr(p, end - p);
    }

    // Runtime and bridge exes that open their own service connection but are not "a VR app the user launched".
    // vrserver is the SteamVR bridge, which the OpenVR path already covers. OculusDash is the void itself. The others are the runtime and frontend.
    bool IsRuntimeExe(const std::string& exe)
    {
        std::string e = LowerAscii(exe);
        return e == LowerAscii(kHomeProcess)
            || e == "oculusdash.exe"
            || e == "ovrserver_x64.exe"
            || e == "home2remitted.exe";
    }

    // A parsed decimal pid, or 0 if the text is not a usable pid.
    DWORD ParsePid(const std::string& digits)
    {
        if (digits.empty()) return 0;
        unsigned long v = std::strtoul(digits.c_str(), nullptr, 10);
        return (v == 0 || v > 0xFFFFFFFFul) ? 0 : (DWORD)v;
    }

    bool ProcessAlive(HANDLE process)
    {
        // The handle is bound to the exact launched instance so a signaled wait means that instance has exited even if the pid was later reused.
        return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    // The exe base name behind this handle, for readable logs. "" if it cannot be read.
    std::string ProcessImageBaseName(HANDLE process)
    {
        char path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (!QueryFullProcessImageNameA(process, 0, path, &size)) return {};

        std::string full(path, size);
        size_t slash = full.find_last_of("\\/");
        return (slash == std::string::npos) ? full : full.substr(slash + 1);
    }

    // A comparable local wall clock value from the line's leading "DD/MM HH:MM:SS.mmm" stamp, combined with the log file's year. 0 if it cannot be parsed.
    unsigned long long LineTimeFileTime(const std::string& line, unsigned short year)
    {
        int d = 0, mo = 0, h = 0, mi = 0, s = 0, ms = 0;
        if (year == 0 || sscanf_s(line.c_str(), "%d/%d %d:%d:%d.%d", &d, &mo, &h, &mi, &s, &ms) != 6)
        {
            return 0;
        }

        SYSTEMTIME st = {};
        st.wYear = year;
        st.wMonth = (WORD)mo;
        st.wDay = (WORD)d;
        st.wHour = (WORD)h;
        st.wMinute = (WORD)mi;
        st.wSecond = (WORD)s;
        st.wMilliseconds = (WORD)ms;

        FILETIME ft;
        if (!SystemTimeToFileTime(&st, &ft)) return 0; // treated as a plain wall clock value, matched against a wall clock creation time below
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart;
    }

    // The process creation time as the same local wall clock value LineTimeFileTime produces. 0 on failure.
    unsigned long long ProcessCreationWallClock(HANDLE process)
    {
        FILETIME creation, exit, kernel, user;
        if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return 0;

        FILETIME local;
        if (!FileTimeToLocalFileTime(&creation, &local)) return 0; // creation is UTC, bring it to local to match the log's local stamps
        ULARGE_INTEGER u{};
        u.LowPart = local.dwLowDateTime;
        u.HighPart = local.dwHighDateTime;
        return u.QuadPart;
    }
}

void OVRLogWatch::Start()
{
    if (running_.load())
    {
        return;
    }

    running_.store(true);
    thread_ = std::thread(&OVRLogWatch::Loop, this);
    homeLogger.write() << "OVRLogWatch: watching the Oculus service log for app lifecycle." << std::endl;
}

void OVRLogWatch::Stop()
{
    running_.store(false);
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void OVRLogWatch::Track(DWORD pid, const std::string& expectedExe, unsigned long long lineTimeFt)
{
    if (apps_.find(pid) != apps_.end())
    {
        return; // already tracking this instance
    }

    // Bind to the process now, at the launch edge, so liveness is immune to the pid being reused later.
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
    {
        // The process is already gone, or cannot be opened, so there is nothing to track. This is the common case when replaying old connection lines for apps that have long since exited.
        return;
    }

    // Confirm the live process really is the exe the log named. When replaying the session history a pid has very often been recycled onto something unrelated, so a mismatch or an unreadable image means this is not the app the line referred to.
    std::string image = ProcessImageBaseName(h);
    if (image.empty() || !EqualsNoCase(image, expectedExe))
    {
        CloseHandle(h);
        return;
    }

    // Second guard against pid reuse. The instance can only be the one the line named if it was created at or before the line's time, since no two live processes share a pid.
    unsigned long long creation = ProcessCreationWallClock(h);
    if (lineTimeFt != 0 && creation != 0)
    {
        const unsigned long long kSlack = 5ull * 10000000ull; // 5 seconds in 100ns units, absorbs any ordering jitter between launch and the connection line
        if (creation > lineTimeFt + kSlack)
        {
            CloseHandle(h);
            return;
        }
    }

    apps_.emplace(pid, TrackedApp{ image, h });
    homeLogger.write() << "OVRLogWatch: track " << image.c_str() << " pid " << pid << " (tracked: " << apps_.size() << ")." << std::endl;
}

void OVRLogWatch::UntrackAll()
{
    for (auto& kv : apps_)
    {
        if (kv.second.process) CloseHandle(kv.second.process);
    }
    apps_.clear();
}

std::wstring OVRLogWatch::FindLatestLog() const
{
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return {};
    }

    fs::path dir = fs::path(local) / L"Oculus";
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
    {
        return {};
    }

    // The active session's log is the Service_*.txt being written to, so pick the newest by write time.
    std::wstring best;
    fs::file_time_type bestTime{};
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;

        const std::wstring name = it->path().filename().wstring();
        if (name.rfind(L"Service_", 0) != 0) continue; // Service_YYYY-MM-DD_hh.mm.ss.txt
        if (it->path().extension() != L".txt") continue;

        fs::file_time_type t = fs::last_write_time(it->path(), ec);
        if (ec) continue;

        if (best.empty() || t > bestTime)
        {
            bestTime = t;
            best = it->path().wstring();
        }
    }
    return best;
}

bool OVRLogWatch::OpenLog(const std::wstring& path)
{
    CloseLog();

    // Share write and delete so we never block the service or its log rotation.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    file_ = h;
    logPath_ = path;
    partial_.clear();

    // The year is only in the file name (Service_YYYY-MM-DD_...), so pull it out here for the launch line time parsing. Fall back to the current year if the name does not match.
    logYear_ = 0;
    {
        const std::wstring stem = fs::path(path).filename().wstring();
        size_t us = stem.find(L'_');
        if (us != std::wstring::npos && us + 5 <= stem.size())
        {
            logYear_ = (unsigned short)_wtoi(stem.substr(us + 1, 4).c_str());
        }
        if (logYear_ == 0)
        {
            SYSTEMTIME now;
            GetLocalTime(&now);
            logYear_ = now.wYear;
        }
    }

    UntrackAll();
    appRunning_.store(false);

    // Replay the whole current session from the start so the running-app state is correct immediately, then keep tailing. Launch lines for apps that already exited resolve to nothing because their process can no longer be opened.
    DrainNewLines();
    PruneDeadApps();
    UpdateRunningState();
    homeLogger.write() << "OVRLogWatch: tailing " << fs::path(path).filename().string().c_str() << " (apps running: " << apps_.size() << ")." << std::endl;

    ready_.store(true);
    return true;
}

void OVRLogWatch::CloseLog()
{
    if (file_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    logPath_.clear();
    ready_.store(false);
}

void OVRLogWatch::DrainNewLines()
{
    if (file_ == INVALID_HANDLE_VALUE)
    {
        return;
    }

    // Once the handle sits at EOF a later ReadFile returns the bytes appended since, so keeping it open tails the log.
    char buf[64 * 1024];
    DWORD got = 0;
    for (;;)
    {
        if (!ReadFile(file_, buf, sizeof(buf), &got, nullptr) || got == 0)
        {
            break;
        }

        partial_.append(buf, got);
        size_t nl;
        while ((nl = partial_.find('\n')) != std::string::npos)
        {
            std::string line = partial_.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            ConsumeLine(line);
            partial_.erase(0, nl + 1);
        }

        if (got < sizeof(buf)) break; // everything currently available has been read
    }
}

void OVRLogWatch::ConsumeLine(const std::string& line)
{
    // A VR app connecting to the runtime service, which carries a OS pid and the client exe name: "Connection open for pid: <pid>, file: <exe>".
    static const char* kConn = "Connection open for pid: ";
    if (line.find(kConn) != std::string::npos)
    {
        DWORD pid = ParsePid(DigitsAfter(line, kConn));
        if (pid == 0) return;

        std::string exe = FileNameAfter(line);
        // Skip the SteamVR bridge, the dash, the runtime and our own frontend. Everything else that opens a service connection is a VR app.
        if (exe.empty() || IsRuntimeExe(exe)) return;
        Track(pid, exe, LineTimeFileTime(line, logYear_));
        return;
    }
}

void OVRLogWatch::PruneDeadApps()
{
    for (auto it = apps_.begin(); it != apps_.end(); )
    {
        if (!ProcessAlive(it->second.process))
        {
            if (it->second.process) CloseHandle(it->second.process);
            std::string image = it->second.image;
            DWORD pid = it->first;
            it = apps_.erase(it);
            homeLogger.write() << "OVRLogWatch: exit " << (image.empty() ? "app" : image.c_str()) << " pid " << pid << " (tracked: " << apps_.size() << ")." << std::endl;
        }
        else
        {
            ++it;
        }
    }
}

void OVRLogWatch::UpdateRunningState()
{
    bool prev = appRunning_.load();
    bool now = !apps_.empty();
    appRunning_.store(now);
    if (now != prev)
    {
        homeLogger.write() << "OVRLogWatch: Oculus app " << (now ? "running" : "none") << " (tracked: " << apps_.size() << ")." << std::endl;
    }
}

void OVRLogWatch::Loop()
{
    while (running_.load())
    {
        unsigned long long now = GetTickCount64();

        // Attach to the newest service log, and check periodically so a runtime restart into a fresh log is picked up.
        if (file_ == INVALID_HANDLE_VALUE || now - lastScanTick_ >= kRescanMs)
        {
            lastScanTick_ = now;
            std::wstring latest = FindLatestLog();
            if (!latest.empty() && latest != logPath_)
            {
                OpenLog(latest); // resets state and replays the new file
            }
        }

        if (file_ != INVALID_HANDLE_VALUE)
        {
            DrainNewLines(); // add in any newly launched apps and the accelerator exit lines
        }
        PruneDeadApps(); // remove any tracked app whose process has since exited, even without a log line
        UpdateRunningState();

        Sleep(kPollMs);
    }

    UntrackAll();
    CloseLog();
}
