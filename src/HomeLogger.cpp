#include "HomeLogger.h"
#include <functional>
#include <iomanip>
#include <mutex>


HomeLogger homeLogger;

void HomeLogger::open(const std::string& path)
{
    stream_.open(path);
}

void HomeLogger::flush()
{
    stream_.flush();
}

void HomeLogger::close()
{
    stream_.close();
}

std::wofstream& HomeLogger::write()
{
    if (stream_.fail())
    {
        stream_.clear();
    }

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
    localtime_s(&tm, &t);

    stream_ << std::put_time(&tm, L"%m-%d %X") << L" | ";

    return stream_;
}