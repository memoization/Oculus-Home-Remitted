#include "BackendLogger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace home2backend {

    BackendLogger GLog;

    bool BackendLogger::Open(const std::wstring& path)
    {
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            fileHandle = nullptr;
            return false;
        }
        fileHandle = handle;
        return true;
    }

    void BackendLogger::Write(const std::string& bytes)
    {
        if (!fileHandle) return;

        DWORD written = 0;
        WriteFile(static_cast<HANDLE>(fileHandle), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }

    void BackendLogger::Close()
    {
        if (fileHandle)
        {
            CloseHandle(static_cast<HANDLE>(fileHandle));
            fileHandle = nullptr;
        }
    }

    void LogLine(const std::string& line)
    {
        SYSTEMTIME now;
        GetLocalTime(&now);
        char stamp[32];
        _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "[%02d:%02d:%02d.%03d] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        std::string out = stamp;
        out += line;
        out += "\r\n";
        GLog.Write(out);
        OutputDebugStringA(out.c_str());
    }

    std::string NarrowUtf8(const std::wstring& wide)
    {
        if (wide.empty()) return std::string();

        int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
    
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    std::string HexU(unsigned long long value)
    {
        char buf[20];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%llx", value);
        return buf;
    }

    // Returns the first line only (up to CR/LF), capped at 200 chars. Used so that only the HTTP request LINE is ever logged, never headers/body (no token).
    std::string FirstLine(const char* data, int length)
    {
        std::string out;
        for (int i = 0; i < length && i < 200; ++i)
        {
            char c = data[i];
            if (c == '\r' || c == '\n') break;

            out += c;
        }
        return out;
    }

}
