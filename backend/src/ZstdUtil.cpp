#include "ZstdUtil.h"

#include <cstdint>

namespace home2hook {

    bool IsZstdFrame(const std::string& data)
    {
        return data.size() >= 4
            && static_cast<unsigned char>(data[0]) == 0x28
            && static_cast<unsigned char>(data[1]) == 0xB5
            && static_cast<unsigned char>(data[2]) == 0x2F
            && static_cast<unsigned char>(data[3]) == 0xFD;
    }

    std::string ZstdWrapRaw(const std::string& data)
    {
        std::string out;
        out.reserve(data.size() + 32);

        // Frame magic number, little endian 0xFD2FB528
        const unsigned char magic[4] = { 0x28, 0xB5, 0x2F, 0xFD };
        out.append(reinterpret_cast<const char*>(magic), 4);

        // Frame header descriptor. Frame_Content_Size_flag 2 means a 4 byte content size.
        // Single_Segment_flag 1 drops the window descriptor and sets the window to the content size.
        out.push_back(static_cast<char>(0xA0));

        // Frame content size, 4 bytes little endian, the exact decompressed length
        uint32_t total = static_cast<uint32_t>(data.size());
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<char>((total >> (8 * i)) & 0xFF));
        }

        // Each block has a 3 byte little endian header: (size shifted left 3) or (type shifted left 1) or last.
        // Block type 0 is a raw block, so the payload bytes follow verbatim.
        // The raw block maximum is 128 KB, so the input is chunked at that size.
        const size_t kMaxBlock = 128 * 1024;
        size_t n = data.size();

        if (n == 0)
        {
            uint32_t hdr = (0u << 3) | 1u; // one last raw block of size 0
            for (int i = 0; i < 3; ++i)
            {
                out.push_back(static_cast<char>((hdr >> (8 * i)) & 0xFF));
            }
            return out;
        }

        size_t off = 0;
        while (off < n)
        {
            size_t chunk = n - off;
            if (chunk > kMaxBlock) chunk = kMaxBlock;

            uint32_t last = (off + chunk >= n) ? 1u : 0u;
            uint32_t hdr = (static_cast<uint32_t>(chunk) << 3) | (0u << 1) | last;
            for (int i = 0; i < 3; ++i)
            {
                out.push_back(static_cast<char>((hdr >> (8 * i)) & 0xFF));
            }

            out.append(data, off, chunk);
            off += chunk;
        }
        return out;
    }

}
