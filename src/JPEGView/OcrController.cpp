#include "stdafx.h"
#include "resource.h"
#include "OcrController.h"
#include "MainDlg.h"
#include "JPEGImage.h"
#include "HelpersGUI.h"
#include "NLS.h"
#include "SettingsProvider.h"
#include <gdiplus.h>
#include <algorithm>
#include <mutex>
#include <thread>
#include <cmath>

struct COcrController::Job {
    std::mutex mutex;
    std::atomic<bool> canceled{false};
    bool done = false;
    HWND window = nullptr;
    Ocr::Result result;
};

COcrController::COcrController() = default;
COcrController::~COcrController() { Shutdown(); }

bool COcrController::Busy() const {
    if (!m_job) return false;
    std::lock_guard lock(m_job->mutex);
    return !m_job->done && !m_job->canceled.load();
}

void COcrController::Reset() {
    if (m_job) m_job->canceled.store(true);
    m_result = {};
    m_selected.clear();
    m_active = m_selecting = m_hovering = false;
    m_dragAnchor = m_dragEnd = -1;
}

void COcrController::Shutdown() {
    Reset();
    if (m_job) {
        std::lock_guard lock(m_job->mutex);
        m_job->window = nullptr;
    }
}

void COcrController::Toggle(CMainDlg& main) {
    if (m_active) {
        m_active = m_selecting = false;
        main.Invalidate(FALSE);
        return;
    }
    if (!m_result.words.empty()) {
        m_active = true;
        main.Invalidate(FALSE);
        return;
    }
    if (m_job) {
        std::lock_guard lock(m_job->mutex);
        // A canceled job (the previous image was switched away) must not block a
        // new recognition: it has already been told to stop, and its result will
        // be discarded, so starting the next one is safe and keeps the key from
        // silently doing nothing after a fast image change.
        if (!m_job->done && !m_job->canceled.load()) return; // one bounded job; repeated clicks never queue work
    }
    auto image = main.GetCurrentImage();
    if (!image) return;
    const std::shared_ptr<Job> previousJob = m_job;
    try {
        const void* pixels = image->OriginalPixels();
        auto snapshot = Ocr::MakeSnapshot(pixels, image->OrigWidth(), image->OrigHeight(), image->OriginalChannels());
        auto job = std::make_shared<Job>();
        job->window = main.GetHWND();
        // 设置只在 UI 线程读一次，工作线程拿到的是一份纯字符串列表。
        auto languages = Ocr::SplitLanguageList(std::wstring(CSettingsProvider::This().OcrLanguages()));
        // Publish before the thread starts: a worker that finishes immediately
        // posts WM_OCR_COMPLETED, and Complete has to find the job it belongs to.
        m_job = job;
        std::thread([job, snapshot = std::move(snapshot), languages = std::move(languages)]() {
            auto result = Ocr::Recognize(snapshot, languages, job->canceled);
            std::lock_guard lock(job->mutex);
            job->result = std::move(result);
            job->done = true;
            if (job->window) ::PostMessage(job->window, WM_OCR_COMPLETED, 0, 0);
        }).detach();
        m_result = {};
        m_selected.clear();
        m_dragAnchor = m_dragEnd = -1;
    } catch (const std::exception&) {
        // Undo the publish. A job that is out but has no worker behind it would
        // never be marked done and never posts WM_OCR_COMPLETED, and the busy gate
        // at the top of Toggle turns away every later attempt for good - that is
        // the state this catches, not a running worker being mislabelled (there is
        // none: the gate only lets us through when the job is null, done or
        // canceled).
        m_job = previousJob;
        ::MessageBox(main.GetHWND(), CNLS::GetString(_T("Could not start text recognition. Memory may be low.")), _T("OCR"), MB_OK | MB_ICONERROR);
    }
    main.Invalidate(FALSE);
}

void COcrController::Complete(CMainDlg& main) {
    if (!m_job) return;
    {
        std::lock_guard lock(m_job->mutex);
        if (!m_job->done) return;
        if (!m_job->canceled.load()) m_result = std::move(m_job->result);
    }
    m_job.reset();
    m_active = !m_result.words.empty();
    m_selected.assign(m_result.words.size(), false);
    main.Invalidate(FALSE);
    if (!m_result.error.empty()) {
        ::MessageBox(main.GetHWND(), m_result.error.c_str(), _T("OCR"), MB_OK | MB_ICONINFORMATION);
    } else if (!m_active && main.GetCurrentImage() && !m_result.language.empty()) {
        ::MessageBox(main.GetHWND(), CNLS::GetString(_T("No text found in this image.")), _T("OCR"), MB_OK | MB_ICONINFORMATION);
    }
}

int COcrController::Hit(CMainDlg& main, CPoint point) const {
    // ScreenToImage 内部自己完成屏幕到图像的换算（含居中偏移），
    // 与其他调用者（CropCtl 等）一致，直接传屏幕坐标即可。
    float x = float(point.x), y = float(point.y);
    if (!main.ScreenToImage(x, y)) return -1;
    for (size_t i = 0; i < m_result.words.size(); ++i)
        if (Ocr::Contains(m_result.words[i], {x, y})) return int(i);
    return -1;
}

bool COcrController::MouseDown(CMainDlg& main, CPoint point) {
    if (!m_active) return false;
    const int hit = Hit(main, point);
    if (hit < 0) {
        // Background click drops the selection but still leaves the event to
        // panning, which is what it did before multi-select existed.
        m_selected.assign(m_result.words.size(), false);
        m_dragAnchor = m_dragEnd = -1;
        main.Invalidate(FALSE);
        return false;
    }
    // The word is not toggled yet: a click (press and release without moving)
    // toggles it in MouseUp, a drag replaces the selection with the swept range.
    m_dragAnchor = m_dragEnd = hit;
    m_selecting = true;
    main.Invalidate(FALSE);
    return true;
}

bool COcrController::MouseMove(CMainDlg& main, CPoint point) {
    if (!m_active) { m_hovering = false; return false; }
    const int hit = Hit(main, point);
    m_hovering = hit >= 0;
    if (m_selecting) {
        if (hit >= 0 && m_dragEnd != hit) { m_dragEnd = hit; main.Invalidate(FALSE); }
        ::SetCursor(::LoadCursor(nullptr, IDC_IBEAM));
        return true; // swallow while a selection is being dragged
    }
    // Hovering must not consume the event. It used to return true here as well,
    // purely to keep the I-beam from being overwritten, and the side effect was
    // that a pan whose cursor crossed a word box stopped moving: OnMouseMove
    // returns early on a swallowed event, so DoDragging never ran.
    return false;
}

bool COcrController::MouseUp(CMainDlg& main) {
    const bool wasSelecting = m_selecting;
    m_selecting = false;
    const int anchor = m_dragAnchor, end = m_dragEnd;
    m_dragAnchor = m_dragEnd = -1;
    if (!wasSelecting || anchor < 0) return false;

    if (end < 0 || end == anchor) {
        // A click toggles one word, so a second click on it deselects again.
        if (size_t(anchor) < m_selected.size()) {
            m_selected[size_t(anchor)] = !m_selected[anchor];
        }
    } else {
        // A drag is a fresh selection: the swept range replaces what was there.
        // 这里不用 std::min/std::max：windows.h 把它们定义成了宏，而本文件没有
        // #undef（OcrEngine.cpp 里有），展开后会变成语法错误。
        std::fill(m_selected.begin(), m_selected.end(), false);
        int first = anchor, last = end;
        if (first > last) std::swap(first, last);
        for (int i = first; i <= last && size_t(i) < m_selected.size(); ++i) {
            m_selected[size_t(i)] = true;
        }
    }
    main.Invalidate(FALSE);
    return true;
}

bool COcrController::SelectAll() {
    if (!m_active || m_result.words.empty()) return false;
    m_selected.assign(m_result.words.size(), true);
    return true;
}

bool COcrController::Copy(HWND window) {
    const auto text = Ocr::SelectedText(m_result.words, m_selected);
    if (text.empty()) return false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    auto data = ::GlobalLock(memory);
    if (!data) { ::GlobalFree(memory); return false; }
    ::memcpy(data, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    if (!::OpenClipboard(window)) {
        ::GlobalFree(memory);
        return false;
    }
    ::EmptyClipboard();
    // On success the clipboard owns the block; on failure it is still ours and
    // has to be freed, otherwise every failed copy leaks the text.
    const bool copied = ::SetClipboardData(CF_UNICODETEXT, memory) != NULL;
    if (!copied) {
        ::GlobalFree(memory);
    }
    ::CloseClipboard();
    return copied;
}

// Paint: draw word bounding boxes and highlight the selected range.
// Image-space word corners are mapped to screen space via CMainDlg::ImageToScreen.
void COcrController::Paint(CMainDlg& main, HDC dc) {
    // 左上角状态行：识别中显示进度，识别完显示实际使用的语言标签。
    // 认不出文字时，"是哪个引擎在跑"是第一个要排查的问题，所以直接摆在屏幕上。
    const bool busy = Busy();
    if (busy || m_active) {
        std::wstring text;
        if (busy) {
            text = L"OCR [recognizing...]";
        } else {
            text = L"OCR";
            const std::wstring& tag = m_result.languageTag.empty() ? m_result.language : m_result.languageTag;
            if (!tag.empty()) { text += L" ["; text += tag; text += L"]"; }
            text += L": click words to toggle, drag for a range | Ctrl+A select all | Ctrl+C copy | Esc hide";
        }
        const int saved = ::SaveDC(dc);
        ::SetTextColor(dc, RGB(255, 255, 255));
        ::SetBkColor(dc, RGB(30, 35, 40));
        ::SetBkMode(dc, OPAQUE);
        ::TextOutW(dc, HelpersGUI::ScaleToScreen(12), HelpersGUI::ScaleToScreen(36),
                   text.c_str(), int(text.size()));
        ::RestoreDC(dc, saved);
    }

    if (!m_active || m_result.words.empty()) return;

    // While dragging, preview the range that will be selected on release;
    // otherwise show the words that are actually selected.
    int dragFirst = m_dragAnchor, dragLast = m_dragEnd;
    if (dragFirst > dragLast) std::swap(dragFirst, dragLast);
    const bool dragging = m_selecting && dragFirst >= 0 && dragLast >= 0;

    HPEN hBoxPen = ::CreatePen(PS_SOLID, 1, RGB(0, 120, 215));
    HPEN hSelPen = ::CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
    HPEN hOldPen = (HPEN)::SelectObject(dc, hBoxPen);
    HBRUSH hOldBrush = (HBRUSH)::SelectObject(dc, GetStockObject(NULL_BRUSH));
    ::SetBkMode(dc, TRANSPARENT);

    for (size_t i = 0; i < m_result.words.size(); ++i) {
        const auto& w = m_result.words[i];
        // Map the four corners from image space to screen space
        POINT pts[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            float x = w.corners[k].x, y = w.corners[k].y;
            if (!main.ImageToScreen(x, y)) { ok = false; break; }
            pts[k].x = (LONG)std::round(x);
            pts[k].y = (LONG)std::round(y);
        }
        if (!ok) continue;

        bool selected = dragging ? (int(i) >= dragFirst && int(i) <= dragLast) : IsSelected(i);
        if (selected) {
            ::SelectObject(dc, hSelPen);
            ::SelectObject(dc, GetStockObject(NULL_BRUSH));
            // 半透明填充：GDI 的 brush 没有 alpha，RGB 宏也没有第 4 个参数，
            // 所以这里用 GDI+ 画淡蓝填充，边框仍由上面的 pen 描。
            Gdiplus::Graphics g(dc);
            Gdiplus::SolidBrush fill(Gdiplus::Color(60, 0, 120, 215));
            Gdiplus::PointF gp[4] = {
                { (float)pts[0].x, (float)pts[0].y },
                { (float)pts[1].x, (float)pts[1].y },
                { (float)pts[2].x, (float)pts[2].y },
                { (float)pts[3].x, (float)pts[3].y },
            };
            g.FillPolygon(&fill, gp, 4);
        } else {
            ::SelectObject(dc, hBoxPen);
            ::SelectObject(dc, GetStockObject(NULL_BRUSH));
        }

        ::Polygon(dc, pts, 4);

        if (selected && !w.text.empty()) {
            // Draw the selected word text on top of the highlight
            RECT textRect = {pts[0].x, pts[0].y, pts[2].x, pts[2].y};
            ::SetTextColor(dc, RGB(255, 255, 255));
            ::DrawTextW(dc, w.text.c_str(), (int)w.text.size(), &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    ::SelectObject(dc, hOldPen);
    ::SelectObject(dc, hOldBrush);
    ::DeleteObject(hBoxPen);
    ::DeleteObject(hSelPen);
}
