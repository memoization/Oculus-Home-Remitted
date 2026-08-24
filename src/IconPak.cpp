#include "IconPak.h"
#include "IconPak_asset.h"
#include "../dependencies/picopng/picopng.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace iconpak {

    using std::vector;
    using u8 = unsigned char;

    struct Image { int w = 0, h = 0; vector<u8> rgba; }; // 8-bit RGBA

    static bool LoadPng(const std::string& path, Image& out, std::string& err)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        { 
           err = "cannot open image file: " + path;
           return false;
        }

        f.seekg(0, std::ios::end);
        std::streamoff sz = f.tellg();
        f.seekg(0);
        if (sz <= 0)
        {
            err = "image file is empty";
            return false;
        }
        vector<u8> buf(static_cast<size_t>(sz));

        if (!f.read(reinterpret_cast<char*>(buf.data()), sz))
        {
            err = "failed reading image file";
            return false;
        }

        unsigned long w = 0, h = 0;
        if (decodePNG(out.rgba, w, h, buf.data(), buf.size(), true) != 0)
        {
            err = "could not decode image (a PNG is required)";
            return false;
        }

        out.w = static_cast<int>(w);
        out.h = static_cast<int>(h);
        if (out.w <= 0 || out.h <= 0)
        {
            err = "image has invalid dimensions";
            return false;
        }

        return true;
    }

    // Center-crop to a square, then bilinear-resize to size x size.
    static Image CropResizeSquare(const Image& in, int size)
    {
        int side = std::min(in.w, in.h);
        int ox = (in.w - side) / 2;
        int oy = (in.h - side) / 2;
        Image out;
        out.w = size;
        out.h = size;
        out.rgba.resize(static_cast<size_t>(size) * size * 4);

        for (int y = 0; y < size; ++y)
        {
            float sy = (y + 0.5f) * side / size - 0.5f + oy;
            int y0 = static_cast<int>(std::floor(sy));
            float fy = sy - y0;
            int y0c = std::min(std::max(y0, 0), in.h - 1);
            int y1c = std::min(y0 + 1, in.h - 1);
            for (int x = 0; x < size; ++x)
            {
                float sx = (x + 0.5f) * side / size - 0.5f + ox;
                int x0 = static_cast<int>(std::floor(sx));
                float fx = sx - x0;
                int x0c = std::min(std::max(x0, 0), in.w - 1);
                int x1c = std::min(x0 + 1, in.w - 1);
                for (int c = 0; c < 4; ++c)
                {
                    float p00 = in.rgba[(static_cast<size_t>(y0c) * in.w + x0c) * 4 + c];
                    float p10 = in.rgba[(static_cast<size_t>(y0c) * in.w + x1c) * 4 + c];
                    float p01 = in.rgba[(static_cast<size_t>(y1c) * in.w + x0c) * 4 + c];
                    float p11 = in.rgba[(static_cast<size_t>(y1c) * in.w + x1c) * 4 + c];
                    float top = p00 + (p10 - p00) * fx;
                    float bot = p01 + (p11 - p01) * fx;
                    float v = top + (bot - top) * fy;
                    out.rgba[(static_cast<size_t>(y) * size + x) * 4 + c] = static_cast<u8>(std::min(255.0f, std::max(0.0f, v + 0.5f)));
                }
            }
        }
        return out;
    }

    // Box-downsample by 2 (min 1px) for the mip chain.
    static Image Halve(const Image& in)
    {
        int w = std::max(1, in.w / 2), h = std::max(1, in.h / 2);
        Image o; o.w = w; o.h = h; o.rgba.resize(static_cast<size_t>(w) * h * 4);

        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                for (int c = 0; c < 4; ++c)
                {
                    int x0 = std::min(x * 2, in.w - 1), x1 = std::min(x * 2 + 1, in.w - 1);
                    int y0 = std::min(y * 2, in.h - 1), y1 = std::min(y * 2 + 1, in.h - 1);
                    int s = in.rgba[(static_cast<size_t>(y0) * in.w + x0) * 4 + c]
                          + in.rgba[(static_cast<size_t>(y0) * in.w + x1) * 4 + c]
                          + in.rgba[(static_cast<size_t>(y1) * in.w + x0) * 4 + c]
                          + in.rgba[(static_cast<size_t>(y1) * in.w + x1) * 4 + c];
                    o.rgba[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<u8>((s + 2) / 4);
                }
            }
        }

        return o;
    }

    // --- DXT1 (BC1) ---
    static inline uint16_t Pack565(int r, int g, int b)
    {
        return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    static inline void Unpack565(uint16_t c, int& r, int& g, int& b)
    {
        r = ((c >> 11) & 0x1f) * 255 / 31;
        g = ((c >> 5) & 0x3f) * 255 / 63;
        b = (c & 0x1f) * 255 / 31;
    }

    // Encode one 4x4 block (BC1). Pixels with alpha below 128 are treated as fully transparent, which
    // forces the 3-color transparent BC1 mode where c0 is not greater than c1. Fully-opaque blocks use 4-color mode.
    static void EncodeBlockDXT1(const u8 px[16][4], u8* out)
    {
        int minr = 255, ming = 255, minb = 255, maxr = 0, maxg = 0, maxb = 0, nOpaque = 0;
        bool anyTrans = false;
        for (int i = 0; i < 16; ++i)
        {
            if (px[i][3] < 128)
            { 
                anyTrans = true;
                continue;
            }

            ++nOpaque;
            minr = std::min(minr, (int)px[i][0]); ming = std::min(ming, (int)px[i][1]); minb = std::min(minb, (int)px[i][2]);
            maxr = std::max(maxr, (int)px[i][0]); maxg = std::max(maxg, (int)px[i][1]); maxb = std::max(maxb, (int)px[i][2]);
        }
        if (nOpaque == 0)
        {
            // Fully transparent: c0 not greater than c1 (alpha mode), every pixel is index 3 (transparent).
            out[0] = out[1] = out[2] = out[3] = 0;
            out[4] = out[5] = out[6] = out[7] = 0xFF;
            return;
        }

        uint16_t c0 = Pack565(maxr, maxg, maxb); // brighter endpoint
        uint16_t c1 = Pack565(minr, ming, minb); // darker endpoint
        bool alphaMode = anyTrans;
        if (alphaMode)
        {
            if (c0 > c1) std::swap(c0, c1); // 3-color alpha mode needs c0 not greater than c1
        }
        else
        {
            if (c0 < c1) std::swap(c0, c1);
            if (c0 == c1) { if (c0 > 0) c1 = c0 - 1; else c0 = 1; } // 4-color mode needs c0 greater than c1
        }

        int R[4], G[4], B[4];
        Unpack565(c0, R[0], G[0], B[0]);
        Unpack565(c1, R[1], G[1], B[1]);
        int lim;
        if (alphaMode)
        {
            R[2] = (R[0] + R[1]) / 2; G[2] = (G[0] + G[1]) / 2; B[2] = (B[0] + B[1]) / 2;
            lim = 3; // index 3 means transparent
        }
        else
        {
            R[2] = (2 * R[0] + R[1]) / 3; G[2] = (2 * G[0] + G[1]) / 3; B[2] = (2 * B[0] + B[1]) / 3;
            R[3] = (R[0] + 2 * R[1]) / 3; G[3] = (G[0] + 2 * G[1]) / 3; B[3] = (B[0] + 2 * B[1]) / 3;
            lim = 4;
        }

        uint32_t indices = 0;
        for (int i = 0; i < 16; ++i)
        {
            int id;
            if (alphaMode && px[i][3] < 128)
            {
                id = 3;
            }
            else
            {
                int best = 0, bestD = INT_MAX;
                for (int k = 0; k < lim; ++k)
                {
                    int dr = px[i][0] - R[k], dg = px[i][1] - G[k], db = px[i][2] - B[k];
                    int d = dr * dr + dg * dg + db * db;
                    
                    if (d < bestD)
                    {
                        bestD = d;
                        best = k;
                    }
                }
                id = best;
            }
            indices |= static_cast<uint32_t>(id) << (2 * i);
        }
        out[0] = c0 & 0xFF;
        out[1] = (c0 >> 8) & 0xFF;
        out[2] = c1 & 0xFF;
        out[3] = (c1 >> 8) & 0xFF;
        out[4] = indices & 0xFF;
        out[5] = (indices >> 8) & 0xFF;
        out[6] = (indices >> 16) & 0xFF;
        out[7] = (indices >> 24) & 0xFF;
    }

    static vector<u8> EncodeDXT1(const Image& img)
    {
        int bw = (img.w + 3) / 4, bh = (img.h + 3) / 4;
        vector<u8> out(static_cast<size_t>(bw) * bh * 8);
        for (int by = 0; by < bh; ++by)
        {
            for (int bx = 0; bx < bw; ++bx)
            {
                u8 block[16][4];
                for (int j = 0; j < 4; ++j)
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        int sx = std::min(bx * 4 + i, img.w - 1);
                        int sy = std::min(by * 4 + j, img.h - 1);
                        const u8* p = &img.rgba[(static_cast<size_t>(sy) * img.w + sx) * 4];
                        block[j * 4 + i][0] = p[0];
                        block[j * 4 + i][1] = p[1];
                        block[j * 4 + i][2] = p[2];
                        block[j * 4 + i][3] = p[3];
                    }
                }

                EncodeBlockDXT1(block, &out[(static_cast<size_t>(by) * bw + bx) * 8]);
            }
        }

        return out;
    }

    // ------------- building the pak ----
    static void Sha1(const u8* data, size_t len, u8 out[20])
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE h = nullptr;
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
        BCryptHashData(h, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0);
        BCryptFinishHash(h, out, 20, 0);
        BCryptDestroyHash(h);
        BCryptCloseAlgorithmProvider(alg, 0);
    }

    template <typename T> static void Put(vector<u8>& b, T v)
    {
        for (size_t i = 0; i < sizeof(T); ++i)
        {
            b.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
        }
    }

    static void PutFString(vector<u8>& b, const std::string& s)
    {
        Put<int32_t>(b, static_cast<int32_t>(s.size()) + 1); // positive length incl. null = ASCII
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }

    static void PutEntry(vector<u8>& b, uint64_t off, uint64_t size, const u8 sha[20])
    {
        Put<uint64_t>(b, off); Put<uint64_t>(b, size);
        Put<uint64_t>(b, size); Put<uint32_t>(b, 0);
        b.insert(b.end(), sha, sha + 20);
        b.push_back(0);              // bEncrypted
        Put<uint32_t>(b, 0);         // compression block size
    }

    struct PakFile { std::string name; const vector<u8>* data; };

    static bool WritePak(const std::wstring& path, const std::string& mount, const vector<PakFile>& files)
    {
        const uint32_t kMagic = 0x5A6F12E1u;
        vector<u8> body;
        struct Rec { std::string name; uint64_t off; uint64_t size; u8 sha[20]; };
        vector<Rec> recs;
        for (const auto& f : files)
        {
            uint64_t off = body.size();
            u8 sha[20]; Sha1(f.data->data(), f.data->size(), sha);
            PutEntry(body, 0, f.data->size(), sha);          // in-body header uses offset 0
            body.insert(body.end(), f.data->begin(), f.data->end());
            Rec r; r.name = f.name; r.off = off; r.size = f.data->size(); std::memcpy(r.sha, sha, 20);
            recs.push_back(r);
        }

        vector<u8> index;
        PutFString(index, mount);
        Put<uint32_t>(index, static_cast<uint32_t>(recs.size()));
        for (const auto& r : recs)
        {
            PutFString(index, r.name);
            PutEntry(index, r.off, r.size, r.sha);
        }

        uint64_t indexOff = body.size();
        u8 idxSha[20]; Sha1(index.data(), index.size(), idxSha);
        vector<u8> footer;
        footer.insert(footer.end(), 16, 0);   // EncryptionKeyGuid
        footer.push_back(0);                  // bEncryptedIndex
        Put<uint32_t>(footer, kMagic);
        Put<uint32_t>(footer, 5);             // version
        Put<uint64_t>(footer, indexOff);
        Put<uint64_t>(footer, index.size());
        footer.insert(footer.end(), idxSha, idxSha + 20);

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(body.data()), body.size());
        out.write(reinterpret_cast<const char*>(index.data()), index.size());
        out.write(reinterpret_cast<const char*>(footer.data()), footer.size());
        return static_cast<bool>(out);
    }

    static std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return L"";

        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
        
        if (n > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        }
        return w;
    }
    static std::string NarrowW(const std::wstring& s)
    {
        if (s.empty()) return "";

        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string a(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &a[0], n, nullptr, nullptr);
        }

        return a;
    }

    bool BuildProfilePak(const std::string& home2ExePath, const std::string& imagePath, std::string& errOut)
    {
        if (home2ExePath.empty())
        {
            errOut = "Set the \"Home2-Win64-Shipping.exe\" executable first.";
            return false;
        }

        // <exe dir>\..\..\Content\Paks  (exe is ...\Home2\Binaries\Win64\Home2-Win64-Shipping.exe)
        std::wstring exeW = Widen(home2ExePath);
        size_t slash = exeW.find_last_of(L"\\/");
        std::wstring exeDir = (slash == std::wstring::npos) ? L"." : exeW.substr(0, slash);
        std::wstring raw = exeDir + L"\\..\\..\\Content\\Paks";
        wchar_t full[MAX_PATH * 2] = {0};
        
        if (GetFullPathNameW(raw.c_str(), MAX_PATH * 2, full, nullptr) == 0)
        {
            errOut = "could not resolve the Content\\Paks path from the executable";
            return false;
        }

        std::wstring pakDir = full;
        DWORD attr = GetFileAttributesW(pakDir.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            errOut = "Content\\Paks folder not found at " + NarrowW(pakDir) + " (is the executable path correct?)";
            return false;
        }

        Image img;
        if (!LoadPng(imagePath, img, errOut)) return false;

        Image sq = CropResizeSquare(img, 512);
        vector<Image> mips; mips.reserve(10);
        mips.push_back(std::move(sq));

        for (int i = 1; i < 10; ++i)
        {
            mips.push_back(Halve(mips.back()));
        }

        vector<vector<u8>> dxt; dxt.reserve(10);
        for (const auto& m : mips) 
        {
            dxt.push_back(EncodeDXT1(m));
        }

        // Sanity: encoded sizes must match the cooked layout.
        if (dxt[0].size() != kUBulkMipSizes[0] || dxt[1].size() != kUBulkMipSizes[1] || dxt[2].size() != kUBulkMipSizes[2])
        {
            errOut = "Unexpected DXT1 mip sizes: The selected image may be an invalid format.";
            return false;
        }

        // .ubulk = mips 0,1,2
        vector<u8> ubulk;
        ubulk.reserve(kUBulkTotal);
        for (int i = 0; i < 3; ++i)
        {
            ubulk.insert(ubulk.end(), dxt[i].begin(), dxt[i].end());
        }

        // .uexp = scaffolding with the 7 inline mip payloads (mips 3..9) overwritten.
        vector<u8> uexp(kUExp, kUExp + sizeof(kUExp));
        for (int i = 0; i < 7; ++i)
        {
            const MipSlot& s = kUExpInlineMips[i];
            if (dxt[3 + i].size() != s.size)
            {
                errOut = "internal: inline mip size mismatch";
                return false;
            }
            std::memcpy(&uexp[s.offset], dxt[3 + i].data(), s.size);
        }

        vector<u8> uasset(kUAsset, kUAsset + sizeof(kUAsset));

        const std::string base = "Home2/Content/UI/Sprites/Editor/ui_tray_icon_profile";
        vector<PakFile> files = {
            { base + ".uasset", &uasset },
            { base + ".uexp",   &uexp },
            { base + ".ubulk",  &ubulk },
        };
        
        std::wstring pakPath = pakDir + L"\\Home2-WindowsNoEditor_Q.pak";
        if (!WritePak(pakPath, "../../../", files))
        {
            errOut = "failed to write " + NarrowW(pakPath);
            return false;
        }

        return true;
    }

}
