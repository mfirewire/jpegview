#include "stdafx.h"
// stdafx.h 经 ATL/windows.h 引入 max/min 宏。本项目使用预编译头，
// 写在 stdafx.h 之前的 #define NOMINMAX 会被 PCH 机制跳过而不生效，
// 因此在这里直接 undef，否则下面的 std::min/std::max 会被宏展开成语法错误。
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#include "OcrEngine.h"
#include "NLS.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <memory>
#include <stdexcept>
#include <wchar.h>

// WinRT/WRL requires NTDDI_VERSION >= NTDDI_VISTA (Windows Vista),
// but stdafx.h sets _WIN32_WINNT=0x0501 for XP compatibility.
// Use NTDDI_WIN10: robuffer.h / MemoryBuffer.h hide IBufferByteAccess behind
// NTDDI_WIN8 guards, and Windows.Media.Ocr itself only exists on Windows 10+.
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10
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

// One engine's line of words, in image space.
struct OcrLine {
    std::vector<Word> words;
    float top = 0, bottom = 0, left = 0;
    float center() const { return (top + bottom) * 0.5f; }
    float height() const { return bottom - top; }
};

// CJK unified ideographs and their extensions/compatibility forms, BMP
// only. CJK punctuation (U+3000-U+303F, e.g. 《》) and fullwidth forms are
// deliberately excluded: the zh engine's "He 《《" garbling must not count
// as Chinese content, or a pure-English line would keep the zh version.
bool IsCjkIdeograph(wchar_t c) {
    return (c >= 0x3400 && c <= 0x4DBF)
        || (c >= 0x4E00 && c <= 0x9FFF)
        || (c >= 0xF900 && c <= 0xFAFF);
}

// Lowercased primary subtag: the part before the first '-'.
std::wstring PrimarySubtag(const std::wstring& tag) {
    std::wstring out;
    for (wchar_t c : tag) {
        if (c == L'-') break;
        out += wchar_t(std::tolower(c));
    }
    return out;
}

// D2: merge zh and en engine lines into one reading order. Lines are
// matched across engines by vertical overlap (overlap > half the shorter
// line's height, best match wins, each en line used at most once). A
// matched pair keeps the zh line when it carries any real CJK ideograph,
// otherwise the en line. (An earlier 0.2 share bar lost Chinese on EN+CN
// translation lines, where the English sentence is long and the Chinese
// translation compact; and baseline zh output never puts a real ideograph
// on a pure-English line, so any ideograph reliably signals real Chinese.) Unmatched lines are kept as-is, so a line only one engine saw
// (pure Chinese, or the en-only "A - B") survives. The merged words come
// back top-to-bottom with fresh line indices, and zhUsed/enUsed report
// which engines contributed lines for the status line.
std::vector<Word> MergeEngineLines(const std::vector<OcrLine>& zh,
                                   const std::vector<OcrLine>& en,
                                   bool& zhUsed, bool& enUsed) {
    zhUsed = false;
    enUsed = false;
    struct Picked { const OcrLine* line; bool fromZh; };
    std::vector<Picked> picked;
    std::vector<bool> enTaken(en.size(), false);
    for (const auto& zl : zh) {
        int best = -1;
        float bestOverlap = 0.0f;
        for (size_t j = 0; j < en.size(); ++j) {
            if (enTaken[j]) continue;
            const OcrLine& el = en[j];
            const float overlap = std::min(zl.bottom, el.bottom) - std::max(zl.top, el.top);
            const float minH = std::min(zl.height(), el.height());
            if (minH > 0 && overlap > 0.5f * minH && overlap > bestOverlap) {
                best = int(j);
                bestOverlap = overlap;
            }
        }
        if (best >= 0) {
            enTaken[size_t(best)] = true;
            int c = 0;
            for (const auto& w : zl.words) {
                for (wchar_t ch : w.text) {
                    if (IsCjkIdeograph(ch)) ++c;
                }
            }
            // Any real CJK content means the line has Chinese the en engine
            // cannot show, so the zh line wins outright. A pure-English zh
            // line has c == 0 (its garbling is 《, Latin letters and digits,
            // never a real ideograph), so it always loses to en.
            const bool useZh = (c > 0);
            picked.push_back({useZh ? &zl : &en[size_t(best)], useZh});
        } else {
            picked.push_back({&zl, true});
        }
    }
    for (size_t j = 0; j < en.size(); ++j) {
        if (!enTaken[j]) picked.push_back({&en[j], false});
    }
    std::sort(picked.begin(), picked.end(), [](const Picked& a, const Picked& b) {
        const float ca = a.line->center(), cb = b.line->center();
        if (ca != cb) return ca < cb;
        return a.line->left < b.line->left;
    });
    std::vector<Word> out;
    for (size_t i = 0; i < picked.size(); ++i) {
        if (picked[i].fromZh) zhUsed = true; else enUsed = true;
        for (auto w : picked[i].line->words) {
            w.line = i;
            out.push_back(std::move(w));
        }
    }
    return out;
}

} // namespace

Snapshot MakeSnapshot(const void* pixels, int width, int height, int channels) {
    if (!pixels || width <= 0 || height <= 0 || (channels != 3 && channels != 4))
        throw std::invalid_argument("Invalid OCR image");
    // Windows OCR 对小字号很敏感：12–16 px 的文字会丢字形细节，典型症状是
    // l/I 混淆、词被切开（"expo rts"）、o 认成 0、甚至把英文认成汉字。
    // 屏幕截图正好落在这个尺寸，所以小图先放大再交给引擎。
    const int longest = std::max(width, height);
    double scale = std::min(1.0, double(SnapshotLimit) / longest);
    if (scale == 1.0) {
        if (longest <= 1200) scale = 2.0;
        else if (longest <= 2000) scale = 1.5;
    }
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

namespace {
// 图片里是别的文种时，首选引擎常常一个字都认不出来，这时才需要换下一个。
// 天花板压住最坏情况：N 个语言包不等于 N 倍等待时间。
constexpr int MaxLanguageAttempts = 3;

// Windows 的 OCR 引擎一次只吃一种语言包，不存在"同时多语言识别"。所以这里能做的
// 只是排序：INI 里配置的标签排前面（允许 zh-CN 这类简写去匹配 zh-Hans-CN），系统
// 已安装但没被列出的语言全部追加在后面 —— 装了什么就能用什么，没有白名单。
std::vector<std::wstring> BuildLanguageCandidates(const std::vector<std::wstring>& preferred) {
    std::vector<std::wstring> installed;
    for (const auto& language : winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages())
        installed.emplace_back(language.LanguageTag().c_str());

    auto lower = [](std::wstring text) {
        for (auto& c : text) c = wchar_t(std::towlower(c));
        return text;
    };
    auto primary = [&lower](const std::wstring& tag) {
        const size_t dash = tag.find(L'-');
        return lower(dash == std::wstring::npos ? tag : tag.substr(0, dash));
    };

    std::vector<std::wstring> candidates;
    std::vector<bool> consumed(installed.size(), false);
    auto push = [&candidates](const std::wstring& tag) {
        if (tag.empty()) return;
        for (const auto& existing : candidates)
            if (_wcsicmp(existing.c_str(), tag.c_str()) == 0) return;
        candidates.push_back(tag);
    };

    for (const auto& wanted : preferred) {
        bool matched = false;
        for (size_t i = 0; i < installed.size() && !matched; ++i) {
            if (_wcsicmp(installed[i].c_str(), wanted.c_str()) == 0) {
                push(installed[i]);
                consumed[i] = true;
                matched = true;
            }
        }
        // zh-CN 对不上 zh-Hans-CN，退一步比主语言子标签
        for (size_t i = 0; i < installed.size() && !matched; ++i) {
            if (!consumed[i] && primary(installed[i]) == primary(wanted)) {
                push(installed[i]);
                consumed[i] = true;
                matched = true;
            }
        }
        // 没匹配到也照样试一次：Language() 可能把它解析到已安装的区域变体
        if (!matched) push(wanted);
    }
    for (size_t i = 0; i < installed.size(); ++i)
        if (!consumed[i]) push(installed[i]);
    return candidates;
}
}

std::vector<std::wstring> SplitLanguageList(const std::wstring& list) {
    std::vector<std::wstring> tags;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t end = list.find_first_of(L",;", start);
        const std::wstring item = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        const size_t first = item.find_first_not_of(L" \t");
        const size_t last = item.find_last_not_of(L" \t");
        if (first != std::wstring::npos) tags.push_back(item.substr(first, last - first + 1));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return tags;
}

Result Recognize(const Snapshot& image, const std::vector<std::wstring>& preferredLanguages,
                 const std::atomic<bool>& canceled) {
    Result result;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Media::Ocr;
        using namespace winrt::Windows::Globalization;
        using namespace winrt::Windows::Storage::Streams;

        if (canceled.load()) return result;

        // 引擎按候选顺序逐个试。中文引擎本身能读英文和数字，英文引擎完全读不出汉字，
        // 所以顺序很重要 —— 这也是中英混排曾经失效的根因。
        const auto candidates = BuildLanguageCandidates(preferredLanguages);
        if (candidates.empty()) {
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

        // Build a SoftwareBitmap from the gray buffer (exact-size work plane only;
        // an earlier draft allocated a second unused buffer of gray.size() bytes here)
        const size_t planeSize = size_t(workWidth) * workHeight;
        // 用 DataWriter 拷贝字节，全程走 C++/WinRT 投影 API。
        // 不走 IBufferByteAccess（robuffer.h，本 TU 里不可见）也不走
        // ABI::Windows::Storage::Streams::IBufferByteAccess（ABI 命名空间未引入）。
        DataWriter writer;
        writer.WriteBytes(winrt::array_view<const uint8_t>(grayPtr, grayPtr + planeSize));
        auto planeBuffer = writer.DetachBuffer();

        SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(planeBuffer, BitmapPixelFormat::Gray8, workWidth, workHeight);

        // 快照可能被放大（小图）或缩小，OCR 的坐标是快照空间的，必须换算回原图
        // 空间，否则高亮框会整体放大或偏移，和文字对不上。
        const float toImageX = float(image.imageWidth) / float(workWidth);
        const float toImageY = float(image.imageHeight) / float(workHeight);

        std::wstring honoredName, honoredTag;
        int attempts = 0;

        // Single-engine path, kept for configs without both a zh and an en
        // engine (and as the fallback when a zh+en merge finds no words).
        // skipTags lists engines already tried by the merge, so the fallback
        // never pays for the same engine twice.
        auto runFirstWithWords = [&](const std::vector<std::wstring>& skipTags) -> bool {
            for (const auto& tag : candidates) {
                bool skipped = false;
                for (const auto& s : skipTags) {
                    if (_wcsicmp(tag.c_str(), s.c_str()) == 0) { skipped = true; break; }
                }
                if (skipped) continue;
                if (attempts >= MaxLanguageAttempts) break;
                OcrEngine engine{ nullptr };
                try {
                    engine = OcrEngine::TryCreateFromLanguage(Language(winrt::hstring(tag)));
                } catch (...) {
                    continue;   // 标签写错或语言包已卸载：跳过，别让一条坏配置拖垮整次识别
                }
                if (!engine) continue;
                ++attempts;
                const std::wstring display(engine.RecognizerLanguage().DisplayName().c_str());
                const std::wstring languageTag(engine.RecognizerLanguage().LanguageTag().c_str());
                if (honoredName.empty()) { honoredName = display; honoredTag = languageTag; }
                if (canceled.load()) return false;

                auto ocrResult = engine.RecognizeAsync(bitmap).get();

                result.words.clear();
                size_t lineIndex = 0;
                for (auto line : ocrResult.Lines()) {
                    for (auto word : line.Words()) {
                        Word w;
                        w.text = word.Text();
                        w.line = lineIndex;
                        // OcrWord 的包围盒属性名是 BoundingRect（Windows.Foundation.Rect），不是 BoundingBox。
                        auto rect = word.BoundingRect();
                        // std::array 是聚合类型，不能从 braced-init-list 直接赋值，逐元素写。
                        const float x0 = rect.X * toImageX, y0 = rect.Y * toImageY;
                        const float x1 = (rect.X + rect.Width) * toImageX, y1 = (rect.Y + rect.Height) * toImageY;
                        w.corners[0] = {x0, y0};
                        w.corners[1] = {x1, y0};
                        w.corners[2] = {x1, y1};
                        w.corners[3] = {x0, y1};
                        result.words.push_back(std::move(w));
                    }
                    ++lineIndex;
                }
                if (!result.words.empty()) {
                    // OcrResult 没有 Language 属性；识别语言在引擎上（RecognizerLanguage）。
                    result.language = display;
                    result.languageTag = languageTag;
                    return true;
                }
            }
            return false;
        };

        // D2: run the zh and en engines and merge per line, instead of
        // stopping at the first engine that returns words (M-OCR3's second
        // half). The first zh and en candidates in preferred order win;
        // BuildLanguageCandidates already puts exact matches first and
        // dedups (M-OCR3's matching half, done by M-OCR11).
        std::wstring zhTag, enTag;
        for (const auto& tag : candidates) {
            if (zhTag.empty() && PrimarySubtag(tag) == L"zh") zhTag = tag;
            if (enTag.empty() && PrimarySubtag(tag) == L"en") enTag = tag;
        }
        const bool useMerge = !zhTag.empty() && !enTag.empty()
            && _wcsicmp(zhTag.c_str(), enTag.c_str()) != 0;
        if (useMerge) {
            // Runs one engine and returns its lines in image space.
            // False means the engine could not be created (or the job was
            // canceled before the run); a RecognizeAsync failure still
            // propagates to the outer catch, as on the single-engine path.
            auto runOne = [&](const std::wstring& tag, std::vector<OcrLine>& linesOut,
                              std::wstring& displayOut, std::wstring& tagOut) -> bool {
                OcrEngine engine{ nullptr };
                try {
                    engine = OcrEngine::TryCreateFromLanguage(Language(winrt::hstring(tag)));
                } catch (...) {
                    return false;
                }
                if (!engine) return false;
                displayOut = engine.RecognizerLanguage().DisplayName().c_str();
                tagOut = engine.RecognizerLanguage().LanguageTag().c_str();
                if (canceled.load()) return false;

                auto ocrResult = engine.RecognizeAsync(bitmap).get();
                for (auto line : ocrResult.Lines()) {
                    OcrLine ol;
                    for (auto word : line.Words()) {
                        Word w;
                        w.text = word.Text();
                        // OcrWord 的包围盒属性名是 BoundingRect（Windows.Foundation.Rect），不是 BoundingBox。
                        auto rect = word.BoundingRect();
                        const float x0 = rect.X * toImageX, y0 = rect.Y * toImageY;
                        const float x1 = (rect.X + rect.Width) * toImageX, y1 = (rect.Y + rect.Height) * toImageY;
                        w.corners[0] = {x0, y0};
                        w.corners[1] = {x1, y0};
                        w.corners[2] = {x1, y1};
                        w.corners[3] = {x0, y1};
                        ol.words.push_back(std::move(w));
                    }
                    if (!ol.words.empty()) {
                        ol.top = ol.words[0].corners[0].y;
                        ol.bottom = ol.words[0].corners[2].y;
                        ol.left = ol.words[0].corners[0].x;
                        for (const auto& w : ol.words) {
                            if (w.corners[0].y < ol.top) ol.top = w.corners[0].y;
                            if (w.corners[2].y > ol.bottom) ol.bottom = w.corners[2].y;
                            if (w.corners[0].x < ol.left) ol.left = w.corners[0].x;
                        }
                        linesOut.push_back(std::move(ol));
                    }
                }
                return true;
            };

            std::vector<OcrLine> zhLines, enLines;
            std::wstring zhDisplay, zhLangTag, enDisplay, enLangTag;
            if (!canceled.load()) {
                if (runOne(zhTag, zhLines, zhDisplay, zhLangTag)) {
                    ++attempts;
                    if (honoredName.empty()) { honoredName = zhDisplay; honoredTag = zhLangTag; }
                }
            }
            if (!canceled.load()) {
                if (runOne(enTag, enLines, enDisplay, enLangTag)) {
                    ++attempts;
                    if (honoredName.empty()) { honoredName = enDisplay; honoredTag = enLangTag; }
                }
            }
            if (!canceled.load()) {
                bool zhUsed = false, enUsed = false;
                std::vector<Word> merged = MergeEngineLines(zhLines, enLines, zhUsed, enUsed);
                if (!merged.empty()) {
                    result.words = std::move(merged);
                    if (zhUsed && enUsed) {
                        result.language = zhDisplay + L"+" + enDisplay;
                        result.languageTag = zhLangTag + L"+" + enLangTag;
                    } else if (zhUsed) {
                        result.language = zhDisplay;
                        result.languageTag = zhLangTag;
                    } else {
                        result.language = enDisplay;
                        result.languageTag = enLangTag;
                    }
                }
            }
            if (result.words.empty() && !canceled.load()) {
                runFirstWithWords({zhTag, enTag});
            }
        } else {
            runFirstWithWords({});
        }
        // 一个引擎都没认出字时保留首选引擎的名字，"未找到文字"提示才有东西可显示
        if (result.words.empty()) {
            result.language = honoredName;
            result.languageTag = honoredTag;
        }
        // 所有候选语言都创建失败（标签写错、语言包全卸载）时，上面两个都是空、
        // error 也是空，调用方连个提示都弹不出来。给一个明确的原因。
        if (attempts == 0 && result.words.empty()) {
            // M-OCR11: this used to be a raw literal, so it never went through the
            // language tables at all. Route it through CNLS like every other user-
            // facing message; the key is in strings.txt (English baseline) and
            // awaits translation in the other language files.
            result.error = CNLS::GetString(_T("No OCR engine could be created for the configured languages."));
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

namespace {
// 中日韩文字之间不插空格——中文排版本来就没有词间空格，硬加会让整段难以阅读。
// 拉丁文字之间才需要分隔。范围覆盖假名、统一表意文字、兼容表意文字、全角标点与韩文音节。
bool NeedsSpaceBetween(wchar_t prev, wchar_t next) {
    auto isCjk = [](wchar_t c) {
        return (c >= 0x2E80 && c <= 0x9FFF)
            || (c >= 0xF900 && c <= 0xFAFF)
            || (c >= 0xFF00 && c <= 0xFFEF)
            || (c >= 0xAC00 && c <= 0xD7AF);
    };
    return !(isCjk(prev) && isCjk(next));
}
} // namespace

// 中文模型会把拉丁词切碎（"expo rts"、"n ew"、"co pyi ng"）。两个纯 ASCII 片段
// 之间的间隙明显小于一个字符宽度时，判定为同一个词被切开，拼接时不补空格。
static bool SplitInsideLatinWord(const Word& prev, const Word& next) {
    auto asciiWord = [](const std::wstring& s) {
        if (s.empty()) return false;
        for (wchar_t c : s) {
            const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
            if (!ok) return false;
        }
        return true;
    };
    if (!asciiWord(prev.text) || !asciiWord(next.text)) return false;
    const float prevChar = (prev.corners[1].x - prev.corners[0].x) / float(prev.text.size());
    const float nextChar = (next.corners[1].x - next.corners[0].x) / float(next.text.size());
    const float gap = next.corners[0].x - prev.corners[1].x;
    const float reference = std::min(prevChar, nextChar);
    return reference > 0 && gap < reference * 0.2f;
}

// 撇号与连字符是词内标点。OCR 会把 "don't" / "well-known" 切成 "don" + "'" + "t"、
// "well" + "-" + "known"，这三个框之间都不能有空格。只有当整框就是一个标点字符
// 时才这样判定——词尾的 "word'" 或真正的词间破折号（en/em dash）不在其列，仍按
// 普通词间空格处理。ASCII 与 Unicode 弯撇号/连字符都收进来。
static bool IsStandaloneWordInternalPunct(const std::wstring& s) {
    return s == L"'" || s == L"\u2019" || s == L"-" || s == L"\u2010" || s == L"\u2011";
}

// R-12: punctuation joining is gap-checked, unlike the old code which joined
// any standalone quote/hyphen regardless of distance. A quote only joins
// when it sits close to its neighbor: an opening quote in "said 'hello'"
// and the dashes in "A - B" are word gaps, not word-internal punctuation.
// The reference char width comes from the non-punct side, because a punct
// box itself is too narrow to calibrate against (a 1 px gap next to a 3 px
// quote box would otherwise fail a 0.2-width test and split "don't").
static bool PunctJoinsWithoutSpace(const Word& prev, const Word& next) {
    const bool prevPunct = IsStandaloneWordInternalPunct(prev.text);
    const bool nextPunct = IsStandaloneWordInternalPunct(next.text);
    if (!prevPunct && !nextPunct) return false;
    auto charWidth = [](const Word& w) {
        return w.text.empty() ? 0.0f : (w.corners[1].x - w.corners[0].x) / float(w.text.size());
    };
    float reference = 0.0f;
    if (prevPunct && nextPunct) {
        reference = std::min(charWidth(prev), charWidth(next));
    } else if (prevPunct) {
        reference = charWidth(next);
    } else {
        reference = charWidth(prev);
    }
    const float gap = next.corners[0].x - prev.corners[1].x;
    return reference > 0 && gap < reference * 0.2f;
}

std::wstring SelectedText(const std::vector<Word>& words, const std::vector<bool>& selected) {
    if (words.empty() || selected.size() != words.size()) return L"";

    std::wstring out;
    size_t lastLine = 0;
    int previous = -1;
    for (size_t i = 0; i < words.size(); ++i) {
        if (!selected[i]) continue;
        const std::wstring& text = words[i].text;
        if (text.empty()) continue;
        if (previous >= 0) {
            // 跨行保留换行；行内按语言决定是否插入空格。
            // R-12: 拼接判定看词表里的直接前驱 words[i - 1]，而不是上一个
            // 被选中的词。选中 don + t、中间的 ' 框没被选中时，直接前驱
            // 就是这个 '，按词内标点紧密拼接得到 don't；若按上一个被选
            // 中词判定，don 与 t 之间隔着 ' 框，间隙大，会被误判为两个词。
            // 标点拼接带间隙判定（见 PunctJoinsWithoutSpace），所以开头的
            // 引号（said 'hello'）与词间横线（A - B）不会被错误粘连。
            const Word& directPrev = words[i - 1];
            if (words[i].line != lastLine) {
                // N-03: one newline per skipped line, so selecting lines 0
                // and 2 keeps the blank line 1 instead of collapsing it.
                const int nSteps = (words[i].line > lastLine) ? int(words[i].line - lastLine) : 1;
                for (int k = 0; k < nSteps; ++k) out += L"\r\n";
            } else if (NeedsSpaceBetween(out.back(), text.front())
                       && !SplitInsideLatinWord(directPrev, words[i])
                       && !PunctJoinsWithoutSpace(directPrev, words[i])) {
                out += L' ';
            }
        }
        out += text;
        lastLine = words[i].line;
        previous = int(i);
    }
    return out;
}

std::wstring SelectedText(const std::vector<Word>& words, int first, int last) {
    if (words.empty() || first < 0 || last < 0 || first >= int(words.size()) || last >= int(words.size()))
        return L"";
    if (first > last) std::swap(first, last);

    std::vector<bool> selected(words.size(), false);
    for (int i = first; i <= last; ++i) selected[size_t(i)] = true;
    return SelectedText(words, selected);
}

} // namespace Ocr
