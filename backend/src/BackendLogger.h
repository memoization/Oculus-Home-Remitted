#pragma once
#include <string>

namespace home2backend {

class BackendLogger {
    public:
        bool Open(const std::wstring& path);
        bool IsOpen() const { return fileHandle != nullptr; }
        void Write(const std::string& bytes);
        void Close();

    private:
        void* fileHandle = nullptr;
    };

    extern BackendLogger GLog;

    void LogLine(const std::string& line);
    std::string NarrowUtf8(const std::wstring& wide);
    std::string HexU(unsigned long long value);
    std::string FirstLine(const char* data, int length);

}
