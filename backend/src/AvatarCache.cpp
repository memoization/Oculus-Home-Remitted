#include "Probe.h"
#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <string>
#include <vector>
#include <filesystem>

// Point the Avatar SDK's asset cache folder to the local avatar assets in the store location.
//
// Newer libovravatar / LibOVRPlatform binaries only loads from its local cache at GetTempPath()\Oculus\Avatars
// The SDK checks that cache by file id from the spec, or from a node-to-file resolve
//
// store\avatar-assets already holds exactly all the <fileId>.tex / <fileId>.mesh files, so instead of
// copying 1GB+ into the temp cache, make the cache directory a junction to the "avatar-assets" store.

namespace home2backend {

    // Create a directory junction at link pointing to target.
    static bool CreateJunction(const std::wstring& link, const std::wstring& target)
    {
        if (!CreateDirectoryW(link.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;

        HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        std::wstring subst = L"\\??\\" + target;// the "substitute" (NT) name
        const std::wstring& print = target;// the display name

        size_t substBytes = subst.size() * sizeof(wchar_t);
        size_t printBytes = print.size() * sizeof(wchar_t);

        // Noted layout: ReparseTag(4) ReparseDataLength(2) Reserved(2) | SubstOff(2) SubstLen(2) PrintOff(2) PrintLen(2) | PathBuffer
        const size_t kHeader = 8;
        const size_t kMountInfo = 8;
        size_t pathBytes = substBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
        size_t total = kHeader + kMountInfo + pathBytes;

        std::vector<BYTE> buf(total, 0);
        BYTE* p = buf.data();
        *reinterpret_cast<DWORD*>(p + 0) = IO_REPARSE_TAG_MOUNT_POINT;
        *reinterpret_cast<WORD*>(p + 4) = static_cast<WORD>(kMountInfo + pathBytes); // ReparseDataLength?
        WORD substOff = 0;
        WORD substLen = static_cast<WORD>(substBytes);
        WORD printOff = static_cast<WORD>(substBytes + sizeof(wchar_t));
        WORD printLen = static_cast<WORD>(printBytes);
        *reinterpret_cast<WORD*>(p + 8) = substOff;
        *reinterpret_cast<WORD*>(p + 10) = substLen;
        *reinterpret_cast<WORD*>(p + 12) = printOff;
        *reinterpret_cast<WORD*>(p + 14) = printLen;
        BYTE* pathBuf = p + 16;
        memcpy(pathBuf + substOff, subst.c_str(), substBytes + sizeof(wchar_t));
        memcpy(pathBuf + printOff, print.c_str(), printBytes + sizeof(wchar_t));

        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(total), nullptr, 0, &ret, nullptr);
        CloseHandle(h);
        
        if (!ok)
        {
            RemoveDirectoryW(link.c_str());
            return false;
        }
        return true;
    }

    void EnsureAvatarCacheJunction(const std::wstring& assetsDir)
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        // Absolute target
        wchar_t absBuf[MAX_PATH] = { 0 };
        if (GetFullPathNameW(assetsDir.c_str(), MAX_PATH, absBuf, nullptr) == 0)
        {
            LogLine("avatarcache: could not resolve avatar-assets path, cache junction skipped");
            return;
        }
        std::wstring target(absBuf);
        while (!target.empty() && (target.back() == L'\\' || target.back() == L'/')) 
        {
            target.pop_back();
        }

        DWORD ta = GetFileAttributesW(target.c_str());
        if (ta == INVALID_FILE_ATTRIBUTES || !(ta & FILE_ATTRIBUTE_DIRECTORY))
        {
            LogLine("avatarcache: store\\avatar-assets missing at " + NarrowUtf8(target) + ", cache junction skipped");
            return;
        }

        // Target cache path: "GetTempPath()\Oculus\Avatars"
        wchar_t tmp[MAX_PATH] = { 0 };
        if (GetTempPathW(MAX_PATH, tmp) == 0)
        {
            LogLine("avatarcache: GetTempPath failed, cache junction skipped");
            return;
        }
        std::wstring oculusDir = std::wstring(tmp) + L"Oculus";
        std::wstring cache = oculusDir + L"\\Avatars";
        CreateDirectoryW(oculusDir.c_str(), nullptr); // ensure the parent exists

        DWORD attr = GetFileAttributesW(cache.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            // Already a reparse point. Drop it and recreate so it always tracks the current store of this session.
            // RemoveDirectoryW deletes only the junction, never the target's contents.
            RemoveDirectoryW(cache.c_str());
        }
        else if (attr != INVALID_FILE_ATTRIBUTES)
        {
            // Preserve an existing directory aside once, otherwise remove it, so the junction can take its place.
            std::wstring bak = cache + L".orig";
            if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES && MoveFileExW(cache.c_str(), bak.c_str(), 0))
            {
                LogLine("avatarcache: moved existing SDK Avatars cache aside to Avatars.orig");
            }
            else
            {
                fs::remove_all(fs::path(cache), ec); // fallback: clear junction
            }
        }

        if (CreateJunction(cache, target))
        {
            LogLine("avatarcache: cache junction ready, " + NarrowUtf8(cache) + " -> " + NarrowUtf8(target));
        }  
        else
        {
            LogLine("avatarcache: Failed to create cache junction (err " + std::to_string(GetLastError()) + "), avatar assets may not load");
        }
    }

}
