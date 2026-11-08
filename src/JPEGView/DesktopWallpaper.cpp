#include "StdAfx.h"
#include "DesktopWallpaper.h"
#include "JPEGImage.h"
#include "SaveImage.h"
#include "MultiMonitorSupport.h"
#include "SettingsProvider.h"

namespace SetDesktopWallpaper {

	// Sets a string value in the registry in the path HKEY_CURRENT_USER\Control Panel\Desktop
	static bool SetRegistryStringValue(LPCTSTR name, LPCTSTR stringValue) {
		HKEY key;
		if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Control Panel\\Desktop"), 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
			return false;
		// E1 (F-L01): the key was never closed, so every wallpaper change
		// leaked one handle. Close it on both outcomes.
		LSTATUS status = RegSetValueEx(key, name, 0, REG_SZ, (const BYTE *)stringValue, ((int)_tcslen(stringValue) + 1) * sizeof(TCHAR));
		RegCloseKey(key);
		return status == ERROR_SUCCESS;
	}

	// F-M02: validate first, then set the registry, then apply. The old
	// order wrote WallpaperStyle/TileWallpaper before the path was even
	// checked, so a failed wallpaper change still altered the desktop
	// style with no error. Returns false on failure; the registry is
	// untouched on the validation/save paths below.
	bool SetFileAsWallpaper(CJPEGImage& image, LPCTSTR fileName)
	{
		if (fileName == NULL || fileName[0] == 0 || ::GetFileAttributes(fileName) == INVALID_FILE_ATTRIBUTES) {
			::OutputDebugString(_T("JPEGView: SetFileAsWallpaper called with a missing file\n"));
			return false;
		}
		CRect largestMonitor = CMultiMonitorSupport::GetMonitorRect(-1);
		bool needsFitToScreen = image.InitOrigWidth() > largestMonitor.Width() || image.InitOrigHeight() > largestMonitor.Height();
		SetRegistryStringValue(_T("WallpaperStyle"), needsFitToScreen ? _T("6") : _T("0"));
		SetRegistryStringValue(_T("TileWallpaper"), _T("0"));

		void* parameter = (void*)fileName;
		if (::SystemParametersInfo(SPI_SETDESKWALLPAPER,
			0,
			parameter,
			SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE) == 0) {
			::OutputDebugString(_T("JPEGView: SystemParametersInfo(SPI_SETDESKWALLPAPER) failed\n"));
			return false;
		}
		return true;
	}

	bool SetProcessedImageAsWallpaper(CJPEGImage& image)
	{
		LPCTSTR path = CSettingsProvider::This().WallpaperPath();
		if (path == NULL || path[0] == 0) {
			return false;
		}

		CString fileName = CString(path) + _T("\\JPEGViewWallpaper.bmp");

		if (!CSaveImage::SaveImage(fileName, &image, false)) {
			return false;
		}

		bool bitmapMatchesDesktop = false;
		int windowsVersion = Helpers::GetWindowsVersion();
		LPCTSTR wallpaperStyle = _T("0");
		LPCTSTR tileWallpaper = _T("0");
		if (windowsVersion >= 601) {
			// Check if image spans all screens
			CRect allScreens = CMultiMonitorSupport::GetVirtualDesktop();
			bitmapMatchesDesktop = (allScreens.Size() == CSize(image.DIBWidth(), image.DIBHeight()));
			if (bitmapMatchesDesktop && windowsVersion >= 602) wallpaperStyle = _T("22"); // for Windows 8 ff
			if (bitmapMatchesDesktop && windowsVersion == 601) tileWallpaper = _T("1"); // for Windows 7
		}
		SetRegistryStringValue(_T("WallpaperStyle"), wallpaperStyle);
		SetRegistryStringValue(_T("TileWallpaper"), tileWallpaper);

		void* parameter = (void*)(LPCTSTR)fileName;
		if (::SystemParametersInfo(SPI_SETDESKWALLPAPER,
			0,
			parameter,
			SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE) == 0) {
			::OutputDebugString(_T("JPEGView: SystemParametersInfo(SPI_SETDESKWALLPAPER) failed\n"));
			return false;
		}
		return true;
	}
}