# -*- coding: utf-8 -*-
"""#84 试修镜像：用两张样张实测几何 + 预期文本，跑修前/修后拼接逻辑。
纯 Python + PIL，可在 Linux 重跑；不依赖 WinRT（WinRT 只负责认字，拼接逻辑与它无关）。
用法：python3 tests/check_ocr_84_mirror.py
"""
import sys
from PIL import Image

SAMPLES = 'tests/samples'

# ---------- 图像几何：投影法量出文本包围盒 ----------
def load_gray(path):
    return Image.open(path).convert('L')

def ink_rows(im, thresh=128):
    """每行墨量 >0 的行带 [(y0, y1)]。"""
    w, h = im.size
    px = im.load()
    bands, y0 = [], None
    for y in range(h):
        ink = any(px[x, y] < thresh for x in range(w))
        if ink and y0 is None:
            y0 = y
        elif not ink and y0 is not None:
            bands.append((y0, y - 1))
            y0 = None
    if y0 is not None:
        bands.append((y0, h - 1))
    return bands

def ink_cols(im, y0, y1, thresh=128):
    """在行带内，每列墨量 >0 的列带 [(x0, x1)]。"""
    px = im.load()
    bands, x0 = [], None
    for x in range(im.size[0]):
        ink = any(px[x, y] < thresh for y in range(y0, y1 + 1))
        if ink and x0 is None:
            x0 = x
        elif not ink and x0 is not None:
            bands.append((x0, x - 1))
            x0 = None
    if x0 is not None:
        bands.append((x0, im.size[0] - 1))
    return bands

def word_boxes_horizontal(im):
    """横排：行带 -> 每行内按列带分组（列带间距 > 25px 视为词间空格）。
    实测：词内间隙 <= 24px，词间间隙 >= 30px，阈值取 25。"""
    lines = []
    for (y0, y1) in ink_rows(im):
        cols = ink_cols(im, y0, y1)
        words, cur = [], [cols[0]]
        for c in cols[1:]:
            if c[0] - cur[-1][1] > 25:
                words.append(cur)
                cur = [c]
            else:
                cur.append(c)
        words.append(cur)
        boxes = [(min(c[0] for c in w), y0, max(c[1] for c in w), y1) for w in words]
        lines.append(boxes)
    return lines

def col_boxes_vertical(im):
    """竖排：列带 -> 每列内按行带分组（单字）。"""
    w, h = im.size
    px = im.load()
    cols, x0 = [], None
    for x in range(w):
        ink = any(px[x, y] < 128 for y in range(h))
        if ink and x0 is None:
            x0 = x
        elif not ink and x0 is not None:
            cols.append((x0, x - 1))
            x0 = None
    if x0 is not None:
        cols.append((x0, w - 1))
    out = []
    for (cx0, cx1) in cols:
        chars = []
        for (y0, y1) in ink_rows(im):
            if any(px[x, y] < 128 for x in range(cx0, cx1 + 1) for y in range(y0, y1 + 1)):
                chars.append((cx0, y0, cx1, y1))
        out.append(chars)
    return out

# ---------- C++ 逻辑镜像 ----------
def is_cjk_old(c):
    return (0x2E80 <= c <= 0x9FFF) or (0xF900 <= c <= 0xFAFF) \
        or (0xFF00 <= c <= 0xFFEF) or (0xAC00 <= c <= 0xD7AF)

def is_cjk_new(c):
    return (0x2E80 <= c <= 0x9FFF) or (0xF900 <= c <= 0xFAFF) \
        or (0xFF00 <= c <= 0xFFEF)

def needs_space(prev, nxt, is_cjk):
    return not (is_cjk(ord(prev)) and is_cjk(ord(nxt)))

def ascii_word(s):
    return bool(s) and all(c.isascii() and (c.isalnum()) for c in s)

class W:
    def __init__(self, text, box, line=0):
        self.text = text
        self.box = box  # (x0, y0, x1, y1)
        self.line = line
    @property
    def c(self):
        x0, y0, x1, y1 = self.box
        return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]

def is_vertical_pair(prev, nxt):
    x_ov = min(prev.c[1][0], nxt.c[1][0]) - max(prev.c[0][0], nxt.c[0][0])
    y_ov = min(prev.c[2][1], nxt.c[2][1]) - max(prev.c[0][1], nxt.c[0][1])
    return x_ov > y_ov

def char_size(w, vertical):
    if not w.text:
        return 0.0
    span = (w.c[2][1] - w.c[0][1]) if vertical else (w.c[1][0] - w.c[0][0])
    return span / len(w.text)

def gap_along(prev, nxt, vertical):
    return (nxt.c[0][1] - prev.c[2][1]) if vertical else (nxt.c[0][0] - prev.c[1][0])

def split_inside_latin_old(prev, nxt):
    if not ascii_word(prev.text) or not ascii_word(nxt.text):
        return False
    pc = (prev.c[1][0] - prev.c[0][0]) / len(prev.text)
    nc = (nxt.c[1][0] - nxt.c[0][0]) / len(nxt.text)
    gap = nxt.c[0][0] - prev.c[1][0]
    ref = min(pc, nc)
    return ref > 0 and gap < ref * 0.2

def split_inside_latin_new(prev, nxt):
    if not ascii_word(prev.text) or not ascii_word(nxt.text):
        return False
    v = is_vertical_pair(prev, nxt)
    ref = min(char_size(prev, v), char_size(nxt, v))
    return ref > 0 and gap_along(prev, nxt, v) < ref * 0.2

def is_standalone_punct(s):
    return s in ("'", '\u2019', '-', '\u2010', '\u2011')

def punct_joins_old(prev, nxt):
    pp, np = is_standalone_punct(prev.text), is_standalone_punct(nxt.text)
    if not pp and not np:
        return False
    def cw(w):
        return 0.0 if not w.text else (w.c[1][0] - w.c[0][0]) / len(w.text)
    if pp and np:
        ref = min(cw(prev), cw(nxt))
    elif pp:
        ref = cw(nxt)
    else:
        ref = cw(prev)
    return ref > 0 and (nxt.c[0][0] - prev.c[1][0]) < ref * 0.2

def punct_joins_new(prev, nxt):
    pp, np = is_standalone_punct(prev.text), is_standalone_punct(nxt.text)
    if not pp and not np:
        return False
    v = is_vertical_pair(prev, nxt)
    def cs(w):
        return char_size(w, v)
    if pp and np:
        ref = min(cs(prev), cs(nxt))
    elif pp:
        ref = cs(nxt)
    else:
        ref = cs(prev)
    return ref > 0 and gap_along(prev, nxt, v) < ref * 0.2

def selected_text(words, is_cjk, split_fn, punct_fn):
    out, last_line, previous = '', 0, -1
    for i, w in enumerate(words):
        if not w.text:
            continue
        if previous >= 0:
            direct_prev = words[i - 1]
            if w.line != last_line:
                out += '\r\n' * max(1, w.line - last_line)
            elif needs_space(out[-1], w.text[0], is_cjk) \
                    and not split_fn(direct_prev, w) \
                    and not punct_fn(direct_prev, w):
                out += ' '
        out += w.text
        last_line, previous = w.line, i
    return out

# ---------- 样张 1：韩文 ----------
KO_LINES = [
    ['안녕하세요', '세계'],
    ['한국어', '테스트', '문장입니다'],
    ['서울은', '대한민국의', '수도입니다'],
    ['JPEGView', 'OCR', '테스트'],
]
KO_EXPECT = ['안녕하세요 세계', '한국어 테스트 문장입니다',
             '서울은 대한민국의 수도입니다', 'JPEGView OCR 테스트']

# ---------- 样张 2：竖排 ----------
ZH_COLS = [['春', '眠', '不', '觉', '晓'],
           ['处', '处', '闻', '啼', '鸟'],
           ['夜', '来', '风', '雨', '声']]
ZH_EXPECT_ORDER = ['春眠不觉晓', '处处闻啼鸟', '夜来风雨声']  # 右列 -> 中列 -> 左列

def main():
    fails = 0
    def check(name, got, want):
        nonlocal fails
        ok = got == want
        print(('PASS' if ok else 'FAIL'), '|', name, '| got:', repr(got), '| want:', repr(want))
        if not ok:
            fails += 1

    # --- 韩文：实测几何 + 预期词 ---
    ko_im = load_gray(SAMPLES + '/ocr_ko_words.png')
    ko_lines = word_boxes_horizontal(ko_im)
    print('ko_words.png: 测得行数', len(ko_lines), '每行词数', [len(l) for l in ko_lines])
    assert len(ko_lines) == 4, '韩文样张应为 4 行'
    words = []
    for li, (boxes, texts) in enumerate(zip(ko_lines, KO_LINES)):
        assert len(boxes) == len(texts), '行 %d 词数与预期不符: %s vs %s' % (li, boxes, texts)
        for b, t in zip(boxes, texts):
            words.append(W(t, b, li))
    old = selected_text(words, is_cjk_old, split_inside_latin_old, punct_joins_old)
    new = selected_text(words, is_cjk_new, split_inside_latin_new, punct_joins_new)
    check('韩文修前（应粘连）', old, '\r\n'.join(
        ['안녕하세요세계', '한국어테스트문장입니다', '서울은대한민국의수도입니다', 'JPEGView OCR 테스트']))
    check('韩文修后（应保留词间空格）', new, '\r\n'.join(KO_EXPECT))

    # --- 竖排：实测几何 + 列序 ---
    zh_im = load_gray(SAMPLES + '/ocr_vertical_zh.png')
    cols = col_boxes_vertical(zh_im)
    print('vertical_zh.png: 测得列数', len(cols), '每列字数', [len(c) for c in cols])
    assert len(cols) == 3 and all(len(c) == 5 for c in cols), '竖排样张应为 3 列 x 5 字'
    # 列按 left 升序即 cols 顺序（左列在前）；每列 top/bottom/center 相同
    lines = []
    for ci, (chars, texts) in enumerate(zip(cols, [ZH_COLS[2], ZH_COLS[1], ZH_COLS[0]])):
        top = min(c[1] for c in chars)
        bottom = max(c[3] for c in chars)
        left = min(c[0] for c in chars)
        right = max(c[2] for c in chars)
        lines.append({'texts': texts, 'top': top, 'bottom': bottom,
                      'left': left, 'right': right,
                      'center': (top + bottom) / 2.0,
                      'height': bottom - top, 'width': right - left})
    for ln in lines:
        print('  列 left=%d right=%d center=%.1f height=%d width=%d vertical=%s' % (
            ln['left'], ln['right'], ln['center'], ln['height'], ln['width'],
            ln['height'] > ln['width']))
    assert len({ln['center'] for ln in lines}) == 1, '三列 center 应完全相同（README 的前提）'
    old_order = [ln['texts'] for ln in sorted(lines, key=lambda l: (l['center'], l['left']))]
    new_order = [ln['texts'] for ln in sorted(lines, key=lambda l: -l['right'])]
    check('竖排修前列序（应倒序：左列先出）',
          [''.join(t) for t in old_order], ['夜来风雨声', '处处闻啼鸟', '春眠不觉晓'])
    check('竖排修后列序（应正序：右列先出）',
          [''.join(t) for t in new_order], ZH_EXPECT_ORDER)

    # --- 竖排间隙方向：两 ASCII 词上下叠放、大 y 间隙 ---
    upper = W('AB', (100, 100, 140, 120), 0)
    lower = W('CD', (100, 200, 140, 220), 0)  # y 间隙 80，远大于 0.2*字高
    check('竖排大 y 间隙：修前 SplitInsideLatinWord（x 间隙 0 误判为同一词）',
          split_inside_latin_old(upper, lower), True)
    check('竖排大 y 间隙：修后 SplitInsideLatinWord（应判为两个词）',
          split_inside_latin_new(upper, lower), False)
    # 横排回归：两 ASCII 词左右相邻、小 x 间隙仍判同一词
    left = W('expo', (100, 100, 180, 120), 0)
    right = W('rts', (182, 100, 222, 120), 0)  # x 间隙 2 < 0.2*20
    check('横排小 x 间隙：修前 SplitInsideLatinWord', split_inside_latin_old(left, right), True)
    check('横排小 x 间隙：修后 SplitInsideLatinWord', split_inside_latin_new(left, right), True)
    # 横排回归：中文词间不插空格
    check('中文修后仍不插空格', needs_space('春', '眠', is_cjk_new), False)
    check('日文假名修后仍不插空格', needs_space('あ', 'い', is_cjk_new), False)

    print('---')
    print('FAILURES:', fails)
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
