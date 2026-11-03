#include "StdAfx.h"
#include "KeyMap.h"
#include "NLS.h"
#include "resource.h"
#include "Helpers.h"
#include "SettingsProvider.h"

#define M_SHIFT 0x10000
#define M_ALT   0x20000
#define M_CTRL  0x40000

// KeyMap filename constants
static const TCHAR* KEYMAP_USER_FILE_NAME = _T("KeyMap.txt");
static const TCHAR* KEYMAP_DEFAULT_FILE_NAME = _T("KeyMap.txt.default");
static const TCHAR* KEYMAP_SYMBOLS_FILE_NAME = _T("symbols.km");

struct SKey {
	LPCTSTR Name;
	int KeyCode;
};

#define NUM_KEYS 30

static SKey KeyTable[NUM_KEYS] = {
	{ _T("Alt"), M_ALT },
	{ _T("Ctrl"), M_CTRL },
	{ _T("Shift"), M_SHIFT },
	{ _T("Esc"), VK_ESCAPE },
	{ _T("Return"), VK_RETURN },
	{ _T("Space"), VK_SPACE },
	{ _T("End"), VK_END },
	{ _T("Home"), VK_HOME },
	{ _T("Back"), VK_BACK },
	{ _T("Tab"), VK_TAB },
	{ _T("PgDn"), VK_PAGE_DOWN },
	{ _T("PgUp"), VK_PAGE_UP },
	{ _T("Left"), VK_LEFT },
	{ _T("Right"), VK_RIGHT },
	{ _T("Up"), VK_UP },
	{ _T("Down"), VK_DOWN },
	{ _T("Insert"), VK_INSERT },
	{ _T("Del"), VK_DELETE },
	{ _T("Plus"), VK_PLUS },
	{ _T("Minus"), VK_MINUS },
	{ _T("Mul"), VK_MULTIPLY },
	{ _T("Div"), VK_DIVIDE },
	{ _T("Comma"), VK_OEM_COMMA },
	{ _T("Period"), VK_OEM_PERIOD },
	{ _T("MouseL"), VK_LBUTTON },
	{ _T("MouseR"), VK_RBUTTON },
	{ _T("MouseM"), VK_MBUTTON },
	{ _T("MouseX1"), VK_XBUTTON1 },
	{ _T("MouseX2"), VK_XBUTTON2 },
	{ _T("MouseDblClk"), VK_LBUTTONDBLCLK }
};

static LPTSTR _SkipWhiteSpace(LPTSTR sText) {
	while (_istspace(*sText) && *sText != 0) sText++;
	return sText;
}

static bool _IsComment(LPTSTR sText) {
	return sText[0] != 0 && (sText[0] == _T('/') && sText[1] == _T('/'));
}

static LPTSTR _Parse(LPTSTR sText) {
	while (!_istspace(*sText) && *sText != 0) sText++;
	if (*sText == 0) {
		return NULL;
	}
	*sText++ = 0;
	sText = _SkipWhiteSpace(sText);
	if (*sText == 0) {
		return NULL;
	}
	LPTSTR sRight = sText;
	while (!_istspace(*sText) && *sText != 0) sText++;
	*sText = 0;
	return sRight;
}

static int _GetKeyCode(LPTSTR sKey) {
	for (int i = 0; i < NUM_KEYS; i++) {
		if (_tcsicmp(sKey, KeyTable[i].Name) == 0) {
			return KeyTable[i].KeyCode;
		}
	}

	int nFNumber;
	if (_stscanf(sKey, _T("F%d"), &nFNumber) == 1 && nFNumber >= 1 && nFNumber <= 24) {
		return VK_F1 + nFNumber - 1;
	}

	if (_tcslen(sKey) == 1) {
		int nChar = _totupper(sKey[0]);
		if ((nChar >= _T('0') && nChar <= _T('9')) || (nChar >= _T('A') && nChar <= _T('Z'))) {
			return nChar;
		}
	}

	// M-OCR14: a token that is not a modifier, an F key or a single character
	// ("Foo" in "Foo+Q") returns -1 instead of silently contributing 0, so
	// _ParseKeys can reject the whole shortcut. The old 0 made the command
	// end up on a different key than the user intended (bare Q above).
	// No valid token returns -1: every table entry and F key is positive.
	OutputDebugString(_T("KeyMap: unrecognized key token: "));
	OutputDebugString(sKey);
	OutputDebugString(_T("\n"));
	return -1;
}

// Returns the combined key code, or -1 when the shortcut is invalid: an
// unrecognized token, a repeated modifier ("Ctrl+Ctrl+Q" would sum the
// modifier bit twice into a value no real key press ever produces), or a
// second base key ("Q+W" likewise never matches a single key press).
// Callers treat -1 like 0 (no mapping); both are <= 0, so a single
// "nKeyCode <= 0" check covers the old empty case and the new rejects.
static int _ParseKeys(LPTSTR sKeys, CString* pReason = NULL) {
	int nKeyCode = 0;
	int nModifiersSeen = 0;
	bool bBaseKeySeen = false;
	bool bContinue = true;
	while (bContinue) {
		LPTSTR sStart = sKeys;
		while (*sKeys != _T('+') && *sKeys != 0) sKeys++;
		if (*sKeys == 0) bContinue = false;
		*sKeys = 0;
		int nCode = _GetKeyCode(sStart);
		if (nCode < 0) {
			if (pReason != NULL) {
				*pReason = CString(_T("unrecognized token '")) + sStart + _T("'");
			}
			return -1;
		}
		if (nCode == M_ALT || nCode == M_CTRL || nCode == M_SHIFT) {
			if ((nModifiersSeen & nCode) != 0) {
				if (pReason != NULL) {
					*pReason = _T("repeated modifier (dead mapping: no real key press ever produces it)");
				}
				return -1; // repeated modifier: dead mapping
			}
			nModifiersSeen |= nCode;
		} else {
			if (bBaseKeySeen) {
				if (pReason != NULL) {
					*pReason = _T("second base key (dead mapping: no single key press ever matches it)");
				}
				return -1; // second base key: dead mapping
			}
			bBaseKeySeen = true;
		}
		nKeyCode += nCode;
		sKeys++;
	}
	return nKeyCode;
}

static int _FindCommandId(stdext::hash_map<LPCTSTR, int, CHashCompareLPCTSTR> & symbolMap, LPCTSTR sSymbolName) {
	stdext::hash_map<LPCTSTR, int, CHashCompareLPCTSTR>::const_iterator iter;
	iter = symbolMap.find(sSymbolName);
	if (iter == symbolMap.end()) {
		return -1;
	} else {
		return iter->second;
	}
}

static CString _GetKeyShortcutName(int nShortcut) {
	CString sKeyDesc;
	if ((nShortcut & M_ALT) != 0) sKeyDesc += _T("Alt+");
	if ((nShortcut & M_CTRL) != 0) sKeyDesc += _T("Ctrl+");
	if ((nShortcut & M_SHIFT) != 0) sKeyDesc += _T("Shift+");
	int nRealShortcut = nShortcut & 0xFF;
	for (int i = 0; i < NUM_KEYS; i++) {
		if (KeyTable[i].KeyCode == nRealShortcut) {
			if (KeyTable[i].Name == _T("Plus")) {
				sKeyDesc += _T("+");
			} else if (KeyTable[i].Name == _T("Minus")) {
				sKeyDesc += _T("-");
			} else {
				sKeyDesc += KeyTable[i].Name;
			}
		}
	}
	if (nRealShortcut >= VK_F1 && nRealShortcut <= VK_F24) {
		TCHAR buff[8];
		_sntprintf(buff, 8, _T("F%d"), nRealShortcut - VK_F1 + 1);
		sKeyDesc += buff;
	}
	if ((nRealShortcut >= _T('0') && nRealShortcut <= _T('9')) || (nRealShortcut >= _T('A') && nRealShortcut <= _T('Z'))) {
		sKeyDesc += CString((TCHAR)nRealShortcut);
	}
	return sKeyDesc;
}

// Similar to SettingsProvider, attemps to load the keymap from various locations
// before settling for the default
//
// First tries to see if /keymap parameter was passed on command line
// Then tries %AppData%\JPEGView\KeyMap.txt
// Then tries (JPEGView root)\KeyMap.txt
// if none of those, then (JPEGView root)\KeyMap.txt.default
CKeyMap::CKeyMap() {
	FILE* fkm_ptr;
	CString exe_path = CString(CSettingsProvider::This().GetEXEPath());

	// TODO: ParseCommandLineForKeyMapFile or something... where you can specify a custom full file path name for a keymap you'd like to use instead of the default search order

	// Which file was actually loaded, and whether it is the user's own
	// KeyMap.txt. Only a user file is ever rewritten (commenting out
	// rejected lines); the shipped KeyMap.txt.default is never touched.
	CString sLoadedKeyMapPath;
	bool bLoadedKeyMapIsUserFile = false;

	// check if the use directory keymap.txt exists
	fkm_ptr = _tfopen(CString(Helpers::JPEGViewAppDataPath()) + KEYMAP_USER_FILE_NAME, _T("r"));
	if (fkm_ptr != NULL) {
		sLoadedKeyMapPath = CString(Helpers::JPEGViewAppDataPath()) + KEYMAP_USER_FILE_NAME;
		bLoadedKeyMapIsUserFile = true;
	} else {
		// the appdata one doesn't exist, try the local directory
		fkm_ptr = _tfopen(exe_path + KEYMAP_USER_FILE_NAME, _T("r"));
		if (fkm_ptr != NULL) {
			sLoadedKeyMapPath = exe_path + KEYMAP_USER_FILE_NAME;
			bLoadedKeyMapIsUserFile = true;
		} else {
			// the user one doesn't exist either, load the default file (if that doesn't exist either someone messed up)
			fkm_ptr = _tfopen(exe_path + KEYMAP_DEFAULT_FILE_NAME, _T("r"));
		}
	}

	// no keymap file found from above
	if (fkm_ptr == NULL) {
		// default handling
		AddDefaultEscapeHandling();
		return;
	}

	// no symbols file
	FILE* fsym_ptr = _tfopen(exe_path + KEYMAP_SYMBOLS_FILE_NAME, _T("r"));
	if (fsym_ptr == NULL) {
		fclose(fkm_ptr);
		AddDefaultEscapeHandling();
		return;
	}


	const int BUFF_LEN = 256;
	TCHAR lineBuff[BUFF_LEN];

	const int SYMBOL_TABLE_LEN = 4096;
	TCHAR symbolTable[SYMBOL_TABLE_LEN];
	TCHAR* pSymbolTableEnd = &(symbolTable[SYMBOL_TABLE_LEN - 1]);
	TCHAR* pSymbolTable = &(symbolTable[0]);

	stdext::hash_map<LPCTSTR, int, CHashCompareLPCTSTR> m_symbolMap;

	// read the symbols file first to get all valid symbols
	while (_fgetts(lineBuff, BUFF_LEN, fsym_ptr) != NULL) {
		LPTSTR sLine = _SkipWhiteSpace(lineBuff);
		if (*sLine == 0) continue;
		if (_IsComment(sLine)) continue;

		if (_tcsncmp(sLine, _T("#define"), 7) == 0) {
			LPTSTR sSymbol = sLine + 7;
			sSymbol = _SkipWhiteSpace(sSymbol);
			LPTSTR sValue = _Parse(sSymbol);
			if (sValue == NULL) continue;
			int nValue = _ttoi(sValue);
			if (nValue == 0) continue;
			int nSymbolLen = (int)_tcslen(sSymbol);
			if (pSymbolTable + nSymbolLen + 1 <= pSymbolTableEnd) {
				_tcscpy(pSymbolTable, sSymbol);
				m_symbolMap[pSymbolTable] = nValue;
				pSymbolTable += nSymbolLen + 1;
			} // else the symbol table is full
		} // the symbols file is standalone, all other definitions are ignored
	}

	fclose(fsym_ptr);

	// read just the keymap file
	// Physical line number of the chunk being read. The counter increments
	// for every chunk, including skipped ones, so it matches physical line
	// numbers whenever a rewrite happens (see CommentOutRejectedLines for
	// the long-line guard that keeps this true).
	int nKeyMapLine = 0;
	while (_fgetts(lineBuff, BUFF_LEN, fkm_ptr) != NULL) {
		int nThisLine = nKeyMapLine++;
		LPTSTR sLine = _SkipWhiteSpace(lineBuff);
		if (*sLine == 0) continue;
		if (_IsComment(sLine)) continue;
		// Keep the raw line for the rejected-shortcut report: _Parse and
		// _ParseKeys both rewrite the buffer in place, so the text must be
		// copied before either runs.
		CString sRawLine(sLine);
		sRawLine.TrimRight();
		if (_tcsncmp(sLine, _T("#define"), 7) == 0) continue; // the symbols in the KeyMap are ignored (in case you have an old keymap file which defined the symbols and keybindings together)

		LPTSTR sCommandId = _Parse(sLine);
		// _Parse returns NULL when the line holds only one token (a dangling key
		// with no command). The symbol file loop above already null-checks; this
		// one used to dereference the NULL and crash at startup.
		if (sCommandId == NULL || *sCommandId == 0) continue;
		int nCommandId = _FindCommandId(m_symbolMap, sCommandId);
		if (nCommandId < 0) continue;
		CString sParseReason;
		int nKeyCode = _ParseKeys(sLine, &sParseReason);
		if (nKeyCode <= 0) {
			RecordRejectedShortcut(sRawLine, sParseReason.IsEmpty() ? _T("invalid shortcut") : (LPCTSTR)sParseReason);
			m_rejectedKeyMapLines.push_back(nThisLine);
			continue;
		}
		// A later line for the same key displaces the earlier command's
		// binding; the earlier binding is the one that silently stops
		// working, so report the displacement instead of overwriting quietly.
		stdext::hash_map<int, int>::const_iterator existingBinding = m_keyMap.find(nKeyCode);
		if (existingBinding != m_keyMap.end() && existingBinding->second != nCommandId) {
			CString sDetail;
			sDetail.Format(_T("duplicate binding: this key was already bound to another command, so the earlier binding is displaced (shortcut: %s)"), (LPCTSTR)_GetKeyShortcutName(nKeyCode));
			RecordRejectedShortcut(sRawLine, sDetail);
		}
		m_keyMap[nKeyCode] = nCommandId;
	}

	// The file handle is closed before rewriting: replacing a file that
	// is still open for reading is not reliable on Windows. The dialog is
	// shown by AddDefaultEscapeHandling below, after the rewrite, so it can
	// report whether the lines were commented out.
	fclose(fkm_ptr);

	if (bLoadedKeyMapIsUserFile && !m_rejectedKeyMapLines.empty()) {
		CommentOutRejectedLines(sLoadedKeyMapPath);
	}

	AddDefaultEscapeHandling();
}

int CKeyMap::GetVirtualKeyCode(LPCTSTR keyName) {
	CString sBuffer(keyName);
	sBuffer.TrimLeft();
	sBuffer.TrimRight();
	if (sBuffer.IsEmpty()) {
		return -1;
	}
	LPTSTR buffer = sBuffer.GetBuffer(sBuffer.GetLength());
	int key = _ParseKeys(buffer);
	return (key <= 0) ? -1 : key;
}

int CKeyMap::GetCombinedKeyCode(int keyCode, bool alt, bool control, bool shift) {
	int nKeyCode = keyCode;
	if (alt) nKeyCode += M_ALT;
	if (control) nKeyCode += M_CTRL;
	if (shift) nKeyCode += M_SHIFT;
	return nKeyCode;
}

CString CKeyMap::GetShortcutKey(int combinedKeyCode) {
	if (combinedKeyCode == -1)
	{
		return CString(_T(""));
	}
	return _GetKeyShortcutName(combinedKeyCode);
}

void CKeyMap::AddDefaultEscapeHandling() {
	if (m_keyMap.find(VK_ESCAPE) == m_keyMap.end()) {
		// ESC key not in keymap - map to default command for ESC
		m_keyMap[VK_ESCAPE] = IDM_DEFAULT_ESC;
	}
	ApplyIniOverrides();
	// End of construction on every path (this function is the common
	// tail of all constructor exits): report the rejected definitions
	// once, here, and nowhere else.
	ShowRejectedShortcuts();
}

void CKeyMap::RecordRejectedShortcut(LPCTSTR sDefinition, LPCTSTR sReason) {
	CString sEntry;
	sEntry.Format(_T("%s -- %s"), sDefinition, sReason);
	m_rejectedShortcuts.push_back(sEntry);
}

// Show the collected rejections once. An empty list shows nothing, so a
// clean KeyMap.txt stays silent; the list is capped so a hostile file
// cannot flood the screen. The text is English on purpose: no
// strings-file entry, no translations.
void CKeyMap::ShowRejectedShortcuts() {
	if (m_rejectedShortcuts.empty()) {
		return;
	}
	const size_t kMaxShown = 20;
	CString sMessage = _T("JPEGView ignored the following shortcut definitions:");
	for (size_t i = 0; i < m_rejectedShortcuts.size() && i < kMaxShown; i++) {
		sMessage += _T("\n");
		sMessage += m_rejectedShortcuts[i];
	}
	if (m_rejectedShortcuts.size() > kMaxShown) {
		CString sMore;
		sMore.Format(_T("\n... and %u more ignored definitions."), (unsigned)(m_rejectedShortcuts.size() - kMaxShown));
		sMessage += sMore;
	}
	sMessage += _T("\n\nCheck KeyMap.txt and the INI file (OcrShortcut) and fix or remove them.");
	if (m_nKeyMapRewriteResult == 1) {
		sMessage += _T("\nRejected KeyMap.txt lines have been commented out (//) in the file.");
	} else if (m_nKeyMapRewriteResult == -1) {
		sMessage += _T("\nKeyMap.txt could not be updated; rejected lines were left unchanged.");
	}
	::MessageBox(NULL, sMessage, _T("JPEGView"), MB_OK | MB_ICONWARNING);
}

// Comment out the rejected KeyMap.txt lines in the user's file, in place.
// The file is read as raw bytes and rewritten through a temp file in the
// same directory, then moved over the original, so the original encoding
// and line endings survive untouched; only "//" is inserted at the start
// of each rejected line. Lines that were already comments are never in
// the rejected list (the parser skips them), so no line is double-commented.
//
// The rewrite is refused and the file left untouched (result -1) when any
// physical line holds more than 254 content bytes: _fgetts with BUFF_LEN 256
// reads at most 255 characters, so a longer line would already have been split
// into several chunks by the parser and chunk numbers would no longer match
// physical line numbers. When every line is at most 254 bytes, each chunk the
// parser saw is exactly one physical line, in both ANSI and Unicode builds
// (a line's wide-character count never exceeds its byte count). A UTF-8 BOM,
// if present, stays at the front of the file; a UTF-16 BOM aborts the rewrite.
void CKeyMap::CommentOutRejectedLines(LPCTSTR sPath) {
	m_nKeyMapRewriteResult = -1;
	if (sPath == NULL || m_rejectedKeyMapLines.empty()) {
		return;
	}
	FILE* f = _tfopen(sPath, _T("rb"));
	if (f == NULL) {
		return;
	}
	fseek(f, 0, SEEK_END);
	long nSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (nSize < 0) {
		fclose(f);
		return;
	}
	std::vector<char> bytes((size_t)nSize);
	if (nSize > 0 && fread(&bytes[0], 1, (size_t)nSize, f) != (size_t)nSize) {
		fclose(f);
		return;
	}
	fclose(f);

	// UTF-16 files would not have parsed as text anyway; do not byte-edit them.
	size_t nStart = 0;
	if (nSize >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) return;
	if (nSize >= 2 && (unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF) return;
	if (nSize >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) nStart = 3;

	// Split into physical lines, keeping each line's ending.
	struct LineSpan { size_t nBegin; size_t nEnd; };
	std::vector<LineSpan> lines;
	size_t nPos = nStart;
	while (nPos < (size_t)nSize) {
		LineSpan span;
		span.nBegin = nPos;
		while (nPos < (size_t)nSize && bytes[nPos] != '\n') nPos++;
		if (nPos < (size_t)nSize) nPos++; // keep the '\n'
		span.nEnd = nPos;
		lines.push_back(span);
	}
	for (size_t i = 0; i < lines.size(); i++) {
		size_t nContentEnd = lines[i].nEnd;
		if (nContentEnd > lines[i].nBegin && bytes[nContentEnd - 1] == '\n') nContentEnd--;
		if (nContentEnd > lines[i].nBegin && bytes[nContentEnd - 1] == '\r') nContentEnd--;
		if (nContentEnd - lines[i].nBegin > 254) {
			return; // long line: chunk numbers no longer match physical lines
		}
	}
	std::vector<char> rejected(lines.size(), 0);
	for (size_t i = 0; i < m_rejectedKeyMapLines.size(); i++) {
		if (m_rejectedKeyMapLines[i] < 0 || (size_t)m_rejectedKeyMapLines[i] >= lines.size()) {
			return; // inconsistent index: fail safe, touch nothing
		}
		rejected[(size_t)m_rejectedKeyMapLines[i]] = 1;
	}

	std::vector<char> out;
	out.reserve(bytes.size() + m_rejectedKeyMapLines.size() * 2);
	for (size_t i = 0; i < nStart; i++) out.push_back(bytes[i]);
	for (size_t i = 0; i < lines.size(); i++) {
		if (rejected[i]) { out.push_back('/'); out.push_back('/'); }
		for (size_t j = lines[i].nBegin; j < lines[i].nEnd; j++) out.push_back(bytes[j]);
	}

	CString sTempPath = CString(sPath) + _T(".tmp");
	FILE* fo = _tfopen(sTempPath, _T("wb"));
	if (fo == NULL) {
		return;
	}
	bool bOk = out.empty() || fwrite(&out[0], 1, out.size(), fo) == out.size();
	fclose(fo);
	if (!bOk) {
		_tremove(sTempPath);
		return;
	}
	if (::MoveFileEx(sTempPath, sPath, MOVEFILE_REPLACE_EXISTING) == 0) {
		_tremove(sTempPath);
		return;
	}
	m_nKeyMapRewriteResult = 1;
}

// 用 INI 中的设置覆盖 KeyMap 里的快捷键。目前只有 OCR。
// 放在 AddDefaultEscapeHandling 里调用，是因为它是构造函数所有出口的公共收尾点，
// 保证无论是加载了用户 KeyMap、默认 KeyMap 还是完全没找到文件，覆盖都会生效。
void CKeyMap::ApplyIniOverrides() {
	CString sOcr = CSettingsProvider::This().OcrShortcut();
	if (!sOcr.IsEmpty()) {
		ApplyShortcutOverride(IDM_OCR, sOcr);
	}
}

void CKeyMap::ApplyShortcutOverride(int nCommandId, LPCTSTR sShortcut) {
	if (sShortcut == NULL || *sShortcut == 0) return;
	CString sBuffer(sShortcut);
	sBuffer.TrimLeft();
	sBuffer.TrimRight();
	if (sBuffer.IsEmpty()) return;

	// _ParseKeys 会把 '+' 就地替换成 0，因此必须给它一块可写缓冲。
	// The display text must be copied before parsing: _ParseKeys rewrites
	// '+' in place, so reading sBuffer afterwards would stop at the first
	// replaced plus sign.
	CString sShortcutText = sBuffer;
	CString sParseReason;
	int nKeyCode = 0;
	{
		LPTSTR pBuffer = sBuffer.GetBuffer(sBuffer.GetLength());
		nKeyCode = _ParseKeys(pBuffer, &sParseReason);
		sBuffer.ReleaseBuffer();
	}
	if (nKeyCode <= 0) {
		RecordRejectedShortcut(CString(_T("INI OcrShortcut '")) + sShortcutText + _T("'"), sParseReason.IsEmpty() ? _T("invalid shortcut") : (LPCTSTR)sParseReason);
		return;
	}

	// Esc 和鼠标左键承载基本交互（退出当前模式、平移、裁剪）。让配置把它们
	// 抢走会把用户困在一个没有常规出口的模式里，直接拒绝。
	if (nKeyCode == VK_ESCAPE || nKeyCode == VK_LBUTTON) {
		OutputDebugString(_T("JPEGView: OcrShortcut may not use Esc or the left mouse button; ignored.\n"));
		RecordRejectedShortcut(CString(_T("INI OcrShortcut '")) + sShortcutText + _T("'"), _T("Esc and the left mouse button carry basic interaction and cannot be rebound"));
		return;
	}

	// 不要静默顶掉别的命令。原来的写法直接覆盖 m_keyMap[nKeyCode]，把
	// OcrShortcut 设为 Ctrl+C 就会悄悄删掉“复制图像”的绑定。冲突时保留
	// 原命令，并通过调试输出留下记录。
	stdext::hash_map<int, int>::const_iterator existing = m_keyMap.find(nKeyCode);
	if (existing != m_keyMap.end() && existing->second != nCommandId) {
		OutputDebugString(_T("JPEGView: OcrShortcut conflicts with another command; ignored.\n"));
		RecordRejectedShortcut(CString(_T("INI OcrShortcut '")) + sShortcutText + _T("'"), _T("conflicts with another command; the existing binding is kept"));
		return;
	}

	// 先移除该命令已有的映射，否则同一个命令会同时响应 KeyMap.txt 里的旧键和新键。
	for (stdext::hash_map<int, int>::iterator it = m_keyMap.begin(); it != m_keyMap.end(); ) {
		if (it->second == nCommandId) {
			stdext::hash_map<int, int>::iterator toErase = it++;
			m_keyMap.erase(toErase);
		} else {
			++it;
		}
	}
	m_keyMap[nKeyCode] = nCommandId;
}

int CKeyMap::GetCommandIdForKey(int nVirtualKeyCode, bool bAlt, bool bCtrl, bool bShift) {
	int nKeyCode = GetCombinedKeyCode(nVirtualKeyCode, bAlt, bCtrl, bShift);
	stdext::hash_map<int, int>::const_iterator iter = m_keyMap.find(nKeyCode);
	if (iter == m_keyMap.end()) {
		return -1;
	} else {
		return iter->second;
	}
}

CString CKeyMap::GetKeyStringForCommand(int nCommandId) {
	stdext::hash_map<int, int>::const_iterator iter = m_keyMap.begin();
	std::list<CString> keys;
	while (iter != m_keyMap.end()) {
		if (nCommandId == iter->second) {
			CString sShortCutName = _GetKeyShortcutName(iter->first);
			if (!sShortCutName.IsEmpty()) {
				if (sShortCutName[0] == _T('P')) {
					keys.push_front(sShortCutName);
				} else {
					keys.push_back(sShortCutName);
				}
			}
		}
		iter++;
	}

	CString sKeyDesc;
	for (std::list<CString>::iterator iter = keys.begin(); iter != keys.end(); iter++) {
		if (!sKeyDesc.IsEmpty()) {
			sKeyDesc += _T("/");
		}
		sKeyDesc += *iter;
	}

	return sKeyDesc;
}