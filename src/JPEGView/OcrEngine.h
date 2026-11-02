#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Ocr {
// A bounded snapshot owns its pixels. Workers never access the viewer's images.
constexpr int SnapshotLimit = 2048;
struct Snapshot {
    int width = 0, height = 0;
    int imageWidth = 0, imageHeight = 0;
    std::vector<uint8_t> gray;
};
struct Point { float x, y; };
struct Word {
    std::wstring text;
    std::array<Point, 4> corners;
    size_t line = 0;
};
struct Result {
    std::vector<Word> words;
    std::wstring language;      // display name of the engine that ran, e.g. 中文(简体)
    std::wstring languageTag;   // BCP-47 tag of that engine, e.g. zh-Hans-CN
    std::wstring error;
};

// BGR/BGRA, top-down, DWORD-aligned rows. Bilinear sampling preserves small text
// better than nearest-neighbor while bounding memory independently of image size.
Snapshot MakeSnapshot(const void* pixels, int width, int height, int channels);
// Splits an INI style list ("zh-CN, zh-TW;en-US") into trimmed, non-empty tags.
std::vector<std::wstring> SplitLanguageList(const std::wstring& list);
// Initializes WinRT on the calling worker, never on the UI thread.
// preferredLanguages: ordered tags from the INI. Languages that are installed but
// not listed are appended, so every installed OCR pack stays reachable.
Result Recognize(const Snapshot& image, const std::vector<std::wstring>& preferredLanguages,
                 const std::atomic<bool>& canceled);
bool Contains(const Word& word, Point point);
std::wstring SelectedText(const std::vector<Word>& words, int first, int last);
}
