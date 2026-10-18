#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ImageConv.h"
#include <windows.h>
#include <objidl.h>  // IStream
#include <objbase.h> // CreateStreamOnHGlobal
#include <algorithm>
using std::max; // gdiplus.h uses unqualified min/max, and NOMINMAX removed the macros
using std::min;
#include <gdiplus.h>
#include <mutex>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace imageconv
{

    static bool EnsureGdiplus()
    {
        static std::once_flag once;
        static bool ok = false;
        static ULONG_PTR token = 0;
        std::call_once(once, []
        {
            Gdiplus::GdiplusStartupInput input;
            ok = (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok);
        });
        return ok;
    }

    static bool GetPngEncoderClsid(CLSID& clsid)
    {
        UINT num = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        if (size == 0) return false;

        std::vector<BYTE> buf(size);
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
        Gdiplus::GetImageEncoders(num, size, codecs);

        for (UINT i = 0; i < num; ++i)
        {
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0)
            {
                clsid = codecs[i].Clsid;
                return true;
            }
        }

        return false;
    }

    bool JpegBytesToPngFile(const std::string& imageBytes, const std::wstring& pngPath)
    {
        if (!EnsureGdiplus() || imageBytes.empty()) return false;

        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, imageBytes.size());
        if (!hMem) return false;

        if (void* p = GlobalLock(hMem))
        {
            memcpy(p, imageBytes.data(), imageBytes.size());
            GlobalUnlock(hMem);
        }
        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(hMem, TRUE, &stream) != S_OK) // TRUE: stream frees hMem on release
        {
            GlobalFree(hMem);
            return false;
        }

        std::wstring tmp = pngPath + L".tmp";
        bool ok = false;
        {
            Gdiplus::Bitmap bmp(stream, FALSE);
            CLSID clsid;
            if (bmp.GetLastStatus() == Gdiplus::Ok && GetPngEncoderClsid(clsid))
            {
                ok = (bmp.Save(tmp.c_str(), &clsid, nullptr) == Gdiplus::Ok);
            }
        }// bmp released before the stream

        stream->Release();

        if (!ok)
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        if (!MoveFileExW(tmp.c_str(), pngPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    bool SquareWithBlurFillPngFile(const std::wstring& srcPath, const std::wstring& dstPath)
    {
        if (!EnsureGdiplus()) return false;

        bool ok = false;
        {
            Gdiplus::Bitmap src(srcPath.c_str());
            if (src.GetLastStatus() != Gdiplus::Ok) return false;

            INT w = (INT)src.GetWidth();
            INT h = (INT)src.GetHeight();
            if (w <= 0 || h <= 0) return false;

            INT side = max(w, h);

            // A hard downscale then an upscale with smooth interpolation is a cheap blur for the background fill.
            Gdiplus::Bitmap tiny(24, 24, PixelFormat32bppARGB);
            {
                Gdiplus::Graphics gt(&tiny);
                gt.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
                gt.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                gt.DrawImage(&src, Gdiplus::Rect(0, 0, 24, 24), 0, 0, w, h, Gdiplus::UnitPixel);
            }

            Gdiplus::Bitmap dst(side, side, PixelFormat32bppARGB);
            {
                Gdiplus::Graphics g(&dst);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

                // Background the blurred fill stretched to cover the whole square.
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(&tiny, Gdiplus::Rect(0, 0, side, side), 0, 0, 24, 24, Gdiplus::UnitPixel);

                // A light dark overlay so the foreground reads against a busy fill. The foreground is drawn after, so only the letterbox area stays shaded.
                Gdiplus::SolidBrush shade(Gdiplus::Color(64, 0, 0, 0));
                g.FillRectangle(&shade, 0, 0, side, side);

                // Foreground the source fit and centered at the full resolution
                double scale = (w >= h) ? (double)side / w : (double)side / h;
                INT fw = (INT)(w * scale + 0.5);
                INT fh = (INT)(h * scale + 0.5);
                INT fx = (side - fw) / 2;
                INT fy = (side - fh) / 2;
                g.DrawImage(&src, Gdiplus::Rect(fx, fy, fw, fh), 0, 0, w, h, Gdiplus::UnitPixel);
            } // the Graphics must release its lock on dst before Save!

            CLSID clsid;
            if (GetPngEncoderClsid(clsid))
            {
                std::wstring tmp = dstPath + L".tmp";
                if (dst.Save(tmp.c_str(), &clsid, nullptr) == Gdiplus::Ok &&
                    MoveFileExW(tmp.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    ok = true;
                }
                else
                {
                    DeleteFileW(tmp.c_str());
                }
            }
        }

        return ok;
    }

    bool Rotate90PngFile(const std::wstring& srcPath, const std::wstring& dstPath)
    {
        if (!EnsureGdiplus()) return false;

        bool ok = false;
        {
            Gdiplus::Bitmap src(srcPath.c_str());
            if (src.GetLastStatus() != Gdiplus::Ok) return false;

            if (src.RotateFlip(Gdiplus::Rotate90FlipNone) != Gdiplus::Ok) return false; // 90 degrees clockwise

            CLSID clsid;
            if (GetPngEncoderClsid(clsid))
            {
                std::wstring tmp = dstPath + L".tmp";
                if (src.Save(tmp.c_str(), &clsid, nullptr) == Gdiplus::Ok &&
                    MoveFileExW(tmp.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    ok = true;
                }
                else
                {
                    DeleteFileW(tmp.c_str());
                }
            }
        }

        return ok;
    }

}
