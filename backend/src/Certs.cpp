#include "Certs.h"
#include "BackendLogger.h"

#include <string>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace home2backend {

    static PCCERT_CONTEXT GCa = nullptr;
    static PCCERT_CONTEXT GLeaf = nullptr;

    static const wchar_t* kProv = MS_ENH_RSA_AES_PROV_W;
    static const wchar_t* kCaContainer = L"home2probe_ca_container";
    static const wchar_t* kLeafContainer = L"home2probe_leaf_container";

    static bool EncodeName(const wchar_t* nameStr, std::vector<BYTE>& out)
    {
        DWORD cb = 0;
        if (!CertStrToNameW(X509_ASN_ENCODING, nameStr, CERT_X500_NAME_STR, nullptr, nullptr, &cb, nullptr))
        {
            return false;
        }

        out.resize(cb);
        return CertStrToNameW(X509_ASN_ENCODING, nameStr, CERT_X500_NAME_STR, nullptr, out.data(), &cb, nullptr) != FALSE;
    }

    // Ensure a persisted key container exists with a key of keySpec, and return the prov.
    static HCRYPTPROV EnsureContainer(const wchar_t* container, DWORD keySpec)
    {
        HCRYPTPROV prov = 0;
        if (!CryptAcquireContextW(&prov, container, kProv, PROV_RSA_AES, 0))
        {
            if (!CryptAcquireContextW(&prov, container, kProv, PROV_RSA_AES, CRYPT_NEWKEYSET))
            {
                LogLine("certs: CryptAcquireContext(" + NarrowUtf8(container) + ") failed err=" + std::to_string(GetLastError()));
                return 0;
            }
        }
        HCRYPTKEY key = 0;
        if (!CryptGetUserKey(prov, keySpec, &key))
        {
            if (!CryptGenKey(prov, keySpec, (2048 << 16) | CRYPT_EXPORTABLE, &key))
            {
                LogLine("certs: CryptGenKey failed err=" + std::to_string(GetLastError()));
                CryptReleaseContext(prov, 0);
                return 0;
            }
        }
        if (key) CryptDestroyKey(key);
        return prov;
    }

    static bool EncodeObject(LPCSTR structType, const void* structInfo, std::vector<BYTE>& out)
    {
        DWORD cb = 0;
        if (!CryptEncodeObject(X509_ASN_ENCODING, structType, structInfo, nullptr, &cb)) return false;
        out.resize(cb);

        return CryptEncodeObject(X509_ASN_ENCODING, structType, structInfo, out.data(), &cb) != FALSE;
    }

    static PCCERT_CONTEXT MakeCa()
    {
        std::vector<BYTE> subject;
        if (!EncodeName(L"CN=home2probe Root CA", subject)) return nullptr;

        CERT_NAME_BLOB subjectBlob = {static_cast<DWORD>(subject.size()), subject.data()};

        HCRYPTPROV prov = EnsureContainer(kCaContainer, AT_SIGNATURE);
        if (!prov) return nullptr;

        CryptReleaseContext(prov, 0);

        // basicConstraints: CA=TRUE (critical)
        CERT_BASIC_CONSTRAINTS2_INFO bc;
        ZeroMemory(&bc, sizeof(bc));
        bc.fCA = TRUE;
        std::vector<BYTE> bcEnc;

        if (!EncodeObject(X509_BASIC_CONSTRAINTS2, &bc, bcEnc)) return nullptr;

        CERT_EXTENSION ext;
        ext.pszObjId = const_cast<LPSTR>(szOID_BASIC_CONSTRAINTS2);
        ext.fCritical = TRUE;
        ext.Value.cbData = static_cast<DWORD>(bcEnc.size());
        ext.Value.pbData = bcEnc.data();
        CERT_EXTENSIONS exts = {1, &ext};

        CRYPT_KEY_PROV_INFO kpi;
        ZeroMemory(&kpi, sizeof(kpi));
        kpi.pwszContainerName = const_cast<LPWSTR>(kCaContainer);
        kpi.pwszProvName = const_cast<LPWSTR>(kProv);
        kpi.dwProvType = PROV_RSA_AES;
        kpi.dwKeySpec = AT_SIGNATURE;

        CRYPT_ALGORITHM_IDENTIFIER algo;
        ZeroMemory(&algo, sizeof(algo));
        algo.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

        PCCERT_CONTEXT ca = CertCreateSelfSignCertificate(0, &subjectBlob, 0, &kpi, &algo, nullptr, nullptr, &exts);

        if (!ca) LogLine("certs: CA CertCreateSelfSignCertificate failed err=" + std::to_string(GetLastError()));
        return ca;
    }

    static PCCERT_CONTEXT MakeLeaf(PCCERT_CONTEXT ca)
    {
        HCRYPTPROV leafProv = EnsureContainer(kLeafContainer, AT_KEYEXCHANGE);
        if (!leafProv) return nullptr;

        DWORD cbPk = 0;
        CryptExportPublicKeyInfo(leafProv, AT_KEYEXCHANGE, X509_ASN_ENCODING, nullptr, &cbPk);
        std::vector<BYTE> pkBuf(cbPk);
        CERT_PUBLIC_KEY_INFO* pubKey = reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(pkBuf.data());
        if (!CryptExportPublicKeyInfo(leafProv, AT_KEYEXCHANGE, X509_ASN_ENCODING, pubKey, &cbPk))
        {
            LogLine("certs: CryptExportPublicKeyInfo failed err=" + std::to_string(GetLastError()));
            CryptReleaseContext(leafProv, 0);
            return nullptr;
        }

        std::vector<BYTE> subject;
        EncodeName(L"CN=graph.oculus.com", subject);
        CERT_NAME_BLOB subjectBlob = {static_cast<DWORD>(subject.size()), subject.data()};

        // SubjectAltName: DNS=graph.oculus.com
        CERT_ALT_NAME_ENTRY altEntry;
        ZeroMemory(&altEntry, sizeof(altEntry));
        altEntry.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
        altEntry.pwszDNSName = const_cast<LPWSTR>(L"graph.oculus.com");
        CERT_ALT_NAME_INFO altInfo = {1, &altEntry};
        std::vector<BYTE> sanEnc;
        EncodeObject(szOID_SUBJECT_ALT_NAME2, &altInfo, sanEnc);

        // EKU: serverAuth
        LPSTR ekuOid = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
        CERT_ENHKEY_USAGE eku = {1, &ekuOid};
        std::vector<BYTE> ekuEnc;
        EncodeObject(szOID_ENHANCED_KEY_USAGE, &eku, ekuEnc);

        CERT_EXTENSION leafExt[2];
        leafExt[0].pszObjId = const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2);
        leafExt[0].fCritical = FALSE;
        leafExt[0].Value.cbData = static_cast<DWORD>(sanEnc.size());
        leafExt[0].Value.pbData = sanEnc.data();
        leafExt[1].pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
        leafExt[1].fCritical = FALSE;
        leafExt[1].Value.cbData = static_cast<DWORD>(ekuEnc.size());
        leafExt[1].Value.pbData = ekuEnc.data();

        HCRYPTPROV caProv = 0;
        if (!CryptAcquireContextW(&caProv, kCaContainer, kProv, PROV_RSA_AES, 0))
        {
            LogLine("certs: reacquire CA container failed err=" + std::to_string(GetLastError()));
            CryptReleaseContext(leafProv, 0);
            return nullptr;
        }

        BYTE serial[16];
        CryptGenRandom(caProv, sizeof(serial), serial);
        serial[0] &= 0x7f; // positive

        CERT_INFO ci;
        ZeroMemory(&ci, sizeof(ci));
        ci.dwVersion = CERT_V3;
        ci.SerialNumber.cbData = sizeof(serial);
        ci.SerialNumber.pbData = serial;
        ci.SignatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);
        ci.Issuer = ca->pCertInfo->Subject;

        SYSTEMTIME nowSt;
        GetSystemTime(&nowSt);
        FILETIME start;
        SystemTimeToFileTime(&nowSt, &start);
        ULARGE_INTEGER u;
        u.LowPart = start.dwLowDateTime;
        u.HighPart = start.dwHighDateTime;
        u.QuadPart -= 24ULL * 3600 * 10000000ULL; // -1 day skew
        start.dwLowDateTime = u.LowPart;
        start.dwHighDateTime = u.HighPart;
        ULARGE_INTEGER e = u;
        e.QuadPart += 5ULL * 365 * 24 * 3600 * 10000000ULL; // +5 years
        FILETIME end;
        end.dwLowDateTime = e.LowPart;
        end.dwHighDateTime = e.HighPart;
        ci.NotBefore = start;
        ci.NotAfter = end;
        ci.Subject = subjectBlob;
        ci.SubjectPublicKeyInfo = *pubKey;
        ci.cExtension = 2;
        ci.rgExtension = leafExt;

        CRYPT_ALGORITHM_IDENTIFIER sigAlgo;
        ZeroMemory(&sigAlgo, sizeof(sigAlgo));
        sigAlgo.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

        DWORD cbEncoded = 0;
        CryptSignAndEncodeCertificate(caProv, AT_SIGNATURE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &ci, &sigAlgo, nullptr, nullptr, &cbEncoded);
        std::vector<BYTE> encoded(cbEncoded);

        BOOL signedOk = CryptSignAndEncodeCertificate(caProv, AT_SIGNATURE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &ci, &sigAlgo, nullptr, encoded.data(), &cbEncoded);
        CryptReleaseContext(caProv, 0);
        CryptReleaseContext(leafProv, 0);
        if (!signedOk)
        {
            LogLine("certs: CryptSignAndEncodeCertificate failed err=" + std::to_string(GetLastError()));
            return nullptr;
        }

        PCCERT_CONTEXT leaf = CertCreateCertificateContext(X509_ASN_ENCODING, encoded.data(), cbEncoded);
        if (!leaf)
        {
            LogLine("certs: CertCreateCertificateContext(leaf) failed");
            return nullptr;
        }

        CRYPT_KEY_PROV_INFO leafKpi;
        ZeroMemory(&leafKpi, sizeof(leafKpi));
        leafKpi.pwszContainerName = const_cast<LPWSTR>(kLeafContainer);
        leafKpi.pwszProvName = const_cast<LPWSTR>(kProv);
        leafKpi.dwProvType = PROV_RSA_AES;
        leafKpi.dwKeySpec = AT_KEYEXCHANGE;
        CertSetCertificateContextProperty(leaf, CERT_KEY_PROV_INFO_PROP_ID, 0, &leafKpi);
        return leaf;
    }

    bool GenerateCerts()
    {
        GCa = MakeCa();
        if (!GCa) return false;

        GLeaf = MakeLeaf(GCa);
        if (!GLeaf) return false;

        LogLine("certs: own root CA and leaf CN=graph.oculus.com (leaf signed by CA) ready");
        return true;
    }

    PCCERT_CONTEXT GetLeafCert() { return GLeaf; }
    PCCERT_CONTEXT GetCaCert() { return GCa; }

    bool ExportCaPem(const std::wstring& path)
    {
        if (!GCa) return false;

        DWORD cch = 0;
        if (!CryptBinaryToStringA(GCa->pbCertEncoded, GCa->cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &cch)) return false;

        std::string pem(cch, '\0');
        if (!CryptBinaryToStringA(GCa->pbCertEncoded, GCa->cbCertEncoded, CRYPT_STRING_BASE64HEADER, &pem[0], &cch)) return false;

        // CryptBinaryToStringA writes "-----BEGIN CERTIFICATE-----" headers already.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            LogLine("certs: ExportCaPem CreateFile failed err=" + std::to_string(GetLastError()));
            return false;
        }

        DWORD written = 0;
        // cch includes the trailing NUL, so write only the text bytes.
        DWORD toWrite = cch > 0 ? cch - 1 : 0;
        BOOL ok = WriteFile(h, pem.data(), toWrite, &written, nullptr);
        CloseHandle(h);

        if (!ok || written != toWrite)
        {
            LogLine("certs: ExportCaPem WriteFile failed err=" + std::to_string(GetLastError()));
            return false;
        }
        return true;
    }

}
