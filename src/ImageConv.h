#pragma once
#include <string>

// Isolated GDI+ image conversion. Kept in its own translation unit so its windows.h/gdiplus.h includes never collide with cpr/curl in the caller
namespace imageconv
{
    // Decode image bytes (the CDN screenshot is JPEG) and write them as PNG at pngPath, atomically (a temp file then MoveFileEx). Returns false if the bytes aren't a decodable image.
    bool JpegBytesToPngFile(const std::string& imageBytes, const std::wstring& pngPath);

    // Load an image file and write a square PNG at dstPath. side is the source's longer edge. the image is fit and centered over a blurred, stretched fill of itself so nothing is cropped
    bool SquareWithBlurFillPngFile(const std::wstring& srcPath, const std::wstring& dstPath);

    // Load an image file and rotate it 90 degrees clockwise
    bool Rotate90PngFile(const std::wstring& srcPath, const std::wstring& dstPath);
}
