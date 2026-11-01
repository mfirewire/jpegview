#pragma once

#include <unordered_map>

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
	std::unordered_map<int, int> m_keyMap;

	void AddDefaultEscapeHandling();
	// 应用 INI 中的快捷键覆盖（见 SettingsProvider::OcrShortcut）
	void ApplyIniOverrides();
	// 把某个命令绑定到指定快捷键字符串（如 "Ctrl+Q"），替换它原有的映射
	void ApplyShortcutOverride(int nCommandId, LPCTSTR sShortcut);
};
