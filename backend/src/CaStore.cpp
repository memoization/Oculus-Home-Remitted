#include "Probe.h"
#include "BackendLogger.h"
#include "Certs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include "MinHook.h"

#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "crypt32.lib")

namespace home2backend {

    typedef HCERTSTORE(WINAPI* OpenSysWFn)(HCRYPTPROV_LEGACY, LPCWSTR);
    typedef HCERTSTORE(WINAPI* OpenSysAFn)(HCRYPTPROV_LEGACY, LPCSTR);
    typedef HCERTSTORE(WINAPI* OpenStoreFn)(LPCSTR, DWORD, HCRYPTPROV_LEGACY, DWORD,
                                            const void*);
    typedef PCCERT_CONTEXT(WINAPI* EnumFn)(HCERTSTORE, PCCERT_CONTEXT);
    typedef PCCERT_CONTEXT(WINAPI* FindFn)(HCERTSTORE, DWORD, DWORD, DWORD,
                                           const void*, PCCERT_CONTEXT);
    typedef BOOL(WINAPI* GetChainFn)(HCERTCHAINENGINE, PCCERT_CONTEXT, LPFILETIME,
                                     HCERTSTORE, PCERT_CHAIN_PARA, DWORD, LPVOID,
                                     PCCERT_CHAIN_CONTEXT*);

    static OpenSysWFn GOrigOpenSysW = nullptr;
    static OpenSysAFn GOrigOpenSysA = nullptr;
    static OpenStoreFn GOrigOpenStore = nullptr;
    static EnumFn GOrigEnum = nullptr;
    static FindFn GOrigFind = nullptr;
    static GetChainFn GOrigGetChain = nullptr;

    static std::mutex GMutex;
    static std::unordered_set<HCERTSTORE> GWrapped; // collections returned
    static std::unordered_map<HCERTSTORE, std::string> GStoreNames; // handle to store name
    static std::unordered_map<HCERTSTORE, long> GEnumCount;         // handle to running count
    static long GFindLogged = 0;
    static long GChainLogged = 0;

    static void RememberName(HCERTSTORE h, const std::string& name)
    {
        if (!h) return;
        std::lock_guard<std::mutex> lock(GMutex);
        GStoreNames[h] = name;
    }

    static std::string NameOf(HCERTSTORE h)
    {
        std::lock_guard<std::mutex> lock(GMutex);
        auto it = GStoreNames.find(h);
        return it == GStoreNames.end() ? std::string("?") : it->second;
    }

    static std::string HexStr(DWORD v)
    {
        static const char* d = "0123456789abcdef";
        if (v == 0) return "0";

        std::string s;
        while (v)
        {
            s.insert(s.begin(), d[v & 0xf]);
            v >>= 4;
        }
        return s;
    }

    // Human-readable provider: predefined providers are low-integer sentinels cast to LPCSTR, anything else is a real provider-name string.
    static std::string ProviderDesc(LPCSTR provider)
    {
        uintptr_t v = reinterpret_cast<uintptr_t>(provider);
        if (v < 0x10000)
        {
            switch (v)
            {
            case 2: return "MEMORY";
            case 9: return "SYSTEM_A";
            case 10: return "SYSTEM_W";
            case 11: return "COLLECTION";
            case 13: return "SYSTEM_REGISTRY_W";
            default: return "prov#" + std::to_string(v);
            }
        }
        return std::string("\"") + provider + "\"";
    }

    static bool NameIsRootOrCaW(LPCWSTR name)
    {
        return name && (_wcsicmp(name, L"root") == 0 || _wcsicmp(name, L"ca") == 0);
    }
    static bool NameIsRootOrCaA(LPCSTR name)
    {
        return name && (_stricmp(name, "root") == 0 || _stricmp(name, "ca") == 0);
    }
    static bool IsSystemProvW(LPCSTR provider)
    {
        return provider == CERT_STORE_PROV_SYSTEM_W; // this is CERT_STORE_PROV_SYSTEM
    }
    static bool IsSystemProvA(LPCSTR provider)
    {
        return provider == CERT_STORE_PROV_SYSTEM_A;
    }

    // Build an in-process collection of the real system store and a memory store holding the CA. Only this process sees it and the on-disk system store is not touched. Consumes 'real', so the collection keeps a reference.
    static HCERTSTORE WrapStore(HCERTSTORE real)
    {
        if (!real || !GetCaCert() || !GOrigOpenStore) return real;

        HCERTSTORE coll = GOrigOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr);
        if (!coll) return real;

        CertAddStoreToCollection(coll, real, 0, 1);
        HCERTSTORE mem = GOrigOpenStore(CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, 0, 0, nullptr);
        if (mem)
        {
            CertAddCertificateContextToStore(mem, GetCaCert(), CERT_STORE_ADD_ALWAYS, nullptr);
            CertAddStoreToCollection(coll, mem, 0, 2);
            CertCloseStore(mem, 0);
        }
        CertCloseStore(real, 0);
        {
            std::lock_guard<std::mutex> lock(GMutex);
            GWrapped.insert(coll);
        }
        return coll;
    }

    static bool AlreadyWrapped(HCERTSTORE store)
    {
        std::lock_guard<std::mutex> lock(GMutex);
        return GWrapped.count(store) != 0;
    }

    static HCERTSTORE WINAPI DetourOpenStore(LPCSTR provider, DWORD enc, HCRYPTPROV_LEGACY prov, DWORD flags, const void* para)
    {
        HCERTSTORE real = GOrigOpenStore(provider, enc, prov, flags, para);

        bool inject = false;
        std::string storeName;
        if (IsSystemProvW(provider) && para)
        {
            LPCWSTR nm = reinterpret_cast<LPCWSTR>(para);
            storeName = NarrowUtf8(nm);
            inject = NameIsRootOrCaW(nm);
        }
        else if (IsSystemProvA(provider) && para)
        {
            LPCSTR nm = reinterpret_cast<LPCSTR>(para);
            storeName = nm;
            inject = NameIsRootOrCaA(nm);
        }

        if (inject && real && !AlreadyWrapped(real))
        {
            HCERTSTORE coll = WrapStore(real);
            RememberName(coll, storeName);
            RememberName(real, storeName);
            LogLine("CertOpenStore: prov=" + ProviderDesc(provider) + " name=" + storeName + " flags=0x" + HexStr(flags) + " handle=" + std::to_string(reinterpret_cast<uintptr_t>(coll)) + " *** injected CA into trust ***");
            return coll;
        }

        if (!storeName.empty())
        {
            RememberName(real, storeName);
            LogLine("CertOpenStore: prov=" + ProviderDesc(provider) + " name=" + storeName + " flags=0x" + HexStr(flags) + " handle=" + std::to_string(reinterpret_cast<uintptr_t>(real)));
        }
        return real;
    }

    static HCERTSTORE WINAPI DetourOpenSysW(HCRYPTPROV_LEGACY prov, LPCWSTR name)
    {
        HCERTSTORE real = GOrigOpenSysW(prov, name);
        if (real && AlreadyWrapped(real)) return real; // internal CertOpenStore already injected

        if (NameIsRootOrCaW(name))
        {
            HCERTSTORE coll = WrapStore(real);
            std::string nm = NarrowUtf8(name);
            RememberName(coll, nm);
            RememberName(real, nm);
            LogLine("CertOpenSystemStoreW: " + nm + " handle=" + std::to_string(reinterpret_cast<uintptr_t>(coll)) + " *** injected CA into trust ***");
            return coll;
        }
        std::string nm = name ? NarrowUtf8(name) : std::string("(null)");
        RememberName(real, nm);
        LogLine("CertOpenSystemStoreW: " + nm + " handle=" + std::to_string(reinterpret_cast<uintptr_t>(real)));
        return real;
    }

    static HCERTSTORE WINAPI DetourOpenSysA(HCRYPTPROV_LEGACY prov, LPCSTR name)
    {
        HCERTSTORE real = GOrigOpenSysA(prov, name);
        if (real && AlreadyWrapped(real)) return real;
    
        if (NameIsRootOrCaA(name))
        {
            HCERTSTORE coll = WrapStore(real);
            RememberName(coll, name ? name : "");
            RememberName(real, name ? name : "");
            LogLine(std::string("CertOpenSystemStoreA: ") + name + " handle=" + std::to_string(reinterpret_cast<uintptr_t>(coll)) + " *** injected CA into trust ***");
            return coll;
        }
        std::string nm = name ? name : "(null)";
        RememberName(real, nm);
        LogLine(std::string("CertOpenSystemStoreA: ") + nm + " handle=" + std::to_string(reinterpret_cast<uintptr_t>(real)));
        return real;
    }

    // Full enumeration of a store is the signature of a native-CA import (curl walks the root store to copy every anchor into OpenSSL's X509_STORE).
    // Log START on the first call and end when enumeration terminates so that the log shows whether/when the game imports the Windows root store and whether the injected CA rode along in a store.
    static PCCERT_CONTEXT WINAPI DetourEnum(HCERTSTORE store, PCCERT_CONTEXT prev)
    {
        if (prev == nullptr)
        {
            bool wrapped = AlreadyWrapped(store);
            LogLine("CertEnumCertificatesInStore START store=" + NameOf(store) + " handle=" + std::to_string(reinterpret_cast<uintptr_t>(store)) + (wrapped ? " (wrapped, CA included)" : ""));
            std::lock_guard<std::mutex> lock(GMutex);
            GEnumCount[store] = 0;
        }
        PCCERT_CONTEXT r = GOrigEnum(store, prev);
        if (r)
        {
            std::lock_guard<std::mutex> lock(GMutex);
            ++GEnumCount[store];
        }
        else
        {
            long n = 0;
            {
                std::lock_guard<std::mutex> lock(GMutex);
                auto it = GEnumCount.find(store);
                if (it != GEnumCount.end())
                {
                    n = it->second;
                    GEnumCount.erase(it);
                }
            }
            if (n > 0)
                LogLine("CertEnumCertificatesInStore END store=" + NameOf(store) + " handle=" + std::to_string(reinterpret_cast<uintptr_t>(store)) + ", enumerated " + std::to_string(n) + " certs" + (AlreadyWrapped(store) ? " (wrapped, CA was in this import)" : " (not wrapped)"));
        }
        return r;
    }

    static PCCERT_CONTEXT WINAPI DetourFind(HCERTSTORE store, DWORD enc, DWORD findFlags, DWORD findType, const void* para, PCCERT_CONTEXT prev)
    {
        if (InterlockedIncrement(&GFindLogged) <= 4)
        {
            LogLine("CertFindCertificateInStore: called (findType=" + std::to_string(findType) + (AlreadyWrapped(store) ? ", on wrapped store)" : ")"));
        }

        return GOrigFind(store, enc, findFlags, findType, para, prev);
    }

    static bool SameName(const CERT_NAME_BLOB& a, const CERT_NAME_BLOB& b)
    {
        return a.cbData == b.cbData && a.cbData > 0 && std::memcmp(a.pbData, b.pbData, a.cbData) == 0;
    }

    // True if the built chain roots in CA so only ever relax trust for cert, never blanket-trust anything else
    static bool ChainRootIsOurs(PCCERT_CHAIN_CONTEXT cc)
    {
        if (!GetCaCert() || !cc || cc->cChain == 0) return false;
        PCERT_SIMPLE_CHAIN sc = cc->rgpChain[0];

        if (!sc || sc->cElement == 0) return false;
        PCCERT_CONTEXT root = sc->rgpElement[sc->cElement - 1]->pCertContext;

        return root && SameName(root->pCertInfo->Subject, GetCaCert()->pCertInfo->Subject);
    }

    // if graph verification unexpectedly routes through the chain engine, clear only the UNTRUSTED_ROOT bit for chains that root in CA.
    static BOOL WINAPI DetourGetChain(HCERTCHAINENGINE engine, PCCERT_CONTEXT cert, LPFILETIME time, HCERTSTORE store, PCERT_CHAIN_PARA para, DWORD flags, LPVOID reserved, PCCERT_CHAIN_CONTEXT* chain)
    {
        if (InterlockedIncrement(&GChainLogged) <= 6) LogLine("CertGetCertificateChain: called");

        BOOL ok = GOrigGetChain(engine, cert, time, store, para, flags, reserved, chain);
        if (ok && chain && *chain)
        {
            PCCERT_CHAIN_CONTEXT cc = *chain;
            if ((cc->TrustStatus.dwErrorStatus & CERT_TRUST_IS_UNTRUSTED_ROOT) && ChainRootIsOurs(cc))
            {
                CERT_CHAIN_CONTEXT* mut = const_cast<CERT_CHAIN_CONTEXT*>(cc);
                mut->TrustStatus.dwErrorStatus &= ~CERT_TRUST_IS_UNTRUSTED_ROOT;
                
                for (DWORD i = 0; i < cc->cChain; ++i)
                {
                    CERT_SIMPLE_CHAIN* sc = const_cast<CERT_SIMPLE_CHAIN*>(cc->rgpChain[i]);
                    sc->TrustStatus.dwErrorStatus &= ~CERT_TRUST_IS_UNTRUSTED_ROOT;
                
                    if (sc->cElement)
                    {
                        CERT_CHAIN_ELEMENT* rootEl = const_cast<CERT_CHAIN_ELEMENT*>(sc->rgpElement[sc->cElement - 1]);
                        rootEl->TrustStatus.dwErrorStatus &= ~CERT_TRUST_IS_UNTRUSTED_ROOT;
                    }
                }
                
                LogLine("CertGetCertificateChain: cleared UNTRUSTED_ROOT for CA rooted chain");
            }
        }
        return ok;
    }

    static bool HookExport(HMODULE mod, const char* name, void* detour, void** orig)
    {
        void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
        if (!target)
        {
            LogLine(std::string("castore: export not found: ") + name);
            return false;
        }

        if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK)
        {
            LogLine(std::string("castore: hook failed: ") + name);
            return false;
        }

        return true;
    }

    bool InstallCaStoreHooks()
    {
        HMODULE crypt32 = GetModuleHandleW(L"crypt32.dll");
        if (!crypt32)
        {
            crypt32 = LoadLibraryW(L"crypt32.dll");
        }

        if (!crypt32)
        {
            LogLine("castore: crypt32.dll not available");
            return false;
        }

        bool ok = true;
        ok &= HookExport(crypt32, "CertOpenStore",
                         reinterpret_cast<void*>(&DetourOpenStore),
                         reinterpret_cast<void**>(&GOrigOpenStore));
        ok &= HookExport(crypt32, "CertOpenSystemStoreW",
                         reinterpret_cast<void*>(&DetourOpenSysW),
                         reinterpret_cast<void**>(&GOrigOpenSysW));
        ok &= HookExport(crypt32, "CertOpenSystemStoreA",
                         reinterpret_cast<void*>(&DetourOpenSysA),
                         reinterpret_cast<void**>(&GOrigOpenSysA));
        ok &= HookExport(crypt32, "CertEnumCertificatesInStore",
                         reinterpret_cast<void*>(&DetourEnum),
                         reinterpret_cast<void**>(&GOrigEnum));
        ok &= HookExport(crypt32, "CertFindCertificateInStore",
                         reinterpret_cast<void*>(&DetourFind),
                         reinterpret_cast<void**>(&GOrigFind));
        ok &= HookExport(crypt32, "CertGetCertificateChain",
                         reinterpret_cast<void*>(&DetourGetChain),
                         reinterpret_cast<void**>(&GOrigGetChain));
        LogLine(std::string("castore: CA-store hooks installed (") + (ok ? "all ok" : "some failed") + "). Injecting CA on system Root/CA store open (case-insensitive).");
        return ok;
    }

}
