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

}
