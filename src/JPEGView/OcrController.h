#pragma once

#include "OcrEngine.h"
#include <memory>

class CMainDlg;
class CJPEGImage;

class COcrController {
public:
    COcrController();
    ~COcrController();
    void Toggle(CMainDlg& main);
    void Reset();
    void Shutdown();
    void Complete(CMainDlg& main);
    void Paint(CMainDlg& main, HDC dc);
    bool MouseDown(CMainDlg& main, CPoint point);
    bool MouseMove(CMainDlg& main, CPoint point);
    bool MouseUp(CMainDlg& main);
    bool Active() const { return m_active; }
    // True while the pointer is over a recognized word but no selection is being
    // dragged. MouseMove no longer consumes hover moves, so the caller asks this
    // to pick the I-beam instead of the normal arrow.
    bool Hovering() const { return m_hovering; }
    bool Selecting() const { return m_selecting; }
    bool Busy() const;
    // 选中全部识别出的词；没有识别结果时返回 false，调用方应回落到原有按键行为
    bool SelectAll();
    bool Copy(HWND window);
private:
    struct Job;
    std::shared_ptr<Job> m_job;
    Ocr::Result m_result;
    bool m_active = false, m_selecting = false;
    bool m_hovering = false;
    // 多选：与 m_result.words 等长，true 表示该词被选中。点击切换单个词，
    // 拖动则用一段连续范围整体替换。复制时按索引顺序拼接，天然是原文顺序。
    std::vector<bool> m_selected;
    int m_dragAnchor = -1, m_dragEnd = -1;
    bool IsSelected(size_t index) const { return index < m_selected.size() && m_selected[index]; }
    int Hit(CMainDlg& main, CPoint point) const;
};
