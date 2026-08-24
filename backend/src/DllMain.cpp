#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include "MinHook.h"
#include "Probe.h"
#include "ProbePrefs.h"
#include "Certs.h"
#include "ResponseStore.h"
#include "BackendLogger.h"

#include <string>
#include <cwctype>
#include <initializer_list>

// The command line and parent process this shipping exe was launched with.
// Reveals which launch arg or session context from OVRServer
static std::string LaunchContext()
{
    std::string out;
    LPWSTR cl = GetCommandLineW();
    out += "cmdline: " + home2backend::NarrowUtf8(cl ? cl : L"(null)");

    DWORD pid = GetCurrentProcessId(), ppid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        std::wstring parentName = L"?";
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (pe.th32ProcessID == pid)
                {
                    ppid = pe.th32ParentProcessID;
                }
                
            } while (Process32NextW(snap, &pe));

            if (ppid && Process32FirstW(snap, &pe))
            {
                do
                {
                    if (pe.th32ProcessID == ppid)
                    {
                        parentName = pe.szExeFile;
                        break;
                    }
                } while (Process32NextW(snap, &pe));
            }
        }
        CloseHandle(snap);
        out += "  |  parent: " + home2backend::NarrowUtf8(parentName) + " (pid " + std::to_string(ppid) + ")";
    }

    return out;
}

using namespace home2backend;

namespace home2backend {
volatile bool GGameSentRequestPlain = false;
volatile bool GHandshakeOk = false;
volatile bool GRequestOverTls = false;
}

static HMODULE GSelfModule = nullptr;
static std::wstring selfDir;

static std::wstring SelfDirectory()
{
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(GSelfModule, path, MAX_PATH);
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring(L".") : full.substr(0, slash);
}

// True if the host process command line contains any of the given argument tokens (case-insensitive).
// The command line is lowercased once, then each token is tested, so a set of args is one scan.
static bool HasLaunchArg(std::initializer_list<const wchar_t*> tokens)
{
    const wchar_t* cl = GetCommandLineW();
    if (!cl) return false;

    std::wstring line(cl);
    for (auto& c : line) c = towlower(c);

    for (const wchar_t* tok : tokens)
    {
        if (!tok) continue;
        std::wstring t(tok);
        for (auto& c : t) c = towlower(c);
        if (line.find(t) != std::wstring::npos) return true;
    }
    return false;
}

static bool HasLaunchArg(const wchar_t* token) { return HasLaunchArg({ token }); }

static DWORD WINAPI ResultThread(LPVOID)
{
    for (;;)
    {
        Sleep(10000);
        bool accepted = GGameSentRequestPlain || GRequestOverTls;
        std::string verdict;
        if (accepted)
            verdict = "In-process cert verification defeated! The game accepted the loopback leaf!";
        else if (GHandshakeOk)
            verdict = "Handshake completed but no request seen yet, waiting...";
        else
            verdict = "No accepted handshake / request yet, waiting (CA not yet trusted, or curl imported roots before injection)";
        
        LogLine("result so far: sslWriteReq=" + std::string(GGameSentRequestPlain ? "yes" : "no") + " tlsHandshake=" + (GHandshakeOk ? "yes" : "no") + " reqOverTls=" + (GRequestOverTls ? "yes" : "no") + ": " + verdict);
    }
}

static DWORD WINAPI InitThread(LPVOID)
{
    if (selfDir.empty())
    {
        selfDir = SelfDirectory();
    }

    if (MH_Initialize() != MH_OK)
    {
        LogLine("FATAL: MH_Initialize failed");
        return 1;
    }

    // TIME SENSITIVE
    // Arm the OAF rewrite first ahead of every heavier step below, especially the later "InstallLoginPatch" exe signature scan, which can take seconds..
    // OafIpc_Send/GetReply are hooked with maximum lead time after injection.
    InstallOafRewrite(selfDir);
    PreloadOafIpc();

    // Arm the token hooks first (before the heavier cert/TLS setup) so ovr_User_GetAccessToken is caught as early as possible in startup.
    InstallTokenHooks();

    if (!GLog.Open(selfDir + L"\\backend.log"))
    {
        wchar_t temp[MAX_PATH] = { 0 };
        if (GetTempPathW(MAX_PATH, temp) > 0)
        {
            GLog.Open(std::wstring(temp) + L"backend.log");
        }
    }
    LogLine("========== home2backend.dll loaded ==========");

    // Feed the de-identified identity (userId/oculusId/displayName) from the preferences.json into the login spoof before the token hooks are armed.
    // Missing or unparseable prefs leave the neutral built-in defaults. The ResponseStore loads the same file itself for its owner_id/name substitution.
    home2backend::Identity identity;
    if (home2backend::LoadIdentity(selfDir + L"\\preferences.json", identity))
        LogLine("prefs: identity loaded from preferences.json");
    else
        LogLine("prefs: no usable identity in preferences.json, using neutral defaults");

    home2backend::SetIdentity(identity);

    // Force the OculusWorlds login-complete handler onto its success path so the platform login stops failing
    // Applied early, well before OnLoginComplete fires in Init.
    InstallLoginPatch();

    // Direct-launch survival: spawn a waiter that hooks OVRPlugin's ShouldQuit the moment OVRPlugin.dll loads so the app doesn't quit when launched outside OVRServer's app-launch flow.
    InstallOvrRuntimeHooks();

    //Install the CA-store hooks first, before cert-gen and TLS setup, so a full root enumeration by the game's curl is logged no matter how early it imports
    InstallCaStoreHooks();

    if (!GenerateCerts()) LogLine("FATAL: certificate generation failed!");

    // Drop the CA as a PEM and point OpenSSL's env-var CA overrides at it.
    // If the game's OpenSSL calls set_default_verify_paths it consults these. This is a non-invasive test, with no Windows-store change, of whether it honors SSL_CERT_FILE.
    std::wstring pem = selfDir + L"\\home2probe_ca.pem";
    if (ExportCaPem(pem))
    {
        SetEnvironmentVariableW(L"SSL_CERT_FILE", pem.c_str());
        SetEnvironmentVariableW(L"SSL_CERT_DIR", selfDir.c_str());
        SetEnvironmentVariableW(L"CURL_CA_BUNDLE", pem.c_str());
        LogLine("certs: wrote CA PEM to " + NarrowUtf8(pem) + " and set SSL_CERT_FILE / SSL_CERT_DIR / CURL_CA_BUNDLE");
    }
    else
    {
        LogLine("certs: ExportCaPem failed, SSL_CERT_FILE test skipped");
    }

    // Load home2hook's ResponseStore from store\ next to the DLL and route its log lines into the same file. The loopback TLS server serves its canned /graphql answers.
    std::wstring storeDir = selfDir + L"\\store";
    if (home2hook::GStore.Load(storeDir))
    {
        LogLine("store: ResponseStore loaded from " + NarrowUtf8(storeDir) + " (loopback server will serve canned /graphql)");
    } 
    else
    {
        LogLine("store: WARN ResponseStore load failed from " + NarrowUtf8(storeDir) + ", loopback server will serve empty {} (world_login will fail)");
    }

    StartTlsServer();// presents the CA-signed leaf, serves the ResponseStore
    InstallRedirectHooks();
    InstallSslWatch();
    InstallVerifyHook();// force game-exe X509_verify_cert to success

    LogLine("Backend installed! graph.oculus.com redirected to 127.0.0.1:443 with the leaf signed by the CA.");
    LogLine("A pass shows '*** HANDSHAKE COMPLETED ***' and '*** GAME SENT REQUEST (/graphql) ***'. A fail shows 'HANDSHAKE FAILED / REJECTED' and no request.");

    HANDLE v = CreateThread(nullptr, 0, ResultThread, nullptr, 0, nullptr);
    if (v) CloseHandle(v);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        GSelfModule = module;
        DisableThreadLibraryCalls(module);
        {
            HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (t)
            {
                CloseHandle(t);
            } 
        }
        break;
    case DLL_PROCESS_DETACH:
        GLog.Close();
        break;
    default:
        break;
    }
    return TRUE;
}
