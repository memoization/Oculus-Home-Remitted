#include "Probe.h"
#include "BackendLogger.h"
#include "Certs.h"
#include "ResponseStore.h"
#include "ResponseDefinitions.h" // home2hook::DocIdName (readable doc_id to op name for logs)
#include "HttpParse.h"

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <string>
#include <vector>
#include <set>
#include <mutex>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace home2backend {

    static CredHandle GCred;
    static bool GCredReady = false;
    static HCERTSTORE GChainStore = nullptr;

    static bool InitCredentials()
    {
        PCCERT_CONTEXT leaf = GetLeafCert();
        PCCERT_CONTEXT ca = GetCaCert();
        if (!leaf)
        {
            LogLine("tls: no leaf certificate (cert generation failed)");
            return false;
        }
        GChainStore = CertOpenStore(CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, 0, 0,
                                    nullptr);
        PCCERT_CONTEXT leafInStore = nullptr;
        if (GChainStore)
        {
            if (ca)
            {
                CertAddCertificateContextToStore(GChainStore, ca, CERT_STORE_ADD_ALWAYS, nullptr);
            }
        
            CertAddCertificateContextToStore(GChainStore, leaf, CERT_STORE_ADD_ALWAYS, &leafInStore);
        }
        PCCERT_CONTEXT present = leafInStore ? leafInStore : leaf;

        SCHANNEL_CRED sc;
        ZeroMemory(&sc, sizeof(sc));
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.cCreds = 1;
        sc.paCred = &present;
        sc.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER | SP_PROT_TLS1_1_SERVER | SP_PROT_TLS1_0_SERVER;
        sc.dwFlags = SCH_CRED_NO_SYSTEM_MAPPER;

        TimeStamp expiry;
        SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr, &sc, nullptr, nullptr, &GCred, &expiry);
        
        if (ss != SEC_E_OK)
        {
            LogLine("tls: AcquireCredentialsHandle failed 0x" + HexU(static_cast<unsigned long>(ss)));
            return false;
        }

        GCredReady = true;
        LogLine("tls: server credentials ready (leaf CN=graph.oculus.com signed by the CA, TLS1.0-1.2)");
        return true;
    }

    static std::string ParseSni(const unsigned char* d, size_t n)
    {
        if (n < 43 || d[0] != 0x16)
            return "";
        size_t i = 5 + 4 + 2 + 32;
        if (i + 1 > n) return "";
        size_t sidLen = d[i]; i += 1 + sidLen;
        if (i + 2 > n) return "";
        size_t csLen = (size_t(d[i]) << 8) | d[i + 1]; i += 2 + csLen;
        if (i + 1 > n) return "";
        size_t compLen = d[i]; i += 1 + compLen;
        if (i + 2 > n) return "";
        size_t extLen = (size_t(d[i]) << 8) | d[i + 1]; i += 2;
        size_t extEnd = i + extLen; if (extEnd > n) extEnd = n;
        while (i + 4 <= extEnd)
        {
            size_t etype = (size_t(d[i]) << 8) | d[i + 1];
            size_t elen = (size_t(d[i + 2]) << 8) | d[i + 3];
            i += 4;
            if (etype == 0x0000)
            {
                if (i + 5 <= extEnd)
                {
                    size_t nameLen = (size_t(d[i + 3]) << 8) | d[i + 4];
                    if (i + 5 + nameLen <= extEnd)
                        return std::string(reinterpret_cast<const char*>(d + i + 5), nameLen);
                }
                return "";
            }
            i += elen;
        }
        return "";
    }

    // ---- HTTP helpers ----

    static long HeaderContentLength(const std::string& headers)
    {
        // case-insensitive search for "content-length:"
        std::string lower = headers;
        for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        size_t p = lower.find("content-length:");
        if (p == std::string::npos)
            return 0;
        p += 15;
        while (p < headers.size() && (headers[p] == ' ' || headers[p] == '\t')) ++p;
        return std::strtol(headers.c_str() + p, nullptr, 10);
    }

    static bool HeaderHasExpectContinue(const std::string& headers)
    {
        std::string lower = headers;
        for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return lower.find("expect:") != std::string::npos &&
               lower.find("100-continue") != std::string::npos;
    }

    static std::string FrameBenign(const std::string& body)
    {
        std::string r = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
        r += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
        r += body;
        return r;
    }

    // Look up the served response for a complete request via the store.
    static std::string BuildServed(const std::string& request)
    {
        std::string docId, variables;
        if (home2hook::ParseGraphqlRequest(request, docId, variables))
        {
            home2hook::ResponseAction action = home2hook::GStore.Classify(docId);
            std::string resp = home2hook::GStore.BuildResponse(action, docId, variables);
            if (!resp.empty())
            {
                LogLine("tls: served /graphql doc_id " + docId + " (" + std::string(home2hook::DocIdName(docId)) + ") " + std::to_string(resp.size()) + "B");
                GRequestOverTls = true;
                return resp;
            }
            LogLine("tls: /graphql doc_id " + docId + " (" + std::string(home2hook::DocIdName(docId)) + ") unknown/passthrough, returning {\"data\":{}}");
            // One-time per doc_id: log the decoded variables so an uncaptured mutation (world like/delete, the favorites query, and so on) reveals its doc_id and shape for us to implement.
            // 'variables' carries world data only. The access_token is a separate form field and isn't logged.
            {
                static std::mutex varLogMutex;
                static std::set<std::string> varLogged;
                std::lock_guard<std::mutex> lk(varLogMutex);
                // Mutations (client_mutation_id present) are user-triggered and low-frequency, so log every occurrence so toggles and repeats (like/unlike, repeated entry-point presses) are each visible with their own variables.
                bool isMutation = variables.find("client_mutation_id") != std::string::npos;
                bool logIt = isMutation ? true : varLogged.insert(docId).second;
            
                if (logIt && !variables.empty())
                    LogLine("tls: doc_id " + docId + " variables: " + variables);
            }
            return FrameBenign("{\"data\":{}}");
        }

        // Media uploads (multipart/form-data): world screenshot / cubemap.
        // Detect on the request line, write the raw bytes into the world folder and 200 with an empty body.
        // The body carries image bytes, so log id and byte count only.
        std::string line = FirstLine(request.data(), static_cast<int>(request.size()));
        bool isScreenshot = line.find("/world_upload_screenshot") != std::string::npos;
        bool isCubemap = line.find("/world_upload_cubemap") != std::string::npos;
        if (isScreenshot || isCubemap)
        {
            std::string worldId, fileBytes, ext;
            if (home2hook::ParseMultipartUpload(request, worldId, fileBytes, ext))
            {
                bool ok = home2hook::GStore.WriteWorldMedia(worldId, isScreenshot, fileBytes);
                LogLine("tls: REST " + std::string(isScreenshot ? "screenshot" : "cubemap") + " upload world_id=" + worldId + " bytes=" + std::to_string(fileBytes.size()) + (ok ? ", stored" : ", store failed"));
            }
            else
            {
                LogLine("tls: REST upload multipart parse failed [" + line + "]");
            }
            GRequestOverTls = true;
            return FrameBenign("{}");
        }

        LogLine("tls: non-graphql request [" + line + "] answered with {}");
        return FrameBenign("{}");
    }

    // Encrypt and send plaintext, chunked to the negotiated max message size.
    static bool SendEncrypted(CtxtHandle* ctx, SOCKET sock, const SecPkgContext_StreamSizes& sizes, const char* data, size_t len)
    {
        std::vector<char> buf(sizes.cbHeader + sizes.cbMaximumMessage + sizes.cbTrailer);
        size_t off = 0;
        do
        {
            size_t chunk = len - off;
            if (chunk > sizes.cbMaximumMessage)
            {
                chunk = sizes.cbMaximumMessage;
            }

            std::memcpy(buf.data() + sizes.cbHeader, data + off, chunk);

            SecBuffer sb[4];
            sb[0].BufferType = SECBUFFER_STREAM_HEADER;
            sb[0].cbBuffer = sizes.cbHeader;
            sb[0].pvBuffer = buf.data();
            sb[1].BufferType = SECBUFFER_DATA;
            sb[1].cbBuffer = static_cast<unsigned long>(chunk);
            sb[1].pvBuffer = buf.data() + sizes.cbHeader;
            sb[2].BufferType = SECBUFFER_STREAM_TRAILER;
            sb[2].cbBuffer = sizes.cbTrailer;
            sb[2].pvBuffer = buf.data() + sizes.cbHeader + chunk;
            sb[3].BufferType = SECBUFFER_EMPTY;
            sb[3].cbBuffer = 0;
            sb[3].pvBuffer = nullptr;
            SecBufferDesc desc = {SECBUFFER_VERSION, 4, sb};
            if (EncryptMessage(ctx, 0, &desc, 0) != SEC_E_OK) return false;

            int total = static_cast<int>(sb[0].cbBuffer + sb[1].cbBuffer + sb[2].cbBuffer);
            int sent = 0;
            while (sent < total)
            {
                int s = send(sock, buf.data() + sent, total - sent, 0);
                if (s <= 0) return false;

                sent += s;
            }
            off += chunk;
        } while (off < len);
        
        return true;
    }

    // Read+serve requests over the established TLS connection until it closes.
    static void ServeConnection(CtxtHandle* ctx, SOCKET sock, std::vector<char>& encSeed)
    {
        SecPkgContext_StreamSizes sizes;
        if (QueryContextAttributes(ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK)
        {
            LogLine("tls: SECPKG_ATTR_STREAM_SIZES failed");
            return;
        }
        std::vector<char> enc = encSeed; // encrypted accumulator
        std::string plain; // decrypted accumulator
        char rbuf[16384];
        bool sentExpect = false;

        for (;;)
        {
            // 1) Try to extract a complete request from 'plain'.
            size_t he = plain.find("\r\n\r\n");
            if (he != std::string::npos)
            {
                std::string headers = plain.substr(0, he);
                long contentLen = HeaderContentLength(headers);
                size_t need = he + 4 + (contentLen > 0 ? static_cast<size_t>(contentLen) : 0);
                if (HeaderHasExpectContinue(headers) && !sentExpect && plain.size() < need)
                {
                    static const char kCont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    SendEncrypted(ctx, sock, sizes, kCont, sizeof(kCont) - 1);
                    sentExpect = true;
                }
                if (plain.size() >= need)
                {
                    std::string request = plain.substr(0, need);
                    plain.erase(0, need);
                    sentExpect = false;
                    std::string response = BuildServed(request);
                    if (!response.empty())
                        SendEncrypted(ctx, sock, sizes, response.data(), response.size());
                    if (response.find("\r\nConnection: close\r\n") != std::string::npos)
                        return; // response asked to close, so close after sending
                    continue; // keep-alive, serve the next request on this connection
                }
            }

            //2) decrypt what's current, else recv.
            if (!enc.empty())
            {
                SecBuffer sb[4];
                sb[0].BufferType = SECBUFFER_DATA;
                sb[0].cbBuffer = static_cast<unsigned long>(enc.size());
                sb[0].pvBuffer = enc.data();
                sb[1].BufferType = SECBUFFER_EMPTY;
                sb[2].BufferType = SECBUFFER_EMPTY;
                sb[3].BufferType = SECBUFFER_EMPTY;
                SecBufferDesc desc = {SECBUFFER_VERSION, 4, sb};
                SECURITY_STATUS ss = DecryptMessage(ctx, &desc, 0, nullptr);
                if (ss == SEC_E_OK)
                {
                    std::vector<char> leftover;
                    for (int i = 0; i < 4; ++i)
                    {
                        if (sb[i].BufferType == SECBUFFER_DATA && sb[i].cbBuffer > 0)
                            plain.append(static_cast<char*>(sb[i].pvBuffer), sb[i].cbBuffer);
                        else if (sb[i].BufferType == SECBUFFER_EXTRA && sb[i].cbBuffer > 0)
                            leftover.assign(static_cast<char*>(sb[i].pvBuffer),
                                            static_cast<char*>(sb[i].pvBuffer) + sb[i].cbBuffer);
                    }
                    enc.swap(leftover);
                    continue;
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) return; // peer sent close_notify

                if (ss == SEC_I_RENEGOTIATE)
                {
                    LogLine("tls: renegotiate requested, closing connection");
                    return;
                }

                if (ss != SEC_E_INCOMPLETE_MESSAGE)
                {
                    LogLine("tls: DecryptMessage sec=0x" + HexU(static_cast<unsigned long>(ss)));
                    return;
                }
                // SEC_E_INCOMPLETE_MESSAGE: fall through to recv more.
            }

            int r = recv(sock, rbuf, sizeof(rbuf), 0);
            if (r <= 0) return; // connection closed
            enc.insert(enc.end(), rbuf, rbuf + r);
        }
    }

    static void HandleConnection(SOCKET sock)
    {
        CtxtHandle ctx;
        bool ctxInit = false;
        int callN = 0;
        bool sniLogged = false;
        std::vector<char> inbuf;
        char chunk[8192];
        DWORD flags = ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_CONFIDENTIALITY |
                      ASC_REQ_REPLAY_DETECT | ASC_REQ_SEQUENCE_DETECT |
                      ASC_REQ_EXTENDED_ERROR;

        for (;;)
        {
            int r = recv(sock, chunk, sizeof(chunk), 0);
            if (r <= 0)
            {
                LogLine("tls: connection closed before handshake completed (recv=" + std::to_string(r) + ")");
                break;
            }
            inbuf.insert(inbuf.end(), chunk, chunk + r);

            if (!sniLogged)
            {
                std::string sni = ParseSni(
                    reinterpret_cast<const unsigned char*>(inbuf.data()), inbuf.size());
                LogLine("tls: ClientHello bytes=" + std::to_string(inbuf.size()) + " SNI=" + (sni.empty() ? std::string("(none/partial)") : sni));
                sniLogged = true;
            }

            SecBuffer inSec[2];
            inSec[0].BufferType = SECBUFFER_TOKEN;
            inSec[0].cbBuffer = static_cast<unsigned long>(inbuf.size());
            inSec[0].pvBuffer = inbuf.data();
            inSec[1].BufferType = SECBUFFER_EMPTY;
            inSec[1].cbBuffer = 0;
            inSec[1].pvBuffer = nullptr;
            SecBufferDesc inDesc = {SECBUFFER_VERSION, 2, inSec};

            SecBuffer outSec[1];
            outSec[0].BufferType = SECBUFFER_TOKEN;
            outSec[0].cbBuffer = 0;
            outSec[0].pvBuffer = nullptr;
            SecBufferDesc outDesc = {SECBUFFER_VERSION, 1, outSec};

            DWORD attr = 0;
            TimeStamp expiry;
            SECURITY_STATUS ss = AcceptSecurityContext(
                &GCred, ctxInit ? &ctx : nullptr, &inDesc, flags, SECURITY_NATIVE_DREP,
                &ctx, &outDesc, &attr, &expiry);
            ctxInit = true;

            LogLine("tls: AcceptSecurityContext call#" + std::to_string(callN) + " ss=0x" +
                    HexU(static_cast<unsigned long>(ss)) + " outBytes=" +
                    std::to_string(outSec[0].cbBuffer) + " inBytes=" +
                    std::to_string(inbuf.size()));
            ++callN;

            if (outSec[0].cbBuffer > 0 && outSec[0].pvBuffer)
            {
                send(sock, static_cast<char*>(outSec[0].pvBuffer),
                     static_cast<int>(outSec[0].cbBuffer), 0);
                FreeContextBuffer(outSec[0].pvBuffer);
            }

            if (ss == SEC_E_INCOMPLETE_MESSAGE)
                continue;

            if (ss == SEC_I_CONTINUE_NEEDED)
            {
                if (inSec[1].BufferType == SECBUFFER_EXTRA && inSec[1].cbBuffer > 0)
                    inbuf.erase(inbuf.begin(), inbuf.end() - inSec[1].cbBuffer);
                else
                    inbuf.clear();
                continue;
            }

            if (ss == SEC_E_OK)
            {
                std::vector<char> extra;
                if (inSec[1].BufferType == SECBUFFER_EXTRA && inSec[1].cbBuffer > 0)
                    extra.assign(inbuf.end() - inSec[1].cbBuffer, inbuf.end());

                SecPkgContext_ConnectionInfo ci;
                if (QueryContextAttributes(&ctx, SECPKG_ATTR_CONNECTION_INFO, &ci) ==
                    SEC_E_OK)
                    LogLine("tls: *** HANDSHAKE COMPLETED *** proto=0x" + HexU(ci.dwProtocol) + " cipher=0x" + HexU(ci.aiCipher) + " strength=" + std::to_string(ci.dwCipherStrength));
                else
                    LogLine("tls: *** HANDSHAKE COMPLETED *** (connection info N/A)");
                GHandshakeOk = true;

                ServeConnection(&ctx, sock, extra);
                break;
            }

            LogLine("tls: HANDSHAKE FAILED / REJECTED sec=0x" + HexU(static_cast<unsigned long>(ss)));
            break;
        }

        if (ctxInit)
        {
            DeleteSecurityContext(&ctx);
        }

        closesocket(sock);
    }

    static DWORD WINAPI ConnThread(LPVOID param)
    {
        HandleConnection(reinterpret_cast<SOCKET>(param));
        return 0;
    }

    static DWORD WINAPI ServerThread(LPVOID)
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        if (!InitCredentials())
        {
            LogLine("tls: credentials init failed, server not started");
            return 1;
        }

        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
        {
            LogLine("tls: socket() failed");
            return 1;
        }
        BOOL yes = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes), sizeof(yes));

        sockaddr_in addr;
        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(443);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
            SOCKET_ERROR)
        {
            LogLine("tls: bind 127.0.0.1:443 failed err=" + std::to_string(WSAGetLastError()) + " (is another process using :443?)");
            closesocket(listener);
            return 1;
        }
        if (listen(listener, 16) == SOCKET_ERROR)
        {
            LogLine("tls: listen failed");
            closesocket(listener);
            return 1;
        }
        LogLine("tls: listening on 127.0.0.1:443, serving ResponseStore over TLS...");

        for (;;)
        {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                continue;
            HANDLE t = CreateThread(nullptr, 0, ConnThread,
                                    reinterpret_cast<LPVOID>(client), 0, nullptr);
            if (t)
                CloseHandle(t);
            else
                HandleConnection(client);
        }
    }

    bool StartTlsServer()
    {
        HANDLE t = CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
        if (t)
        {
            CloseHandle(t);
            return true;
        }
        return false;
    }

}
