#pragma once

#include <hash_map>
#include <vector>

#define VK_LBUTTONDBLCLK 7

// KeyMap
class CKeyMap
{
public:
	// Reads the keymap file with logic for loading from different locations, including the default
	CKeyMap();

	// Gets a command Id for a keycode and modifier keys. Returns -1 if no command available for this key.
	int GetCommandIdForKey(int nVirtualKeyCode, bool bAlt, bool bCtrl, bool bShift);

	// Gets the string describing the keyboard shortcut for a given command, empty string if none
	CString GetKeyStringForCommand(int nCommandId);

	// Returns the virtual key code for a key name of the form 'Alt+A'.
	// Returns -1 if key name not valid
	static int GetVirtualKeyCode(LPCTSTR keyName);

	// gets a combined key code
	static int GetCombinedKeyCode(int keyCode, bool alt, bool control, bool shift);

	// gets the shortcut key name, e.g. 'Ctrl+P'
	static CString GetShortcutKey(int combinedKeyCode);
private:

	// key is the key code, value the command ID
	stdext::hash_map<int, int> m_keyMap;

	// Shortcut definitions rejected while loading (KeyMap.txt lines,
	// displaced bindings, INI overrides), each with its reason. Shown once
	// at the end of construction so a bad shortcut is visible instead of
	// silently dead. English text on purpose: no strings-file entry.
	std::vector<CString> m_rejectedShortcuts;

	// Physical line numbers of the loaded user KeyMap.txt rejected by
	// _ParseKeys (invalid token or dead mapping). After loading, those
	// lines are commented out in the user's file (CommentOutRejectedLines).
	// Displaced bindings are not in here: the later line of a duplicate
	// binding is live, so commenting it would break a working binding.
	// INI overrides are not in here either: they are not KeyMap.txt lines.
	std::vector<int> m_rejectedKeyMapLines;

	// KeyMap.txt rewrite outcome: 0 = not attempted (no user file loaded,
	// or nothing rejected), 1 = rejected lines commented out,
	// -1 = rewrite attempted but failed (file left untouched).
	int m_nKeyMapRewriteResult = 0;

	void RecordRejectedShortcut(LPCTSTR sDefinition, LPCTSTR sReason);
	void ShowRejectedShortcuts();
	void CommentOutRejectedLines(LPCTSTR sPath);

	void AddDefaultEscapeHandling();
	// 应用 INI 中的快捷键覆盖（见 SettingsProvider::OcrShortcut）
	void ApplyIniOverrides();
	// 把某个命令绑定到指定快捷键字符串（如 "Ctrl+Q"），替换它原有的映射
	void ApplyShortcutOverride(int nCommandId, LPCTSTR sShortcut);
};
