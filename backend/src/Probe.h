#pragma once

#include <string>

namespace home2backend {

// Verdict signals set as events happen, and the log is the authoritative record.
extern volatile bool GGameSentRequestPlain; // SSL_write fired with an HTTP request
extern volatile bool GHandshakeOk;          // the SChannel server completed a handshake
extern volatile bool GRequestOverTls;       // decrypted a request off the TLS channel

bool InstallRedirectHooks(); // getaddrinfo / GetAddrInfoW / connect steered to 127.0.0.1
bool InstallSslWatch();      // SSL_write@0x20fe110 / SSL_read@0x20fda20, log-only
bool InstallCaStoreHooks();  // inject the CA into the game's root-store trust
bool InstallVerifyHook();    // X509_verify_cert@0x20ea9f0 forced to 1
bool InstallTokenHooks();    // offline access-token synthesis (LibOVRPlatform)
bool InstallOvrRuntimeHooks(); // OVRPlugin ovrp_GetAppShouldQuit forced false (direct launch)
bool StartTlsServer();       // SChannel server on 127.0.0.1:443 presenting the leaf
bool InstallOafCapture(const std::wstring& dir); // log the OAF loopback IPC to OVRServer
bool InstallGraphqlCapture(const std::wstring& dir); // hook SSL_read/SSL_write, dump graph.oculus.com plaintext to graphql_raw.bin
bool InstallVertsCapture(const std::wstring& dir); // enable the Verts state-capture recorder, dump verts_capture_<n>.bin
bool InstallOafRewrite(const std::wstring& dir); // swap OAF NOTIFICATION errors for success
void InstallOafRewriteHooksNow(void* oafModule); // arm the rewrite hooks the instant OafIpc loads
void PreloadOafIpc();        // load OafIpc.dll and arm OAF hooks now (removes the seq 1-3 race)
bool InstallLoginPatch();    // force OnLoginComplete onto its success path (exe byte patch)

}
