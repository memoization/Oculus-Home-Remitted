#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "MinHook.h"

#pragma comment(lib, "user32.lib") // for EnumDisplayMonitors / GetMonitorInfo

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BroadcastSourceShared.h"
#include "ResponseStore.h"

// OAF reply provider for the OafIpc.dll.
//
// Originally, the game talks to OVRServer over OafIpc, a loopback IPC. This backend fill-in answers those requests itself so Home works with or without OVRServer running.
//
// Without OVRServer, the loopback connect fails and the game never sends, so now OafIpc_Connect, OafIpc_ConnectLoosePerms, OafIpc_IsConnected and OafIpc_GetServerProcessId are forced to report connected. The game then proceeds to send its OAF requests.
//
// Answering:
//   OafIpc_Send              the request carries requestName and sequenceId. For a known route the reply is built now and queued, and the sequenceId is recorded so a late real reply is dropped.
//   OafIpc_GetReply          hands back the queued replies first. The out buffer is a UTF16 JSON block we allocate and free in FreeReplyMessage. A real reply for an already answered or panel sequenceId is swallowed.
//   OafIpc_FreeReplyMessage  frees our substitute buffers and forwards the DLL's own buffers to the original.
//
// Known routes: 
// /features/check is the fetch answered from oaf_gk.tsv
// /library/fetchall is built from the local apps library, and the rest come from store\oaf_replies.tsv one line per route holding the route then a tab then the full reply json.
// panel_embedding is answered on send with its three panel messages.
namespace home2backend {

    typedef void*(*OafSendFn)(const char*);
    typedef int(*OafGetReplyFn)(wchar_t**, int*);
    typedef void(*OafFreeFn)(void*);
    typedef int(*OafConnectFn)(); // OafIpc_Connect / OafIpc_ConnectLoosePerms, returns bool (1 = success)
    typedef int(*OafIsConnectedFn)();// returns bool (1 = connected)
    typedef unsigned(*OafGetPidFn)();// returns the server process id

    static OafSendFn GOrigSend = nullptr;
    static OafGetReplyFn GOrigGetReply = nullptr;
    static OafFreeFn GOrigFree = nullptr;
    static OafConnectFn GOrigConnect = nullptr;
    static OafConnectFn GOrigConnectLoose = nullptr;
    static OafIsConnectedFn GOrigIsConnected = nullptr;
    static OafGetPidFn GOrigGetServerPid = nullptr;

    static std::mutex GMutex;
    static std::unordered_map<std::string, std::string> GReplies;  // route to full reply json
    static std::unordered_map<std::string, std::string> GGkMap; // gatekeeper to "true"/"false"
    static std::deque<std::string> GInjectQueue; // synthetic messages to deliver via GetReply
    static std::unordered_set<std::string> GPanelSeqs; // panel_embedding seqs whose real reply to drop
    static std::unordered_set<std::string> GAnsweredSeqs; // seqs answered on send whose real reply to drop if one ever arrives
    static volatile long GProactiveCount = 0;
    static volatile long GProactiveMiss = 0;
    static volatile long GPanelIdCounter = 0;
    static const char kPanelMonitorId[] = "65537"; // captured monitor handle
    static std::unordered_set<void*> GMine; // the substitute buffers
    static volatile long GRewriteInstalled = 0;
    static volatile long GRepliesLoaded = 0;

    static std::string GetField(const std::string& j, const char* key)
    {
        std::string pat = std::string("\"") + key + "\":\"";
        size_t p = j.find(pat);
        if (p == std::string::npos) return "";

        p += pat.size();
        size_t e = j.find('"', p);
        return (e == std::string::npos) ? std::string() : j.substr(p, e - p);
    }

    static void SetField(std::string& j, const char* key, const std::string& val)
    {
        std::string pat = std::string("\"") + key + "\":\"";
        size_t p = j.find(pat);
        if (p == std::string::npos) return;
        p += pat.size();

        size_t e = j.find('"', p);
        if (e == std::string::npos) return;

        j.replace(p, e - p, val);
    }

    static std::string Utf8FromWide(const wchar_t* w)
    {
        if (!w) return "";

        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return "";

        std::string s(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    // Allocate a null-terminated UTF16 buffer owned (freed in DetourFree via delete[]).
    static wchar_t* WideDupUtf8(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        wchar_t* buf = new wchar_t[static_cast<size_t>(n) + 1];
        if (n > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), buf, n);
        }

        buf[n] = 0;
        return buf;
    }

    // Extract a balanced JSON object value ({...}) for a key, verbatim from the source.
    static std::string ExtractObj(const std::string& j, const char* key)
    {
        std::string pat = std::string("\"") + key + "\":";
        size_t p = j.find(pat);
        if (p == std::string::npos) return "{}";

        size_t b = j.find('{', p);
        if (b == std::string::npos) return "{}";

        int depth = 0;
        for (size_t i = b; i < j.size(); ++i)
        {
            if (j[i] == '{')
                ++depth;
            else if (j[i] == '}' && --depth == 0)
                return j.substr(b, i - b + 1);
        }
        return "{}";
    }

    static std::string NowMsStr()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned long long t = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        t = t / 10000ULL - 11644473600000ULL; // 100ns-since-1601 into ms-since-1970
        return std::to_string(t);
    }

    // Broadcast source selection comes live from the frontend via a session only shared block in shared\BroadcastSourceShared.h.
    // The frontend creates the mapping and writes the user's pick as a raw Windows handle.
    static HANDLE GBcMapping = nullptr;
    static const volatile shared::BroadcastSourceShared* GBcView = nullptr;
    static volatile long GBcOpenTried = 0;

    struct BcSource
    {
        std::string type;
        std::string id;
    };

    // Open the frontend's shared broadcast-source block read-only. An absent mapping is tolerated GetBroadcastSource() then uses the primary-monitor fallback.
    static void OpenBcView()
    {
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, shared::kBroadcastSourceObject);
        if (!h) return;

        void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(shared::BroadcastSourceShared));
        if (!view)
        {
            CloseHandle(h);
            return;
        }
        GBcMapping = h;
        GBcView = reinterpret_cast<const volatile shared::BroadcastSourceShared*>(view);
        LogLine("oaf-rewrite: opened broadcast-source shared block");
    }

    // Seqlock read of the shared block. Returns false, so the caller uses the primary fallback, when the mapping is absent, the version mismatches, the value is unset, or the read kept tearing.
    static bool ReadBcSource(uint32_t& kind, uint64_t& id)
    {
        if (!GBcView && InterlockedExchange(&GBcOpenTried, 1) == 0)
        {
            OpenBcView(); // lazy retry: covers the probe loading before the frontend created the block
        }
    
        if (!GBcView) return false;

        const volatile shared::BroadcastSourceShared* v = GBcView;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            uint32_t g0 = v->generation;
            if (g0 & 1u) continue; // writer mid-write

            MemoryBarrier();
            uint32_t ver = v->version;
            uint32_t k = v->kind;
            uint64_t i = v->id;
            MemoryBarrier();
            uint32_t g1 = v->generation;

            if (g0 != g1) continue; // torn read, retry

            if (ver != shared::kBroadcastSourceVersion || i == 0) return false;

            kind = k;
            id = i;
            return true;
        }
        return false;
    }

    static void EnumMons(std::vector<HMONITOR>& mons, int& prim)
    {
        struct C { std::vector<HMONITOR>* m; int* p; } c{&mons, &prim};
        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL
            {
                C* c = reinterpret_cast<C*>(lp);
                MONITORINFO mi;
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(h, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY))
                    *c->p = static_cast<int>(c->m->size());
                c->m->push_back(h);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&c));
    }

    static void EnumAppWins(std::vector<std::pair<HWND, std::string>>& wins)
    {
        EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL
            {
                auto* v = reinterpret_cast<std::vector<std::pair<HWND, std::string>>*>(lp);
                if (!IsWindowVisible(h)) return TRUE;
                if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

                int len = GetWindowTextLengthW(h);
                if (len <= 0) return TRUE;

                std::wstring t(static_cast<size_t>(len) + 1, 0);
                GetWindowTextW(h, &t[0], len + 1);
                t.resize(static_cast<size_t>(len));
                v->push_back({h, NarrowUtf8(t)});
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&wins));
    }

    static BcSource GetBroadcastSource()
    {
        std::vector<HMONITOR> mons;
        int prim = -1;
        EnumMons(mons, prim);

        static volatile long logged = 0;
        if (InterlockedExchange(&logged, 1) == 0)
        {
            std::vector<std::pair<HWND, std::string>> wins;
            EnumAppWins(wins);
            for (size_t i = 0; i < mons.size(); ++i)
            {
                LogLine("oaf-rewrite: monitor[" + std::to_string(i) + "] id=" + std::to_string(reinterpret_cast<uintptr_t>(mons[i])) + (static_cast<int>(i) == prim ? " (primary)" : ""));
            }

            for (auto& w : wins)
            {
               LogLine("oaf-rewrite: window hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(w.first)) + " \"" + w.second + "\"");
            }
 
        }

        // Live pick from the frontend's shared block: use the handle verbatim, but only if it still refers to a real screen/window this session. Otherwise fall through to the primary monitor.
        uint32_t kind = 0;
        uint64_t id = 0;
        if (ReadBcSource(kind, id))
        {
            if (kind == shared::BroadcastSourceApp)
            {
                if (IsWindow(reinterpret_cast<HWND>(static_cast<uintptr_t>(id))))
                {
                    return {"hwnd", std::to_string(id)};
                }
            }
            else
            {
                HMONITOR want = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(id));
                for (HMONITOR m : mons)
                {
                    if (m == want)
                    {
                        return {"monitor", std::to_string(id)};
                    }
                }

            }
        }

        if (!mons.empty())
        {
            return {"monitor", std::to_string(reinterpret_cast<uintptr_t>(mons[prim >= 0 ? prim : 0]))};
        }
        
        return {"monitor", kPanelMonitorId};
    }

    // Extract the string array under "projects" from a /features/check request.
    static std::vector<std::string> ExtractProjects(const std::string& j)
    {
        std::vector<std::string> out;
        size_t p = j.find("\"projects\"");
        if (p == std::string::npos) return out;

        size_t lb = j.find('[', p), rb = (lb == std::string::npos) ? lb : j.find(']', lb);
        if (lb == std::string::npos || rb == std::string::npos) return out;

        size_t i = lb + 1;
        while (i < rb)
        {
            size_t q1 = j.find('"', i);
            if (q1 == std::string::npos || q1 >= rb)
                break;
            size_t q2 = j.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 > rb)
                break;
            out.push_back(j.substr(q1 + 1, q2 - q1 - 1));
            i = q2 + 1;
        }
        return out;
    }

    // Build a GK_UPDATE reply containing exactly the requested projects, order preserved, each with its gatekeeper value, defaulting to "false" for unknown.
    // The game looks up each reply project in a registry built from the request, so returning extra projects, as a canned superset did, makes an unknown-name lookup return -1 and a null-deref crash.
    static std::string SynthGkReply(const std::vector<std::string>& projects, const std::string& seq, const std::string& ts)
    {
        std::string payload = "{\"projects\":[";
        for (size_t i = 0; i < projects.size(); ++i)
        {
            std::string val = "false";
            auto it = GGkMap.find(projects[i]);
            if (it != GGkMap.end())
            {
                val = it->second;
            }

            payload += "{\"isPassing\":\"" + val + "\",\"name\":\"" + projects[i] + "\"}";
        
            if (i + 1 < projects.size())
            {
                payload += ",";
            }
        }
        payload += "]}";

        return "{\"messageType\":\"RESPONSE\",\"payload\":" + payload + ",\"payloadType\":\"GK_UPDATE\",\"sequenceId\":\"" + seq + "\",\"timestamp\":\"" + ts + "\"}";
    }

    // Bring the app window to the foreground.
    static void BringWinForeground()
    {
        HWND w = FindWindowW(nullptr, L"Oculus Home Remitted");
        if (!w) return;

        HWND fg = GetForegroundWindow();
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD myThread = GetCurrentThreadId();
        bool attached = fgThread && fgThread != myThread && AttachThreadInput(myThread, fgThread, TRUE);

        ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
        BringWindowToTop(w);
        SetForegroundWindow(w);

        if (attached)
        {
            AttachThreadInput(myThread, fgThread, FALSE);
        }
        
        LogLine("oaf-rewrite: broadcast start, restored and foregrounded the frontend window");
    }

    // Force the OafIpc channel to report connected so the game proceeds to send its OAF requests
    // The originals are still called first so OafIpc sets up its internal state and only the success result is forced. DetourSend then answers the requests that follow.
    static int DetourConnect()
    {
        if (GOrigConnect) GOrigConnect();
        return 1;
    }

    static int DetourConnectLoose()
    {
        if (GOrigConnectLoose) GOrigConnectLoose();
        return 1;
    }

    static int DetourIsConnected()
    {
        return 1;
    }

    static unsigned DetourGetServerPid()
    {
        unsigned real = GOrigGetServerPid ? GOrigGetServerPid() : 0;
        return real ? real : GetCurrentProcessId(); // a nonzero id so the game does not treat the channel as serverless!
    }

    static void* DetourSend(const char* req)
    {
        if (req)
        {
            std::string j(req);
            std::string route = GetField(j, "requestName");
            std::string seq = GetField(j, "sequenceId");
            if (!route.empty() && !seq.empty())
            {
                std::vector<std::string> projects;
                if (route == "/features/check")
                {
                    projects = ExtractProjects(j); // parse before taking the lock
                }

                // Desktop-panel embedding: OVRServer denies it third-party, but a working
                // first-party session answers one /start with three messages:
                // - PANEL_EMBED_START (empty)
                // - CHANGE (monitor)
                // - FINISH (monitor)
                // now the game embeds a monitor panel, then swallow the real denial reply.

                std::string start, change, finish, pd;
                if (route == "/life_cycle/panel_embedding/start")
                {
                    BringWinForeground(); // HACK - focus the app window since sometimes the broadcast's capture can stall

                    std::string pos = ExtractObj(j, "position"), rot = ExtractObj(j, "rotation");
                    std::string ts = NowMsStr();
                    BcSource src = GetBroadcastSource();
                    pd = "{\"id\":\"" + src.id + "\",\"type\":\"" + src.type + "\"}";
                    long id = InterlockedIncrement(&GPanelIdCounter);
                    start = "{\"messageType\":\"PUSH\",\"payload\":{\"appId\":\"1112064135564993\","
                            "\"data\":{\"embeddedPanelId\":null,\"embeddedPanelType\":\"null\","
                            "\"position\":" + pos + ",\"rotation\":" + rot + "},\"id\":" +
                            std::to_string(id) + "},\"payloadType\":\"PANEL_EMBED_START_MESSAGE\","
                            "\"sequenceId\":\"null\",\"timestamp\":\"" + ts + "\"}";
                    change = "{\"messageType\":\"PUSH\",\"payload\":{\"canceled\":false,"
                             "\"panelData\":" + pd + "},\"payloadType\":"
                             "\"PANEL_EMBED_CHANGE_MESSAGE\",\"sequenceId\":\"" + seq +
                             "\",\"timestamp\":\"" + ts + "\"}";
                    finish = "{\"messageType\":\"RESPONSE\",\"payload\":{\"canceled\":false,"
                             "\"panelData\":" + pd + "},\"payloadType\":"
                             "\"PANEL_EMBED_FINISH_MESSAGE\",\"sequenceId\":\"" + seq +
                             "\",\"timestamp\":\"" + ts + "\"}";
                }

                std::lock_guard<std::mutex> l(GMutex);
                if (!start.empty())
                {
                    GInjectQueue.push_back(start);
                    GInjectQueue.push_back(change);
                    GInjectQueue.push_back(finish);
                    GPanelSeqs.insert(seq);
                    LogLine("oaf-rewrite: panel_embedding/start seq " + seq + " injecting START/CHANGE/FINISH (source " + pd + ")");
                }
                else
                {
                    // Answer every known route on Send so the game never waits on OVRServer. This keeps Home working with or without Meta Link active.
                    // Any late real reply for this seq is dropped and routes with no canned answer fall through to the real channel.
                    std::string ts = NowMsStr();
                    std::string synth;
                    if (route == "/features/check")
                    {
                        synth = SynthGkReply(projects, seq, ts);
                    }
                    else if (route == "/library/fetchall")
                    {
                        synth = home2hook::GStore.BuildOafLibraryReply(seq, ts);
                    }
                    else
                    {
                        auto it = GReplies.find(route);
                        if (it != GReplies.end())
                        {
                            synth = it->second;
                            SetField(synth, "sequenceId", seq);
                            if (!ts.empty())
                            {
                                SetField(synth, "timestamp", ts);
                            }
                        }
                    }

                    if (!synth.empty())
                    {
                        GInjectQueue.push_back(synth);
                        GAnsweredSeqs.insert(seq);
                        long n = InterlockedIncrement(&GProactiveCount);
                        if (n <= 80)
                        {
                            LogLine("oaf-rewrite: proactively answered " + route + " seq " + seq);
                        }
                    }
                    else
                    {
                        long n = InterlockedIncrement(&GProactiveMiss);
                        if (n <= 80)
                        {
                            LogLine("oaf-rewrite: no canned reply for " + route + " seq " + seq + ", left unanswered");
                        }
                    }
                }
            }
        }
        return GOrigSend(req);
    }

    static int DetourGetReply(wchar_t** outReply, int* outStatus)
    {
        // Deliver any injected messages (the panel_embedding flow) before real replies.
        {
            std::lock_guard<std::mutex> l(GMutex);
            if (!GInjectQueue.empty())
            {
                std::string msg = GInjectQueue.front();
                GInjectQueue.pop_front();
                wchar_t* mine = WideDupUtf8(msg);
                GMine.insert(mine);
                if (outReply)
                    *outReply = mine;
                if (outStatus)
                    *outStatus = 0;

                return 1; // a message is present
            }
        }

        int ret = GOrigGetReply(outReply, outStatus);

        // Swallow OVRServer's real (denied) reply for panel seqs we've already answered by injection, so the game doesn't also see the error.
        while (outReply && *outReply)
        {
            bool swallow;
            {
                std::string rseq = GetField(Utf8FromWide(*outReply), "sequenceId");
                std::lock_guard<std::mutex> l(GMutex);
                swallow = GPanelSeqs.count(rseq) > 0 || GAnsweredSeqs.count(rseq) > 0;
            }

            if (!swallow) break;
            GOrigFree(*outReply);
            *outReply = nullptr;
            ret = GOrigGetReply(outReply, outStatus);
        }

        // Anything still here is a real reply for a route that did not answer on send
        return ret;
    }

    static void DetourFree(void* p)
    {
        bool mine = false;
        {
            std::lock_guard<std::mutex> l(GMutex);
            auto it = GMine.find(p);
            if (it != GMine.end())
            {
                GMine.erase(it);
                mine = true;
            }
        }

        if (mine)
        {
            delete[] reinterpret_cast<wchar_t*>(p);
            return; // never hand the buffer to the DLL's free
        }
        GOrigFree(p);
    }

    static bool LoadReplies(const std::wstring& tsvPath)
    {
        HANDLE f = CreateFileW(tsvPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;

        std::string data;
        char buf[65536];
        DWORD r = 0;
        while (ReadFile(f, buf, sizeof(buf), &r, nullptr) && r > 0)
        {
            data.append(buf, r);
        }
        
        CloseHandle(f);

        size_t pos = 0, count = 0;
        while (pos < data.size())
        {
            size_t nl = data.find('\n', pos);
            std::string line = data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? data.size() : nl + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;

            std::string route = line.substr(0, tab);
            std::string reply = line.substr(tab + 1);
            if (!route.empty() && !reply.empty())
            {
                GReplies[route] = reply;
                ++count;
            }
        }
        LogLine("oaf-rewrite: loaded " + std::to_string(count) + " reply templates from " + NarrowUtf8(tsvPath));
        
        if (count > 0)
        {
            InterlockedExchange(&GRepliesLoaded, 1);
        }

        return count > 0;
    }

    // Install all three OafIpc hooks in a single thread-freeze (MH_ApplyQueued).
    // The fatal early exchanges (home_ready/get_worlds_location/get_demo_settings) fire within 90ms of OafIpc.dll loading, and get_demo_settings must be caught or the game's DemoModeFetchEvent times out after 3s into LoginFailed.
    void InstallOafRewriteHooksNow(void* oafModule)
    {
        HMODULE mod = reinterpret_cast<HMODULE>(oafModule);
        if (GRewriteInstalled || !GRepliesLoaded || !mod)
            return;
        if (InterlockedCompareExchange(&GRewriteInstalled, 1, 0) != 0)
            return; // another thread won

        void* pSend = reinterpret_cast<void*>(GetProcAddress(mod, "OafIpc_Send"));
        void* pGet = reinterpret_cast<void*>(GetProcAddress(mod, "OafIpc_GetReply"));
        void* pFree = reinterpret_cast<void*>(GetProcAddress(mod, "OafIpc_FreeReplyMessage"));

        // Connection surface is forced to report connected so the game sends its OAF requests without OVRServer needing to be active
        void* pConnect = reinterpret_cast<void*>(GetProcAddress(mod, "OafIpc_Connect"));
        void* pConnectLoose = reinterpret_cast<void*>(GetProcAddress(mod, "OafIpc_ConnectLoosePerms"));
        void* pIsConn = reinterpret_cast<void*>(GetProcAddress(mod, "OafIpc_IsConnected"));
        void* pPid = reinterpret_cast<void*>(GetProcAddress(mod, "OafIpc_GetServerProcessId"));

        bool ok = pSend && pGet && pFree;
        if (ok)
        {
            ok &= MH_CreateHook(pSend, reinterpret_cast<void*>(&DetourSend),
                                reinterpret_cast<void**>(&GOrigSend)) == MH_OK;
            ok &= MH_CreateHook(pGet, reinterpret_cast<void*>(&DetourGetReply),
                                reinterpret_cast<void**>(&GOrigGetReply)) == MH_OK;
            ok &= MH_CreateHook(pFree, reinterpret_cast<void*>(&DetourFree),
                                reinterpret_cast<void**>(&GOrigFree)) == MH_OK;
            MH_QueueEnableHook(pSend);
            MH_QueueEnableHook(pGet);
            MH_QueueEnableHook(pFree);

            // These connection hooks are not guaranteed. A missing one is skipped rather than failing the whole install :shrugs:
            if (pConnect && MH_CreateHook(pConnect, reinterpret_cast<void*>(&DetourConnect), reinterpret_cast<void**>(&GOrigConnect)) == MH_OK)
                MH_QueueEnableHook(pConnect);
            if (pConnectLoose && MH_CreateHook(pConnectLoose, reinterpret_cast<void*>(&DetourConnectLoose), reinterpret_cast<void**>(&GOrigConnectLoose)) == MH_OK)
                MH_QueueEnableHook(pConnectLoose);
            if (pIsConn && MH_CreateHook(pIsConn, reinterpret_cast<void*>(&DetourIsConnected), reinterpret_cast<void**>(&GOrigIsConnected)) == MH_OK)
                MH_QueueEnableHook(pIsConn);
            if (pPid && MH_CreateHook(pPid, reinterpret_cast<void*>(&DetourGetServerPid), reinterpret_cast<void**>(&GOrigGetServerPid)) == MH_OK)
                MH_QueueEnableHook(pPid);

            ok &= MH_ApplyQueued() == MH_OK;
        }
        LogLine(std::string("oaf-rewrite: hooks installed on OafIpc.dll (") + (ok ? "all ok, single-freeze" : "failed") + "), connect/is_connected forced so now the game sends its OAF requests without needing OVRServer and requests are answered on send!");
    }

    bool OafHooksInstalled()
    {
        return GRewriteInstalled != 0;
    }

    static DWORD WINAPI RewriteWaiter(LPVOID)
    {
        // Backup: if OafIpc.dll was already mapped before the LoadLibrary hook could fire.
        for (int i = 0; i < 8000 && !GRewriteInstalled; ++i)
        {
            HMODULE mod = GetModuleHandleW(L"OafIpc.dll");
            if (mod)
            {
                InstallOafRewriteHooksNow(mod);
                return 0;
            }
            Sleep(15);
        }
        return 0;
    }

    static void LoadGkMap(const std::wstring& tsvPath)
    {
        HANDLE f = CreateFileW(tsvPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            LogLine("oaf-rewrite: WARN oaf_gk.tsv missing, features/check gates default false");
            return;
        }
        std::string data;
        char buf[65536];
        DWORD r = 0;
        while (ReadFile(f, buf, sizeof(buf), &r, nullptr) && r > 0)
        {
            data.append(buf, r);
        }

        CloseHandle(f);
        size_t pos = 0, count = 0;
        while (pos < data.size())
        {
            size_t nl = data.find('\n', pos);
            std::string line = data.substr(pos, nl == std::string::npos ? std::string::npos
                                                                         : nl - pos);
            pos = (nl == std::string::npos) ? data.size() : nl + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;

            std::string name = line.substr(0, tab), val = line.substr(tab + 1);
            if (!name.empty() && !val.empty())
            {
                GGkMap[name] = val;
                ++count;
            }
        }

        LogLine("oaf-rewrite: loaded " + std::to_string(count) + " gatekeeper values");
    }

    bool InstallOafRewrite(const std::wstring& dir)
    {
        if (!LoadReplies(dir + L"\\store\\oaf_replies.tsv"))
        {
            LogLine("oaf-rewrite: WARN oaf_replies.tsv missing/empty, rewrite disabled (the direct-launch GK/third-party errors will not be patched)");
            return false;
        }
        LoadGkMap(dir + L"\\store\\oaf_gk.tsv");

        // Broadcast source: open the frontend's session-only shared block read-only. If it is absent, with the probe injected and no frontend running, GetBroadcastSource() falls back to the primary monitor.
        OpenBcView();

        HANDLE t = CreateThread(nullptr, 0, RewriteWaiter, nullptr, 0, nullptr);
        if (t)
        {
            CloseHandle(t);
        }

        return true;
    }

}
