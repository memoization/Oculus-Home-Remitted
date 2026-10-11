#pragma once
#include <string>
#include <vector>

namespace zip
{
    struct FileEntry
    {
        std::string pathInArchive; // forward-slash relative path stored in the archive
        std::wstring sourcePath; // read the bytes from this file on disk when set
        std::string data; // the bytes to store when sourcePath is empty
    };

    bool CreateArchive(const std::wstring& outPath, const std::vector<FileEntry>& entries);
    bool ExtractArchive(const std::wstring& archivePath, const std::wstring& destDir);
}
