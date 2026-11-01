#define NOMINMAX
#include "stdafx.h"
#include "OcrEngine.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

// WinRT/WRL requires NTDDI_VERSION >= NTDDI_VISTA (Windows Vista),
// but stdafx.h sets _WIN32_WINNT=0x0501 for XP compatibility.
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_VISTA
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wrl/client.h>
#include <robuffer.h>
#include <MemoryBuffer.h>

namespace Ocr {
namespace {

template<int Channels>
void CopyLuminance(Snapshot& result, const uint8_t* source, size_t stride) {
    for (int y = 0; y < result.height; ++y) {
        const auto row = source + size_t(y) * stride;
        auto output = result.gray.data() + size_t(y) * result.width;
        for (int x = 0; x < result.width; ++x) {
            const auto p = row + size_t(x) * Channels;
            output[x] = uint8_t((29 * p[0] + 150 * p[1] + 77 * p[2] + 128) >> 8);
        }
    }
}

struct HorizontalSample {
    size_t left, right;
    double fraction;
};

template<int Channels>
void ResizeLuminance(Snapshot& result, const uint8_t* source, size_t stride) {
    std::vector<HorizontalSample> samples(result.width);
    for (int x = 0; x < result.width; ++x) {
        const double sx = std::max(0.0, (x + 0.5) * result.imageWidth / result.width - 0.5);
        const int x0 = int(sx), x1 = std::min(x0 + 1, result.imageWidth - 1);
        samples[x] = {size_t(x0) * Channels, size_t(x1) * Channels, sx - x0};
    }
    auto luminance = [](const uint8_t* p) { return (29 * p[0] + 150 * p[1] + 77 * p[2]) / 256.0; };
    for (int y = 0; y < result.height; ++y) {
        const double sy = std::max(0.0, (y + 0.5) * result.imageHeight / result.height - 0.5);
        const int y0 = int(sy), y1 = std::min(y0 + 1, result.imageHeight - 1);
        const double fy = sy - y0;
        const auto a = source + size_t(y0) * stride;
        const auto b = source + size_t(y1) * stride;
        auto output = result.gray.data() + size_t(y) * result.width;
        for (int x = 0; x < result.width; ++x) {
            const auto& sample = samples[x];
            const double fx = sample.fraction;
            const double top = luminance(a + sample.left) * (1 - fx) + luminance(a + sample.right) * fx;
            const double bottom = luminance(b + sample.left) * (1 - fx) + luminance(b + sample.right) * fx;
            output[x] = uint8_t(top * (1 - fy) + bottom * fy + 0.5);
        }
    }
}

} // namespace

Snapshot MakeSnapshot(const void* pixels, int width, int height, int channels) {
    if (!pixels || width <= 0 || height <= 0 || (channels != 3 && channels != 4))
        throw std::invalid_argument("Invalid OCR image");
    const double scale = std::min(1.0, double(SnapshotLimit) / std::max(width, height));
    Snapshot result;
    result.imageWidth = width;
    result.imageHeight = height;
    result.width = std::max(1, int(width * scale));
    result.height = std::max(1, int(height * scale));
    result.gray.resize(size_t(result.width) * result.height);
    const size_t stride = (size_t(width) * channels + 3) & ~size_t(3);
    auto source = static_cast<const uint8_t*>(pixels);
    if (result.width == width && result.height == height) {
        if (channels == 3) CopyLuminance<3>(result, source, stride);
        else CopyLuminance<4>(result, source, stride);
    } else {
        if (channels == 3) ResizeLuminance<3>(result, source, stride);
        else ResizeLuminance<4>(result, source, stride);
    }
    return result;
}

Result Recognize(const Snapshot& image, const std::atomic<bool>& canceled) {
    Result result;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        using namespace winrt::Windows::Globalization;
        using namespace winrt::Windows::Storage::Streams;

        if (canceled.load()) return result;

        // Try multiple languages so Chinese+English mixed text works without
        // user configuration.  Chinese engines usually also recognize English,
        // so zh-CN/zh-TW are tried first, then en-US, then the system default.
        OcrEngine engine;
        const std::wstring preferredLanguages[] = {L"zh-CN", L"zh-TW", L"en-US"};
        for (const auto& lang : preferredLanguages) {
            try {
                engine = OcrEngine::TryCreateFromLanguage(Language(lang));
                if (engine) break;
            } catch (...) {}
        }
        if (!engine) {
            try {
                engine = OcrEngine::TryCreateFromUserProfileLanguages();
            } catch (...) {}
        }
        if (!engine) {
            result.error = L"No Windows OCR language is installed. Add a supported language in Windows Settings > Time & language > Language & region.";
            return result;
        }

        if (image.width <= 0 || image.height <= 0 || image.gray.size() != size_t(image.width) * image.height)
            throw std::invalid_argument("Invalid OCR snapshot");

        const int limit = int(OcrEngine::MaxImageDimension());
        const double scale = std::min(1.0, double(limit) / std::max(image.width, image.height));
        const int width = std::max(1, int(image.width * scale));
        const int height = std::max(1, int(image.height * scale));

        // Resize snapshot to engine limit if needed
        std::vector<uint8_t> workGray;
        const uint8_t* grayPtr = image.gray.data();
        int workWidth = image.width, workHeight = image.height;
        if (scale < 1.0) {
            workWidth = width;
            workHeight = height;
            workGray.resize(size_t(workWidth) * workHeight);
            // Simple nearest-neighbor downscale
            for (int y = 0; y < workHeight; ++y) {
                const int sy = std::min(image.height - 1, int((y + 0.5) * image.height / workHeight - 0.5));
                const auto srcRow = image.gray.data() + size_t(sy) * image.width;
                auto dstRow = workGray.data() + size_t(y) * workWidth;
                for (int x = 0; x < workWidth; ++x) {
                    const int sx = std::min(image.width - 1, int((x + 0.5) * image.width / workWidth - 0.5));
                    dstRow[x] = srcRow[sx];
                }
            }
            grayPtr = workGray.data();
        }

        // Build a SoftwareBitmap from the gray buffer
        auto buffer = Buffer(image.gray.size() <= workGray.size() ? workGray.size() : image.gray.size());
        // The buffer above may be larger than needed; create exact-size buffer for the work plane
        const size_t planeSize = size_t(workWidth) * workHeight;
        auto planeBuffer = Buffer(planeSize);
        Microsoft::WRL::ComPtr<IBufferByteAccess> planeAccess;
        planeBuffer.as(planeAccess);
        uint8_t* planeData = nullptr;
        planeAccess->Buffer(&planeData);
        std::memcpy(planeData, grayPtr, planeSize);
        planeBuffer.Length(planeSize);

        SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(planeBuffer, BitmapPixelFormat::Gray8, workWidth, workHeight);

        auto ocrResult = engine.RecognizeAsync(bitmap).get();

        result.language = ocrResult.Language().LanguageTag();

        size_t lineIndex = 0;
        for (auto line : ocrResult.Lines()) {
            for (auto word : line.Words()) {
                Word w;
                w.text = word.Text();
                w.line = lineIndex;
                auto rect = word.BoundingBox();
                w.corners = {
                    {(float)rect.X, (float)rect.Y},
                    {(float)(rect.X + rect.Width), (float)rect.Y},
                    {(float)(rect.X + rect.Width), (float)(rect.Y + rect.Height)},
                    {(float)rect.X, (float)(rect.Y + rect.Height)}
                };
                result.words.push_back(std::move(w));
            }
            ++lineIndex;
        }
    } catch (const std::exception&) {
        result.error = L"OCR recognition failed.";
    } catch (...) {
        result.error = L"OCR recognition failed.";
    }
    return result;
}

bool Contains(const Word& word, Point point) {
    // Point-in-convex-quadrilateral test via cross products (all same sign => inside)
    auto cross = [](Point a, Point b, Point c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    const auto& c = word.corners;
    float c1 = cross(c[0], c[1], point);
    float c2 = cross(c[1], c[2], point);
    float c3 = cross(c[2], c[3], point);
    float c4 = cross(c[3], c[0], point);
    bool pos = c1 > 0 || c2 > 0 || c3 > 0 || c4 > 0;
    bool neg = c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0;
    return !(pos && neg);
}

std::wstring SelectedText(const std::vector<Word>& words, int first, int last) {
    if (words.empty() || first < 0 || last < 0 || first >= int(words.size()) || last >= int(words.size()))
        return L"";
    if (first > last) std::swap(first, last);
    std::wstring out;
    for (int i = first; i <= last; ++i) {
        if (!out.empty()) out += L" ";
        out += words[i].text;
    }
    return out;
}

} // namespace Ocr
