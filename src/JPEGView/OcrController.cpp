#include "stdafx.h"
#include "resource.h"
#include "OcrController.h"
#include "MainDlg.h"
#include "JPEGImage.h"
#include "HelpersGUI.h"
#include "NLS.h"
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
    m_active = m_selecting = false;
    m_anchor = m_end = -1;
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
        if (!m_job->done) return; // one bounded job; repeated clicks never queue work
    }
    auto image = main.GetCurrentImage();
    if (!image) return;
    try {
        const void* pixels = image->OriginalPixels();
        auto snapshot = Ocr::MakeSnapshot(pixels, image->OrigWidth(), image->OrigHeight(), image->OriginalChannels());
        auto job = std::make_shared<Job>();
        job->window = main.GetHWND();
        m_job = job;
        m_result = {};
        m_anchor = m_end = -1;
        std::thread([job, snapshot = std::move(snapshot)]() {
            auto result = Ocr::Recognize(snapshot, job->canceled);
            std::lock_guard lock(job->mutex);
            job->result = std::move(result);
            job->done = true;
            if (job->window) ::PostMessage(job->window, WM_OCR_COMPLETED, 0, 0);
        }).detach();
    } catch (const std::exception&) {
        if (m_job) { std::lock_guard lock(m_job->mutex); m_job->done = true; }
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
    main.Invalidate(FALSE);
    if (!m_result.error.empty()) {
        ::MessageBox(main.GetHWND(), m_result.error.c_str(), _T("OCR"), MB_OK | MB_ICONINFORMATION);
    } else if (!m_active && main.GetCurrentImage() && !m_result.language.empty()) {
        ::MessageBox(main.GetHWND(), CNLS::GetString(_T("No text found in this image.")), _T("OCR"), MB_OK | MB_ICONINFORMATION);
    }
}

int COcrController::Hit(CMainDlg& main, CPoint point) const {
    float x = float(point.x - main.GetDIBOffset().x), y = float(point.y - main.GetDIBOffset().y);
    if (!main.ScreenToImage(x, y)) return -1;
    for (size_t i = 0; i < m_result.words.size(); ++i)
        if (Ocr::Contains(m_result.words[i], {x, y})) return int(i);
    return -1;
}

bool COcrController::MouseDown(CMainDlg& main, CPoint point) {
    if (!m_active) return false;
    const int hit = Hit(main, point);
    if (hit < 0) {
        m_anchor = m_end = -1;
        main.Invalidate(FALSE);
        return false; // background remains available for panning
    }
    m_anchor = m_end = hit;
    m_selecting = true;
    main.Invalidate(FALSE);
    return true;
}

bool COcrController::MouseMove(CMainDlg& main, CPoint point) {
    if (!m_active) return false;
    const int hit = Hit(main, point);
    if (m_selecting) {
        if (hit >= 0 && m_end != hit) { m_end = hit; main.Invalidate(FALSE); }
    }
    if (hit >= 0 || m_selecting) { ::SetCursor(::LoadCursor(nullptr, IDC_IBEAM)); return true; }
    return false;
}

bool COcrController::MouseUp() {
    const bool selecting = m_selecting;
    m_selecting = false;
    return selecting;
}

void COcrController::SelectAll() {
    if (!m_active || m_result.words.empty()) return;
    m_anchor = 0;
    m_end = int(m_result.words.size()) - 1;
}

bool COcrController::Copy(HWND window) {
    const auto text = Ocr::SelectedText(m_result.words, m_anchor, m_end);
    if (text.empty()) return false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    auto data = ::GlobalLock(memory);
    if (!data) { ::GlobalFree(memory); return false; }
    ::memcpy(data, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    if (::OpenClipboard(window)) {
        ::EmptyClipboard();
        ::SetClipboardData(CF_UNICODETEXT, memory);
        ::CloseClipboard();
    } else {
        ::GlobalFree(memory);
        return false;
    }
    return true;
}

// Paint: draw word bounding boxes and highlight the selected range.
// Image-space word corners are mapped to screen space via CMainDlg::ImageToScreen.
void COcrController::Paint(CMainDlg& main, HDC dc) {
    if (!m_active || m_result.words.empty()) return;

    // Selection range (inclusive)
    int selFirst = m_anchor, selLast = m_end;
    if (selFirst > selLast) std::swap(selFirst, selLast);
    // 选区在松开鼠标后必须保持可见，否则 Ctrl+C 复制的内容用户看不到。
    // 因此这里只看 anchor/end 是否有效，不看是否正在拖动。
    bool haveSelection = selFirst >= 0 && selLast >= 0 && selFirst < int(m_result.words.size()) && selLast < int(m_result.words.size());

    HPEN hBoxPen = ::CreatePen(PS_SOLID, 1, RGB(0, 120, 215));
    HBRUSH hSelBrush = ::CreateSolidBrush(RGB(0, 120, 215, 60));
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

        bool selected = haveSelection && int(i) >= selFirst && int(i) <= selLast;
        if (selected) {
            ::SelectObject(dc, hSelPen);
            ::SelectObject(dc, hSelBrush);
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
    ::DeleteObject(hSelBrush);
}
