#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Zip.h"

#include <windows.h>
#include <filesystem>
#include <cstring>

#include "miniz.h"

// A zip reader and writer powered by miniz library. Important key is zip64 on its own once a size, an offset, or the count crosses the 32-bit limits, so a small archive stays plain zip and a large one still works.
// miniz opens files through _wfopen after a CP_UTF8 widen, so every path handed to it is converted to utf-8 first and Unicode paths work.
namespace zip
{
    namespace fs = std::filesystem;

    static std::string wideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();

        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();

        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }

    static std::wstring utf8ToWide(const char* s)
    {
        if (!s || !*s) return std::wstring();

        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (n <= 0) return std::wstring();

        std::wstring w(static_cast<size_t>(n - 1), L'\0'); // n counts the terminator
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
        return w;
    }

    // Turn an archive name into a safe relative path under the destination
    static bool sanitizeRel(const std::wstring& name, fs::path& rel)
    {
        if (name.empty()) return false;

        std::wstring s = name;
        for (auto& c : s)
        {
            if (c == L'\\')
            {
                c = L'/';
            }
        }

        if (s.front() == L'/') return false;
        if (s.size() >= 2 && s[1] == L':') return false;

        rel.clear();
        size_t start = 0;
        while (start < s.size())
        {
            size_t slash = s.find(L'/', start);
            std::wstring part = (slash == std::wstring::npos) ? s.substr(start) : s.substr(start, slash - start);

            if (part == L"..") return false;
            if (!part.empty() && part != L".") rel /= part;

            if (slash == std::wstring::npos) break;
            start = slash + 1;
        }

        return !rel.empty();
    }

    bool CreateArchive(const std::wstring& outPath, const std::vector<FileEntry>& entries)
    {
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));

        std::string outUtf8 = wideToUtf8(outPath);

        // setting flags 0 lets miniz keep the plain 32-bit format and promote to zip64 per member or for the end record only when a size requires it.
        if (!mz_zip_writer_init_file_v2(&zip, outUtf8.c_str(), 0, 0))
        {
            return false;
        }

        bool result = true;
        for (const auto& e : entries)
        {
            if (!e.sourcePath.empty())
            {
                std::string srcUtf8 = wideToUtf8(e.sourcePath);
                result = mz_zip_writer_add_file(&zip, e.pathInArchive.c_str(), srcUtf8.c_str(), nullptr, 0, MZ_DEFAULT_LEVEL);
            }
            else
            {
                result = mz_zip_writer_add_mem(&zip, e.pathInArchive.c_str(), e.data.data(), e.data.size(), MZ_DEFAULT_LEVEL);
            }

            if (!result) break;
        }

        if (result)
        {
            result = mz_zip_writer_finalize_archive(&zip);
        }
        mz_zip_writer_end(&zip);

        if (!result)
        {
            DeleteFileW(outPath.c_str()); // do not leave a half-written archive
        }
        return result;
    }

    bool ExtractArchive(const std::wstring& archivePath, const std::wstring& destDir)
    {
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));

        std::string arcUtf8 = wideToUtf8(archivePath);
        if (!mz_zip_reader_init_file(&zip, arcUtf8.c_str(), 0))
        {
            return false;
        }

        std::error_code ec;
        fs::create_directories(destDir, ec);

        bool result = true;
        mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i)
        {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st))
            {
                result = false;
                break;
            }

            fs::path rel;
            if (!sanitizeRel(utf8ToWide(st.m_filename), rel))
            {
                result = false;
                break;
            }
            fs::path full = fs::path(destDir) / rel;

            if (mz_zip_reader_is_file_a_directory(&zip, i))
            {
                fs::create_directories(full, ec);
                continue;
            }

            fs::create_directories(full.parent_path(), ec); // miniz will not make missing parents

            std::string dstUtf8 = wideToUtf8(full.wstring());
            if (!mz_zip_reader_extract_to_file(&zip, i, dstUtf8.c_str(), 0))
            {
                result = false;
                break;
            }
        }

        mz_zip_reader_end(&zip);
        return result;
    }
}
