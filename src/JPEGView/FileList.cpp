#include "StdAfx.h"
#include "FileList.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include "DirectoryWatcher.h"
#include "Shlwapi.h"
#include <stdlib.h>
#include <new> // ST-10: std::nothrow

///////////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////////

// static initializers
Helpers::ESorting CFileDesc::sm_eSorting = Helpers::FS_LastModTime;
bool CFileDesc::sm_bSortAscending = true;
Helpers::ENavigationMode CFileList::sm_eMode = Helpers::NM_LoopDirectory;

// Helper to add the current file of filefind object to the list
static void AddToFileList(std::list<CFileDesc> & fileList, CFindFile & fileFind, LPCTSTR expectedExtension) {
	if (!fileFind.IsDirectory()) {
		if (expectedExtension != NULL) {
			// compare if the extension is the expected extension
			// this is required because a strange behavior of CFindFile: If searching for "*.tif", .tiff files are also found
			CString fileName = fileFind.GetFileName();
			int lastDot = fileName.ReverseFind(_T('.'));
			if (_tcsicmp(((LPCTSTR)fileName) + lastDot + 1, expectedExtension) != 0) {
				return;
			}
		}
		FILETIME lastWriteTime, creationTime;
		fileFind.GetLastWriteTime(&lastWriteTime);
		fileFind.GetCreationTime(&creationTime);
		CFileDesc thisFile(fileFind.GetFilePath(), &lastWriteTime, &creationTime, fileFind.GetFileSize());
		fileList.push_back(thisFile);
	}
}

static bool s_bUseLogicalStringCompare = true;
static bool s_bUseLogicalStringCompareValid = false;

// Used by method below
static bool IsNoLogicalStrCmpSetInRegistryHive(HKEY hKeyRoot) {
	HKEY key;
	if (RegOpenKeyEx(hKeyRoot, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer"), 0, KEY_READ, &key) == ERROR_SUCCESS) {
		DWORD value;
		DWORD size = sizeof(DWORD);
		DWORD type;
		if (RegQueryValueEx(key, _T("NoStrCmpLogical"), NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
			if (type == REG_DWORD) {
				RegCloseKey(key);
				return value == 1;
			}
		}
		RegCloseKey(key);
	}
	return false;
}

// Queries the registry if logical string comparing (file9.txt before file10.txt) shall be used.
// This is default in Windows XP and later.
static bool UseLogicalStringCompare() {
	if (!s_bUseLogicalStringCompareValid) {
		s_bUseLogicalStringCompareValid = true;
		s_bUseLogicalStringCompare = !(IsNoLogicalStrCmpSetInRegistryHive(HKEY_LOCAL_MACHINE) || 
			IsNoLogicalStrCmpSetInRegistryHive(HKEY_CURRENT_USER));
	}
	return s_bUseLogicalStringCompare;
}

///////////////////////////////////////////////////////////////////////////////////
// CFileDesc
///////////////////////////////////////////////////////////////////////////////////

CFileDesc::CFileDesc(const CString & sName, const FILETIME* lastModTime, const FILETIME* creationTime, __int64 fileSize) {
	m_sName = sName;
	m_sTitle = (LPCTSTR)m_sName + sName.ReverseFind(_T('\\')) + 1;
	memcpy(&m_lastModTime, lastModTime, sizeof(FILETIME));
	memcpy(&m_creationTime, creationTime, sizeof(FILETIME));
	m_nRandomOrderNumber = rand();
	m_fileSize = fileSize;
}

bool CFileDesc::SortAscending(const CFileDesc& other) const {
	if (sm_eSorting == Helpers::FS_CreationTime || sm_eSorting == Helpers::FS_LastModTime) {
		const FILETIME* pTime = (sm_eSorting == Helpers::FS_LastModTime) ? &m_lastModTime : &m_creationTime;
		const FILETIME* pTimeOther = (sm_eSorting == Helpers::FS_LastModTime) ? &(other.m_lastModTime) : &(other.m_creationTime);
		if (pTime->dwHighDateTime < pTimeOther->dwHighDateTime) {
			return true;
		} else if (pTime->dwHighDateTime > pTimeOther->dwHighDateTime) {
			return false;
		} else {
			return pTime->dwLowDateTime < pTimeOther->dwLowDateTime;
		}
	} else if (sm_eSorting == Helpers::FS_Random) {
		return m_nRandomOrderNumber < other.m_nRandomOrderNumber;
	} else if (sm_eSorting == Helpers::FS_FileSize) {
		return m_fileSize < other.m_fileSize;
	} else {
		if (UseLogicalStringCompare()) {
			// If the filename contains numbers, we want to sort the files
			// according to the numbers to place 'File9' before 'File10'
			return StrCmpLogicalW(m_sTitle, other.m_sTitle) < 0;
		} else {
			return _tcsicoll(m_sTitle, other.m_sTitle) < 0;
		}
	}
}

bool CFileDesc::operator < (const CFileDesc& other) const {
	// Must be a strict weak ordering: with the old "SortAscending(other) ^ invert"
	// form two files comparing equal in descending order were both less than each
	// other, which std::sort is allowed to turn into out-of-bounds writes and
	// list::sort into an arbitrary order.
	if (SortAscending(other)) {
		return sm_bSortAscending;
	}
	if (other.SortAscending(*this)) {
		return !sm_bSortAscending;
	}
	return false; // equal sort keys
}


void CFileDesc::SetName(LPCTSTR sNewName) {
	m_sName = sNewName;
	m_sTitle = (LPCTSTR)m_sName + m_sName.ReverseFind(_T('\\')) + 1;
}

void CFileDesc::SetModificationDate(const FILETIME& lastModDate) {
	memcpy(&m_lastModTime, &lastModDate, sizeof(FILETIME));
}

///////////////////////////////////////////////////////////////////////////////////
// Public interface
///////////////////////////////////////////////////////////////////////////////////

// image file types supported internally (there are additional endings for RAW, WIC and Extra - these come from INI file)
// NOTE: when adding more supported filetypes, update installer to add another extension for "SupportedTypes"
static const int cnNumEndingsInternal = 45;
static const TCHAR* csFileEndingsInternal[cnNumEndingsInternal] = {_T("jpg"), _T("jpeg"), _T("jpe"), _T("jfif"), _T("jps"), _T("jfi"), _T("jif"), _T("mpo"), _T("bmp"), _T("png"), _T("apng"),
	_T("tif"), _T("tiff"), _T("gif"), _T("webp"), _T("jxl"), _T("avif"), _T("avifs"), _T("avis"), _T("heif"), _T("heic"), _T("hif"), _T("tga"), _T("icb"), _T("vda"), _T("vst"), _T("qoi"), _T("psd"), _T("psb"), _T("ppm"), _T("pgm"), _T("pnm"), 
	_T("ico"), _T("cur"), _T("wmf"), _T("emf"), _T("hdr"), _T("rgbe"), _T("pbm"), _T("pam"), _T("jp2"), _T("j2k"), _T("j2c"), _T("exr"), _T("svg") };
// supported camera RAW formats
static const TCHAR* csFileEndingsRAW = _T("*.pef;*.dng;*.crw;*.nef;*.cr2;*.mrw;*.rw2;*.orf;*.x3f;*.arw;*.kdc;*.nrw;*.dcr;*.sr2;*.srf;*.raf;*.kc2;*.erf;*.3fr;*.raw;*.mef;*.mos;*.mdc;*.cr3;*.srw");


static const int MAX_ENDINGS = 128; // 45 internal + 5 WIC + 25 RAW = 75, plus room for FileEndingsExtra
static int nNumEndings;
static LPCTSTR* sFileEndings;

__declspec(dllimport) bool __stdcall WICPresent(void);

// Check if Windows Image Codecs library is present
static bool WICPresentGuarded(void) {
	try {
		return WICPresent();
	} catch (...) {
		return false;
	}
}

// Parses a semicolon separated list of file endings of the form "*.nef;*.cr2;*.dng"
// Returns true if at least one new ending was added. F-H03 (contained
// part only): duplicates are skipped case-
// insensitively instead of consuming a slot, and an ending dropped
// because the list is full is reported with OutputDebugString (no
// MessageBox: this runs during list construction).
static bool ParseAndAddFileEndings(LPCTSTR sEndings) {
	bool bAdded = false;
	bool bWarnedFull = false;
	if (_tcslen(sEndings) > 2) {
		LPTSTR buffer = new TCHAR[_tcslen(sEndings) + 1]; // this buffer will not be freed deliberately!
		_tcscpy(buffer, sEndings);
		LPTSTR sStart = buffer, sCurrent = buffer;
		while (*sCurrent != 0) {
			while (*sCurrent != 0 && *sCurrent != _T(';')) {
				sCurrent++;
			}
			if (*sCurrent == _T(';')) {
				*sCurrent = 0;
				sCurrent++;
			}
			if (_tcslen(sStart) > 2) {
				LPCTSTR sEnding = sStart + 2;
				bool bDuplicate = false;
				for (int i = 0; i < nNumEndings; i++) {
					if (_tcsicmp(sFileEndings[i], sEnding) == 0) {
						bDuplicate = true;
						break;
					}
				}
				if (bDuplicate) {
					// already in the list: do not consume a slot
				} else if (nNumEndings < MAX_ENDINGS) {
					sFileEndings[nNumEndings++] = sEnding;
					bAdded = true;
				} else if (!bWarnedFull) {
					CString sMsg;
					sMsg.Format(_T("JPEGView: file ending list full (%d), dropping '%s'\n"), MAX_ENDINGS, sEnding);
					::OutputDebugString(sMsg);
					bWarnedFull = true;
				}
			}
			sStart = sCurrent;
		}
	}
	return bAdded;
}

// Gets all supported file endings, including the ones from WIC.
// The length of the returned list is nNumEndings
static LPCTSTR* GetSupportedFileEndingList() {
	if (sFileEndings == NULL) {
		sFileEndings = new LPCTSTR[MAX_ENDINGS];
		for (nNumEndings = 0; nNumEndings < cnNumEndingsInternal; nNumEndings++) {
			sFileEndings[nNumEndings] = csFileEndingsInternal[nNumEndings];
		}

		LPCTSTR sFileEndingsWIC = CSettingsProvider::This().FilesProcessedByWIC();
		if (_tcslen(sFileEndingsWIC) > 2 && WICPresentGuarded()) {
			ParseAndAddFileEndings(sFileEndingsWIC);
		}
		ParseAndAddFileEndings(CSettingsProvider::This().FileEndingsRAW());
		ParseAndAddFileEndings(CSettingsProvider::This().FileEndingsExtra());
	}
	return sFileEndings;
}

// E4: sorted index over the supported file endings, for O(log n) lookup
// instead of a linear scan (up to MAX_ENDINGS entries) per file in
// FindFiles / per IsImageFile call. The entries point into the same
// storage as sFileEndings, so no ending string is ever copied; the index
// is rebuilt whenever ParseAndAddFileEndings grows the list (e.g. a
// temporarily added RAW ending, O-05), detected by the count. The list
// only ever grows and duplicates are skipped case-insensitively when
// adding, so the count is enough to detect staleness.
struct SFileEndingEntry {
	LPCTSTR pszEnding; // extension without dot, e.g. "nef"
	int nOriginalIndex; // position in sFileEndings (E9 match priority)
};
static SFileEndingEntry* sSortedFileEndings = NULL; // never freed, like sFileEndings
static int s_nSortedFileEndings = -1; // nNumEndings the index was built from

static int __cdecl CompareFileEndings(const void* pA, const void* pB) {
	return _tcsicmp(((const SFileEndingEntry*)pA)->pszEnding, ((const SFileEndingEntry*)pB)->pszEnding);
}

// ST-10: returns false when the sorted index cannot be built (out of
// memory). The old plain new threw bad_alloc to the UI thread; with
// nothrow the caller fails closed instead (FindSupportedEndingRun
// reports "no match", so FindFiles lists nothing and IsImageFile
// returns false) and retries the allocation on the next call, since
// s_nSortedFileEndings stays -1.
static bool EnsureSortedFileEndings() {
	LPCTSTR* allFileEndings = GetSupportedFileEndingList();
	if (sSortedFileEndings == NULL) {
		sSortedFileEndings = new(std::nothrow) SFileEndingEntry[MAX_ENDINGS];
		if (sSortedFileEndings == NULL) {
			return false;
		}
	}
	if (s_nSortedFileEndings != nNumEndings) {
		for (int i = 0; i < nNumEndings; i++) {
			sSortedFileEndings[i].pszEnding = allFileEndings[i];
			sSortedFileEndings[i].nOriginalIndex = i;
		}
		qsort(sSortedFileEndings, nNumEndings, sizeof(SFileEndingEntry), CompareFileEndings);
		s_nSortedFileEndings = nNumEndings;
	}
	return true;
}

// Compares an ending against a (pointer, length) extension run, as used by
// the slideshow-list scan (E9), where the run is not NUL-terminated.
// Identical ordering to _tcsicmp (first differing character, else the
// shorter string is smaller), so binary search over the sorted index
// stays valid.
static int CompareEndingToRun(LPCTSTR pszEnding, LPCTSTR pRun, size_t nRunLen) {
	size_t nEndingLen = _tcslen(pszEnding);
	size_t nCommon = (nEndingLen < nRunLen) ? nEndingLen : nRunLen;
	int nCmp = _tcsnicmp(pszEnding, pRun, nCommon);
	if (nCmp != 0) {
		return nCmp;
	}
	if (nEndingLen < nRunLen) {
		return -1;
	}
	if (nEndingLen > nRunLen) {
		return 1;
	}
	return 0;
}

// Binary search over the sorted index for an extension run (E9) or a
// NUL-terminated extension (E4). Returns the entry, or NULL.
static const SFileEndingEntry* FindSupportedEndingRun(LPCTSTR pRun, size_t nRunLen) {
	if (!EnsureSortedFileEndings()) {
		return NULL; // ST-10: no index (OOM) -> no match, fail closed
	}
	int nLow = 0;
	int nHigh = s_nSortedFileEndings - 1;
	while (nLow <= nHigh) {
		int nMid = (nLow + nHigh) / 2;
		int nCmp = CompareEndingToRun(sSortedFileEndings[nMid].pszEnding, pRun, nRunLen);
		if (nCmp == 0) {
			return &sSortedFileEndings[nMid];
		} else if (nCmp < 0) {
			nLow = nMid + 1;
		} else {
			nHigh = nMid - 1;
		}
	}
	return NULL;
}

// E4: case-insensitive exact match of an extension (without dot) against
// the supported endings. Replaces the linear _tcsicmp scan in FindFiles
// and the MakeLower copy in IsImageFile; unlike the old IsImageFile it
// also matches when the ini lists an ending in upper case, which
// FindFiles already listed.
static bool IsSupportedFileEnding(LPCTSTR sEnding) {
	if (sEnding == NULL || *sEnding == 0) {
		return false;
	}
	return FindSupportedEndingRun(sEnding, _tcslen(sEnding)) != NULL;
}

// E9: scans one slideshow-list line for a supported extension, in place.
// At each '.', the following run of extension characters (letters/digits)
// is looked up in the shared sorted suffix set (E4) instead of running
// stristr() once per suffix, which copied the whole line on every call.
// Returns a pointer to the '.' of the winning match, or NULL. Among
// several matches the ending earliest in the supported-endings list wins,
// as the old per-suffix loop did (first suffix with any occurrence, and
// its first occurrence in the line). One intentional difference from the
// old stristr loop, narrower: a ".jpgx" run no longer matches ".jpg"
// (the old code then extracted the whole "photo.jpgx" token and added
// the file if it existed on disk, even though its extension is not
// supported at all).
static TCHAR* FindSlideShowEndingDot(TCHAR* pLine) {
	const SFileEndingEntry* pBest = NULL;
	TCHAR* pBestDot = NULL;
	for (TCHAR* pDot = pLine; *pDot != 0; pDot++) {
		if (*pDot != _T('.')) {
			continue;
		}
		LPCTSTR pRun = pDot + 1;
		size_t nRunLen = 0;
		while (pRun[nRunLen] != 0) {
			TCHAR c = pRun[nRunLen];
			if ((c >= _T('A') && c <= _T('Z')) || (c >= _T('a') && c <= _T('z')) || (c >= _T('0') && c <= _T('9'))) {
				nRunLen++;
			} else {
				break;
			}
		}
		if (nRunLen == 0) {
			continue;
		}
		const SFileEndingEntry* pEntry = FindSupportedEndingRun(pRun, nRunLen);
		if (pEntry != NULL && (pBest == NULL || pEntry->nOriginalIndex < pBest->nOriginalIndex)) {
			pBest = pEntry;
			pBestDot = pDot;
		}
	}
	return pBestDot;
}

CFileList::CFileList(const CString & sInitialFile, CDirectoryWatcher & directoryWatcher, 
	Helpers::ESorting eInitialSorting, bool isSortedAscending, bool bWrapAroundFolder, int nLevel, bool forceSorting)
	: m_directoryWatcher(directoryWatcher), m_nCurrentIndex(-1) {

	CFileDesc::SetSorting(eInitialSorting, isSortedAscending);
	m_bDeleteHistory = true;
	m_bWrapAroundFolder = bWrapAroundFolder;
	m_sInitialFile = sInitialFile;
	m_nLevel = nLevel;
	m_next = m_prev = NULL;
	int nPos = sInitialFile.ReverseFind(_T('\\'));
	m_sDirectory = (nPos > 0) ? sInitialFile.Left(nPos) : _T(""); // the backslash is stripped away!
	nPos = sInitialFile.ReverseFind(_T('.'));
	DWORD dwInitialFileAttributes = ::GetFileAttributes(sInitialFile);
	// Check for INVALID_FILE_ATTRIBUTES first: its 0xFFFFFFFF value has every
	// attribute bit set, so a path that does not exist used to be taken for a
	// directory and the caller ended up listing the parent folder.
	bool bIsDirectory = dwInitialFileAttributes != INVALID_FILE_ATTRIBUTES && (dwInitialFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	CString sExtensionInitialFile = (nPos > 0) ? sInitialFile.Right(sInitialFile.GetLength()-nPos-1) : _T("");
	sExtensionInitialFile.MakeLower();
	bool bImageFile = !bIsDirectory && IsImageFile(sExtensionInitialFile);
	// O-05: match whole ';'-separated segments, not a prefix substring:
	// _tcsstr("*.sr2;...", "*.sr") matched, so a "*.sr" file was wrongly
	// treated as a supported RAW file. IsInFileEndingList (Helpers.cpp,
	// F-12) compares the ".ext" form against each "*.ext" token.
	CString sDottedInitialExtension = _T(".") + sExtensionInitialFile;
	if (!bIsDirectory && !bImageFile && !sExtensionInitialFile.IsEmpty() && Helpers::IsInFileEndingList(csFileEndingsRAW, (LPCTSTR)sDottedInitialExtension + 1)) {
		// initial file is a supported camera raw file but was excluded in INI file - add temporarily to file ending list
		// F-H03: only claim the file is an image file when the extension
		// was actually added. If the list is full the file must fall
		// through to the single-file branch instead of FindFiles plus
		// the first-image fallback.
		if (ParseAndAddFileEndings(_T("*.") + sExtensionInitialFile)) {
			CSettingsProvider::This().AddTemporaryRAWFileEnding(sExtensionInitialFile);
			bImageFile = true;
		}
	}
	m_bIsSlideShowList = !bIsDirectory && !bImageFile && TryReadingSlideShowList(sInitialFile);
	m_nMarkedIndexShow = -1;

	m_directoryWatcher.SetCurrentDirectory(m_sDirectory);

	if (!m_bIsSlideShowList) {
		if (bImageFile || bIsDirectory) {
			FindFiles();
			std::list<CFileDesc>::iterator iterInitial = FindFile(sInitialFile);
			// F-H02: a directory path is never an entry of its own listing,
			// so opening a folder falls back to the first image (also in
			// wrap mode, where TryCreateFileList opens the next folder this
			// way). An image file that is not in the list stays at end()
			// and shows the empty state instead of silently jumping to
			// the first image.
			if (iterInitial == m_fileList.end() && bIsDirectory) {
				iterInitial = m_fileList.begin();
			}
			SetCurrentIter(iterInitial);
			m_iterStart = bWrapAroundFolder ? m_iter : m_fileList.begin();
			// F-H02: an end() anchor would make Next()'s folder-finished check
			// unreachable (iterTemp wraps to begin() before the comparison),
			// so Next() would cycle inside the folder forever instead of
			// advancing to the next folder. Anchor a missing file at begin();
			// the displayed image is m_iter (still end(), empty state) -- only
			// the navigation anchor moves.
			if (bWrapAroundFolder && m_iterStart == m_fileList.end()) {
				m_iterStart = m_fileList.begin();
			}
		} else {
			// neither image file nor directory nor list of file names - try to read anyway but normally will fail
			CFindFile fileFind;
			if (fileFind.FindFile(sInitialFile)) {
				AddToFileList(m_fileList, fileFind, NULL);
			}
			m_iter = m_iterStart = m_fileList.begin();
			m_nCurrentIndex = m_fileList.empty() ? -1 : 0;
		}
	} else {
		sm_eMode = Helpers::NM_LoopDirectory;
		if (forceSorting) m_fileList.sort();
		m_iter = m_iterStart = m_fileList.begin();
		m_nCurrentIndex = m_fileList.empty() ? -1 : 0;
	}
}

CFileList::~CFileList() {
	if (m_bDeleteHistory) {
		DeleteHistory();
	}
	m_fileList.clear();
}

CString CFileList::GetSupportedFileEndings() {
	LPCTSTR* allFileEndings = GetSupportedFileEndingList();
	CString sList;
	for (int i = 0; i < nNumEndings; i++) {
		sList += _T("*.");
		sList += allFileEndings[i];
		if (i+1 < nNumEndings) sList += _T(";");
	}
	return sList;
}

void CFileList::Reload(LPCTSTR sFileName, bool clearForwardHistory) {
	LPCTSTR sCurrent = sFileName;
	if (sCurrent == NULL) {
		sCurrent = Current();
		if (sCurrent == NULL) {
			m_fileList.clear();
			return;
		}
	}

	CString sCurrentFile = sCurrent;

	if (clearForwardHistory) {
		// delete the chain of CFileLists forward
		CFileList* pFileList = this->m_next;
		while (pFileList != NULL && pFileList->m_prev != NULL) {
			CFileList* thisList = pFileList;
			pFileList = pFileList->m_next;
			thisList->m_bDeleteHistory = false;
			delete thisList;
		}
		m_next = NULL;
	}

	if (!m_bIsSlideShowList) {
		FindFiles();
		// F-H02: FindFile returns end() when the anchor file is gone; the
		// current file keeps the not-found value (empty state) while
		// m_iterStart is re-anchored at begin() below, so Next() can still
		// detect a completed loop and advance to the next folder.
		m_iterStart = m_bWrapAroundFolder ? FindFile(m_sInitialFile) : m_fileList.begin();
		// An end() anchor would make Next()'s folder-finished check
		// unreachable (iterTemp wraps to begin() first); see the same
		// guard in the constructor.
		if (m_bWrapAroundFolder && m_iterStart == m_fileList.end()) {
			m_iterStart = m_fileList.begin();
		}
	} else {
		VerifyFiles(); // maybe some of the files got deleted or moved
		m_iterStart = m_fileList.begin();
	}
	SetCurrentIter(FindFile(sCurrentFile)); // go again to current file; end() if it left the list (empty state, F-H02)
}

bool CFileList::CurrentFileExists() const {
	if (Current() != NULL) {
		return ::GetFileAttributes(Current()) != INVALID_FILE_ATTRIBUTES;
	}
	return false;
}

bool CFileList::DriveHasRecycleBin(LPCTSTR filePath) {
	TCHAR driveLetter[3];
	if (_tcslen(filePath) < 3) return false;
	driveLetter[0] = filePath[0];
	driveLetter[1] = filePath[1];
	driveLetter[2] = 0;

	if (driveLetter[0] == _T('\\') && driveLetter[1] == _T('\\')) return false; // remote drive, no recycle bin

	CString recycleBin = CString((LPCTSTR(driveLetter))) + _T("\\$Recycle.Bin\\");

	DWORD attributes = ::GetFileAttributes(recycleBin);
	if (attributes == INVALID_FILE_ATTRIBUTES) return false;
	if (attributes & FILE_ATTRIBUTE_DIRECTORY) return true; // this is a directory!

	return false; // this is not a directory!
}

bool CFileList::DeleteFile(LPCTSTR fileNameWithPath) const {
	if (fileNameWithPath != NULL) {
		// append a double null character (SHFileOperation wants a double-null-terminated
		// list). Use a dynamically sized buffer instead of a fixed MAX_PATH stack array so a
		// path longer than MAX_PATH cannot overflow the stack.
		size_t nLen = _tcslen(fileNameWithPath);
		TCHAR* fileName = (TCHAR*) malloc((nLen + 2) * sizeof(TCHAR));
		if (fileName == NULL) {
			return false;
		}
		memcpy(fileName, fileNameWithPath, (nLen + 1) * sizeof(TCHAR));
		fileName[nLen + 1] = 0;

		SHFILEOPSTRUCT fileOp{ 0 };
		fileOp.hwnd = NULL;
		fileOp.wFunc = FO_DELETE;
		fileOp.pFrom = fileName;
		fileOp.pTo = NULL;
		fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
		fileOp.hNameMappings = NULL;
		bool bSuccess = (::SHFileOperation(&fileOp) == 0 && !fileOp.fAnyOperationsAborted);
		free(fileName);
		if (bSuccess) {
			return true;
		}
	}
	return false;
}

void CFileList::FileHasRenamed(LPCTSTR sOldFileName, LPCTSTR sNewFileName) {
	if (_tcsicmp(sOldFileName, m_sInitialFile) == 0) {
		m_sInitialFile = sNewFileName;
	}
	std::list<CFileDesc>::iterator iter;
	for (iter = m_fileList.begin( ); iter != m_fileList.end( ); iter++ ) {
		if (_tcsicmp(sOldFileName, iter->GetName()) == 0) {
			iter->SetName(sNewFileName);
		}
	}
}

void CFileList::ModificationTimeChanged() {
	if (m_iter != m_fileList.end()) {
		LPCTSTR sName = m_iter->GetName();
		HANDLE hFile = ::CreateFile(sName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile != NULL) {
			FILETIME lastModTime;
			if (::GetFileTime(hFile, NULL, NULL, &lastModTime)) {
				m_iter->SetModificationDate(lastModTime);
			}
			::CloseHandle(hFile);
		}
	}
}

CFileList* CFileList::Next() {
	m_nMarkedIndexShow = -1;
	if (m_fileList.size() > 0) {
		std::list<CFileDesc>::iterator iterTemp = m_iter;
		if (iterTemp == m_fileList.end())
			return this;
		iterTemp++;
		if (iterTemp == m_fileList.end()) {
			iterTemp = m_fileList.begin();
		}
		if (iterTemp == m_iterStart) {
			// we are finished with this folder
			if (!m_bWrapAroundFolder && sm_eMode == Helpers::NM_LoopDirectory) {
				return this;
			}
			CFileList* pNextList = WrapToNextImage();
			if (pNextList == NULL) {
				// no next found, go to source of the prev->prev chain
				pNextList = this;
				while (pNextList->m_prev != NULL) pNextList = pNextList->m_prev;
				if (pNextList != this) {
					return m_bWrapAroundFolder ? GotoFirstShown() : this; // stop here, do not wrap around
				}
			}
			if (pNextList != this) {
				// leave current iterator on m_iterStart and return the new list
				return pNextList;
			}
		}
		NextInFolder();
	} else {
		// current filelist is empty, try to go to another folder
		CFileList* pNextList = WrapToNextImage();
		if (pNextList != NULL) {
			return pNextList;
		}
	}
	return this;
}

CFileList* CFileList::Prev() {
	m_nMarkedIndexShow = -1;
	if (m_iter == m_iterStart) {
		if (sm_eMode == Helpers::NM_LoopDirectory) {
			if (!m_bWrapAroundFolder) {
				return this;
			}
			if (m_iter == m_fileList.begin()) {
				MoveIterToLast();
			} else {
				m_iter--;
				m_nCurrentIndex--;
			}
			return this;
		} else {
			return WrapToPrevImage();
		}
	}
	if (m_fileList.size() > 0) {
		if (m_iter == m_fileList.begin()) {
			m_iter = m_fileList.end();
			m_iter--;
			m_nCurrentIndex = (int)m_fileList.size() - 1;
		} else if (m_iter == m_fileList.end()) {
			m_iter--;
			m_nCurrentIndex = (int)m_fileList.size() - 1;
		} else {
			m_iter--;
			m_nCurrentIndex--;
		}
	}
	return this;
}

void CFileList::First() {
	m_nMarkedIndexShow = -1;
	m_iter = m_iterStart = m_fileList.begin();
	m_nCurrentIndex = m_fileList.empty() ? -1 : 0;
}

void CFileList::Last() {
	m_nMarkedIndexShow = -1;
	MoveIterToLast();
	m_iterStart = m_fileList.begin();
}

CFileList* CFileList::AwayFromCurrent() {
	LPCTSTR sCurrentFile = Current();
	LPCTSTR sNextFile = PeekNextPrev(1, true, false);
	if (sCurrentFile == NULL || sNextFile == NULL || _tcscmp(sCurrentFile, sNextFile) == 0) {
		CFileList* pFileList = Prev();
		if (Current() != NULL && sCurrentFile != NULL && _tcscmp(sCurrentFile, Current()) == 0) {
			// not moved away, only one image
			m_iter = m_fileList.end();
			m_nCurrentIndex = -1;
		}
		return pFileList;
	} else {
		return Next();
	}
}

LPCTSTR CFileList::Current() const {
	if (m_iter != m_fileList.end()) {
		return m_iter->GetName();
	} else {
		return NULL;
	}
}

LPCTSTR CFileList::CurrentFileTitle() const {
	LPCTSTR sCurrent = Current();
	if (sCurrent != NULL) {
		LPCTSTR sTitle = _tcsrchr(sCurrent, _T('\\'));
		if (sTitle == NULL) sTitle = sCurrent; else sTitle++;
		return sTitle;
	} else {
		return NULL;
	}
}

LPCTSTR CFileList::CurrentDirectory() const {
	if (m_sDirectory.GetLength() > 0) {
		return m_sDirectory;
	} else {
		return NULL;
	}
}

// E10: (re)establish m_iter together with its cached index. Used where
// m_iter is (re)assigned from a FindFile() result; the walk is O(n) but
// these sites (construction, Reload, SetSorting) already walk the list,
// so the navigation hot path (Next/Prev/First/Last) stays O(1).
void CFileList::SetCurrentIter(std::list<CFileDesc>::iterator iter) {
	m_iter = iter;
	RecalcCurrentIndex();
}

void CFileList::RecalcCurrentIndex() {
	m_nCurrentIndex = -1;
	int i = 0;
	std::list<CFileDesc>::const_iterator iter;
	for (iter = m_fileList.begin( ); iter != m_fileList.end( ); iter++ ) {
		if (iter == m_iter) {
			m_nCurrentIndex = i;
			return;
		}
		i++;
	}
}

int CFileList::CurrentIndex() const {
	// E10: cached index, maintained incrementally wherever m_iter moves
	// (NextInFolder/Prev/First/Last/MoveIterToLast/SetCurrentIter/
	// VerifyFiles). Callers only use this for the "[n/N]" display.
	return m_nCurrentIndex;
}

const FILETIME* CFileList::CurrentModificationTime() const {
	if (m_iter != m_fileList.end()) {
		return &(m_iter->GetLastModTime());
	} else {
		return NULL;
	}
}

LPCTSTR CFileList::PeekNextPrev(int nIndex, bool bForward, bool bToggle) {
	if (bToggle) {
		return (m_nMarkedIndexShow == 0) ? m_sMarkedFile : m_sMarkedFileCurrent;
	} else {
		std::list<CFileDesc>::iterator thisIter = m_iter;
		int nSavedIndex = m_nCurrentIndex;
		LPCTSTR sFileName;
		if (nIndex != 0) {
			CFileList* pFL = bForward ? Next() : Prev();
			sFileName = pFL->PeekNextPrev(nIndex-1, bForward, bToggle);
		} else {
			sFileName = Current();
		}
		m_iter = thisIter;
		m_nCurrentIndex = nSavedIndex;
		return sFileName;
	}
}

void CFileList::SetSorting(Helpers::ESorting eSorting, bool sortAscending) {
	CString sThisFile = (m_iter != m_fileList.end()) ? m_iter->GetName() : "";
	CFileDesc::SetSorting(eSorting, sortAscending);
	// P2: every sort-key press re-enumerates the folder, like Reload():
	// files deleted externally drop out of the list (if it is the
	// current file, FindFile below returns end() and the empty state is
	// shown), and files added externally show up. Costs one directory
	// scan per sort-key press, the same as pressing Ctrl+R. Unconditional
	// on purpose: pressing N while the sort is already FileName+ascending
	// must still refresh, or a deleted current file would stay on screen.
	if (!m_bIsSlideShowList) FindFiles(); else VerifyFiles();
	m_fileList.sort();
	// F-H02: FindFile returns end() when the current file left the list,
	// keeping the empty state instead of silently jumping to the first
	// image; m_iterStart follows m_iter in wrap mode (re-anchored at
	// begin() below when m_iter is end()).
	SetCurrentIter(FindFile(sThisFile));
	m_iterStart = m_bWrapAroundFolder ? m_iter : m_fileList.begin();
	// An end() anchor would make Next()'s folder-finished check
	// unreachable; anchor a missing current file at begin().
	if (m_bWrapAroundFolder && m_iterStart == m_fileList.end()) {
		m_iterStart = m_fileList.begin();
	}
}

Helpers::ESorting CFileList::GetSorting() const {
	return CFileDesc::GetSorting();
}

bool CFileList::IsSortedAscending() const {
	return CFileDesc::IsSortedAscending();
}

void CFileList::SetNavigationMode(Helpers::ENavigationMode eMode) {
	if (m_bIsSlideShowList) {
		return;
	}

	sm_eMode = eMode;
	DeleteHistory();
	m_nLevel = 0;
	m_iterStart = m_bWrapAroundFolder ? m_iter : m_fileList.begin();
	// F-H02: an end() anchor would make Next()'s folder-finished check
	// unreachable; anchor a missing current file at begin().
	if (m_bWrapAroundFolder && m_iterStart == m_fileList.end()) {
		m_iterStart = m_fileList.begin();
	}
}

void CFileList::MarkCurrentFile() {
	LPCTSTR sCurrent = Current();
	if (::GetFileAttributes(sCurrent) != INVALID_FILE_ATTRIBUTES) {
		m_sMarkedFile = CString(sCurrent);
		m_sMarkedFileCurrent = _T("");
		m_nMarkedIndexShow = -1;
	}
}

bool CFileList::FileMarkedForToggle() {
	return !m_sMarkedFile.IsEmpty();
}

void CFileList::ToggleBetweenMarkedAndCurrentFile() {
	if (m_nMarkedIndexShow == -1) {
		m_nMarkedIndexShow = 0;
	}
	LPCTSTR sNewFile = (m_nMarkedIndexShow == 0) ? m_sMarkedFile : m_sMarkedFileCurrent;
	if (sNewFile[0] == 0) {
		return;
	}
	if (m_nMarkedIndexShow == 0) {
		m_sMarkedFileCurrent = Current();
	}

	m_sInitialFile = sNewFile;
	int nPos = m_sInitialFile.ReverseFind(_T('\\'));
	m_sDirectory = (nPos > 0) ? m_sInitialFile.Left(nPos) : _T(""); // the backslash is stripped away!

	DeleteHistory();
	Reload(m_sInitialFile);

	m_nMarkedIndexShow = (m_nMarkedIndexShow + 1) & 1;
}

bool CFileList::CanOpenCurrentFileForReading() const
{
	LPCTSTR sCurrentFile = Current();
	if (sCurrentFile != NULL && sCurrentFile[0] != 0) {
		HANDLE hFile = ::CreateFile(Current(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile != NULL) {
			::CloseHandle(hFile);
			return true;
		}
	}
	return false;
}

void CFileList::DeleteHistory(bool onlyForward) {
	// delete the chain of CFileLists forward and backward and only leave the current node alive
	CFileList* pFileList = this->m_next;
	while (pFileList != NULL && pFileList->m_prev != NULL) {
		CFileList* thisList = pFileList;
		pFileList = pFileList->m_next;
		thisList->m_bDeleteHistory = false; // prevent multiple delete
		delete thisList;
	}
	if (!onlyForward) {
		pFileList = this->m_prev;
		while (pFileList != NULL) {
			CFileList* thisList = pFileList;
			pFileList = pFileList->m_prev;
			thisList->m_bDeleteHistory = false;  // prevent multiple delete
			delete thisList;
		}
		m_prev = NULL;
	}
	m_next = NULL;
	
}

///////////////////////////////////////////////////////////////////////////////////
// Private
///////////////////////////////////////////////////////////////////////////////////

void CFileList::MoveIterToLast() {
	std::list<CFileDesc>::iterator lastIter = m_iter;
	while (m_iter != m_fileList.end()) {
		lastIter = m_iter;
		m_iter++;
	}
	m_iter = lastIter;
	m_nCurrentIndex = (m_iter == m_fileList.end()) ? -1 : (int)m_fileList.size() - 1;
}

std::list<CFileDesc>::iterator CFileList::FindFile(const CString& sName) {
	int nStart = sName.ReverseFind(_T('\\')) + 1;
	if (nStart == sName.GetLength()) {
		return m_fileList.end();
	}
	std::list<CFileDesc>::iterator iter;
	for (iter = m_fileList.begin( ); iter != m_fileList.end( ); iter++ ) {
		if (_tcsicmp((LPCTSTR)sName + nStart, iter->GetTitle()) == 0) {
			return iter;
		}
	}
	return m_fileList.end(); // F-H02: not found is distinguishable from the first image; each caller falls back explicitly
}

CFileList* CFileList::WrapToNextImage() {
	if (m_next != NULL) {
		assert(sm_eMode != Helpers::NM_LoopDirectory);
		return m_next;
	}

	if (sm_eMode == Helpers::NM_LoopDirectory) {
		return NULL;
	} else if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel) {
		int nPos = m_sDirectory.ReverseFind(_T('\\'));
		if (nPos <= 0) {
			return NULL; // root dir - no siblings
		}
		CString sNextDirRoot = m_sDirectory.Left(nPos);
		CString sThisDirTitle = m_sDirectory.Right(m_sDirectory.GetLength() - nPos - 1);
		// collect all sibling folders
		CFindFile fileFind;
		std::list<CString> dirList;
		if (fileFind.FindFile(sNextDirRoot + "\\*")) {
			if (fileFind.IsDirectory() && !fileFind.IsDots()) {
				dirList.push_back(fileFind.GetFileName());
			}
			while (fileFind.FindNextFile()) {
				if (fileFind.IsDirectory() && !fileFind.IsDots()) {
					dirList.push_back(fileFind.GetFileName());
				}
			}
		}
		if (dirList.size() == 0) {
			return NULL; // no sibling folders
		}
		dirList.sort(); // sort is alphabetically

		// now find the current folder and take the next in list
		// to handle the wrap around, two iterations are needed (in case the current folder is the last in the list)
		std::list<CString>::iterator iter;
		bool bFound = false;
		for (int nStep = 0; nStep < 2; nStep++) {
			for (iter = dirList.begin( ); iter != dirList.end( ); iter++ ) {
				if (iter->CompareNoCase(sThisDirTitle) == 0) {
					bFound = true;
				} else if (bFound) {
					CFileList* pNewList = TryCreateFileList(sNextDirRoot + '\\' + *iter + '\\', 0);
					if (pNewList != NULL) {
						return pNewList;
					}
				}
			}
		}
	} else if (sm_eMode == Helpers::NM_LoopSubDirectories) {
		CFileList* pFileList = FindFileRecursively(m_sDirectory, _T(""), false, m_nLevel, 0);
		return pFileList;
	}
	return NULL;
}

CFileList* CFileList::FindFileRecursively (const CString& sDirectory, const CString& sFindAfter,
										   bool bSearchThisFolder, int nLevel, int nRecursion) {
	if (bSearchThisFolder) {
		CFileList* pFileList = TryCreateFileList(sDirectory + "\\", nLevel);
		if (pFileList != NULL) {
			return pFileList;
		}
	}
	// create a list of all child directories
	CFindFile fileFind;
	std::list<CString> dirList;
	if (fileFind.FindFile(sDirectory + "\\*")) {
		if (fileFind.IsDirectory() && !fileFind.IsDots()) {
			dirList.push_back(fileFind.GetFileName());
		}
		while (fileFind.FindNextFile()) {
			if (fileFind.IsDirectory() && !fileFind.IsDots()) {
				dirList.push_back(fileFind.GetFileName());
			}
		}
	}

	if (dirList.size() > 0) {
		dirList.sort();

		bool bStart = sFindAfter.GetLength() == 0;
		std::list<CString>::iterator iter;
		for (iter = dirList.begin( ); iter != dirList.end( ); iter++ ) {
			if (bStart) {
				CFileList* pFileList = FindFileRecursively(sDirectory + "\\" + *iter, _T(""), true, nLevel+1, nRecursion+1);
				if (pFileList != NULL) {
					return pFileList;
				}
			}
			if (iter->CompareNoCase(sFindAfter) == 0) {
				bStart = true;
			}
		}
	}

	// terminate recursion if on start level
	if (nLevel == 0) {
		return NULL;
	}

	if (nRecursion > 0) {
		return NULL; // this branch is not containing any images
	} else {
		// backtrack
		int nPos = sDirectory.ReverseFind(_T('\\'));
		CString sParentDir = (nPos > 0) ? sDirectory.Left(nPos) : _T("");
		CString sThisDir = sDirectory.Right(sDirectory.GetLength() - nPos - 1);
		return FindFileRecursively(sParentDir, sThisDir, false, nLevel - 1, max(0, nRecursion - 1));
	}
}

CFileList* CFileList::WrapToPrevImage() {
	// This is much easier than going to next image as we never go back to new unknown folders
	if (m_prev != NULL) {
		return m_prev;
	}
	return this;
}

void CFileList::NextInFolder() {
	if (m_fileList.size() > 0) {
		if (m_iter == m_fileList.end()) {
			// Not reachable via Next(), which returns early on end(),
			// but stepping from end() would be undefined; restart at
			// the first file instead.
			m_iter = m_fileList.begin();
			m_nCurrentIndex = 0;
		} else {
			m_iter++;
			if (m_iter == m_fileList.end()) {
				m_iter = m_fileList.begin();
				m_nCurrentIndex = 0;
			} else {
				m_nCurrentIndex++;
			}
		}
	}
}

CFileList* CFileList::GotoFirstShown() {
	if (sm_eMode == Helpers::NM_LoopDirectory)
		return this;

	LPCTSTR thisFile, prevFile;
	LPCTSTR firstFile = Current();
	CFileList* pThis = this;
	do {
		thisFile = pThis->Current();
		CFileList* pPrev = pThis->Prev();
		pThis = pPrev;
		prevFile = pThis->Current();
	} while (thisFile != NULL && prevFile != NULL && firstFile != NULL && _tcscmp(thisFile, prevFile) != 0 && _tcscmp(prevFile, firstFile) != 0);

	return pThis;
}

CFileList* CFileList::TryCreateFileList(const CString& directory, int nNewLevel) {
	// Check if we already had this directory in the loop, do not create a new loop
	CFileList* pList = this;
	while (pList != NULL) {
		if (pList->m_sDirectory.CompareNoCase(directory.Left(directory.GetLength() - 1)) == 0) {
			return NULL;
		}
		pList = pList->m_prev;
	}

	CFileList* pNewList = new CFileList(directory, m_directoryWatcher, CFileDesc::GetSorting(), CFileDesc::IsSortedAscending(), m_bWrapAroundFolder, nNewLevel);
	if (pNewList->m_fileList.size() > 0) {
		pNewList->m_prev = this;
		m_next = pNewList;
		return pNewList;
	} else {
		delete pNewList;
		return NULL;
	}
}

void CFileList::FindFiles() {
	m_fileList.clear();
	if (!m_sDirectory.IsEmpty()) {
		// One pass over the directory, matching the ending in memory. The old loop
		// asked the file system once per ending, once the RAW and
		// WIC lists from the ini are added - and a suffix wildcard like "*.jpg"
		// cannot use the directory index, so each of those calls re-scanned the
		// whole directory. Asking for "*" once turns that into a single scan.
		//
		// The "has an ending" test deliberately mirrors the old comparison instead
		// of following the usual "a leading dot is not an extension" rule: the old
		// code took everything after the last dot and compared it, so a file named
		// ".jpg" was listed, and rejecting leading dots here would silently drop
		// it. ".gitignore" still falls out on its own because "gitignore" is not a
		// supported ending. Only a name without a dot, or one ending in a dot, is
		// rejected up front. The ending itself is compared exactly, which also
		// covers the reason the old code needed expectedExtension: FindFirstFile
		// ("*.tif") also returns ".tiff" files through their 8.3 short name.
		CFindFile fileFind;
		if (fileFind.FindFile(m_sDirectory + _T("\\*"))) {
			do {
				if (fileFind.IsDirectory()) {
					continue;
				}
				const CString& sFileName = fileFind.GetFileName();
				int nLastDot = sFileName.ReverseFind(_T('.'));
				if (nLastDot < 0 || nLastDot == sFileName.GetLength() - 1) {
					continue;
				}
				LPCTSTR sEnding = ((LPCTSTR) sFileName) + nLastDot + 1;
				// E4: O(log n) lookup in the sorted suffix set instead of a
				// linear _tcsicmp scan over all endings for every file.
				if (IsSupportedFileEnding(sEnding)) {
					AddToFileList(m_fileList, fileFind, NULL);
				}
			} while (fileFind.FindNextFile());
		}
	}

	m_fileList.sort();
	// E10: the list was rebuilt; the current position is re-established
	// by the caller (FindFile + SetCurrentIter in the constructor/Reload/SetSorting).
	m_nCurrentIndex = -1;
}

void CFileList::VerifyFiles() {
	// The old for-loop did "iter = erase(iter)" and then the loop's own iter++,
	// which skipped the element after each deleted one, leaving dead entries
	// behind when several consecutive files were gone. Iterative erase-returning-
	// next visits every element exactly once.
	// E10: the cached index is adjusted as elements vanish: an erasure
	// before the current file shifts it down by one, erasing the current
	// file moves m_iter to the next file (same index) or to end() (-1).
	// nPos is the position of iter in the list as it stands now, so it
	// stays comparable with the adjusted index; it is not advanced past
	// an erasure because the next element slides into its place.
	std::list<CFileDesc>::iterator iter = m_fileList.begin();
	int nPos = 0;
	while (iter != m_fileList.end()) {
		if (::GetFileAttributes(iter->GetName()) == INVALID_FILE_ATTRIBUTES) {
			bool bErasingCurrent = (iter == m_iter);
			iter = m_fileList.erase(iter);
			if (m_nCurrentIndex >= 0) {
				if (bErasingCurrent) {
					m_iter = iter;
					m_nCurrentIndex = (iter == m_fileList.end()) ? -1 : nPos;
				} else if (nPos < m_nCurrentIndex) {
					m_nCurrentIndex--;
				}
			}
		} else {
			++iter;
			++nPos;
		}
	}
}

bool CFileList::IsImageFile(const CString & sEnding) {
	// E4: same O(log n) lookup as FindFiles; no MakeLower copy, and
	// case-insensitive, so an upper-case ini ending is recognised here
	// exactly as it already was by FindFiles.
	return IsSupportedFileEnding(sEnding);
}

bool CFileList::TryReadingSlideShowList(const CString & sSlideShowFile) {
	FILE *fptr;
	if ((fptr = _tfopen(sSlideShowFile,_T("rb"))) == NULL) {
		return false;
	}
	// Try to detect if this is a UTF16 Unicode text file or a ANSI or UTF-8 coded file
	const int NUMPROBE = 64;
	uint8 buff[NUMPROBE];
	bool bUnicode;
	bool bUTF8Marker = false;
	int nSeekPos = 0;
	int nRealProbe = (int)fread(buff, 1, NUMPROBE, fptr);
	if (nRealProbe < 16) {
		// file is too short for a good guess
		bUnicode = false;
	} else {
		if (buff[0] == 0xFF && buff[1] == 0xFE) {
			// Unicode byte order marker, only supporting little endian unicode files
			bUnicode = true;
			nSeekPos = 2;
		} else if (buff[0] == 0xEF && buff[1] == 0xBB && buff[2] == 0xBF) {
			// UTF-8 encoded text file
			bUTF8Marker = true;
		} else {
			// Use a heuristic in case the BOM is missing
			int nGood = 0;
			for (int i = 0; i < nRealProbe; i++) {
				if (i & 1) {
					if (buff[i] == 0) nGood++;
				} else {
					if (buff[i] != 0) nGood++;
				}
			}
			bUnicode = nGood > nRealProbe*2/3;
		}
	}

	bool bIsUTF8EncodedXML = strstr((char*)buff, "encoding=\"utf-8\"") != NULL;
	if (bUTF8Marker || bIsUTF8EncodedXML) {
		// UTF-8 encoded text file, open the file in UTF-8 mode, it will be read as Unicode
		fclose(fptr);
		if ((fptr = _tfopen(sSlideShowFile,_T("r, ccs=UTF-8"))) == NULL) {
			return false;
		}
		nSeekPos = bIsUTF8EncodedXML ? max(0, (int)(strchr((char*)buff, '<') - (char*)buff)) : 3;
		bUnicode = true;
	}

	int FILE_BUFFER_SIZE = CSettingsProvider::This().MaxSlideShowFileListSize() * 1024;
	char* fileBuff = new char[FILE_BUFFER_SIZE];
	char* fileBuffOrig = fileBuff;
	wchar_t* wideFileBuff = (wchar_t*)fileBuff; // same buffer interpreted as Unicode
	fseek(fptr, nSeekPos, SEEK_SET);
	int nRealFileSizeChars = (int)fread(fileBuff, 1, FILE_BUFFER_SIZE, fptr);
	if (bUnicode) nRealFileSizeChars = nRealFileSizeChars/2;

	const int LINE_BUFF_SIZE = 512;
	TCHAR lineBuff[LINE_BUFF_SIZE + 1];
	bool bValid = true;
	int nTotalChars = 0;

	do {
		int nNumChars = 0;
		if (bUnicode) {
			wchar_t* pRunning = wideFileBuff;
			bool bIsNull = false;
			while (*pRunning != L'\n' && nNumChars < LINE_BUFF_SIZE && nTotalChars < nRealFileSizeChars) {
				if (*pRunning == 0) {
					bIsNull = true;
				}
				pRunning++; nNumChars++; nTotalChars++;
			}
			bValid = nNumChars < 512 && !bIsNull;
			if (bValid) {
				memcpy(lineBuff, wideFileBuff, (pRunning - wideFileBuff)*sizeof(TCHAR));
			}
			wideFileBuff = pRunning+1;
			nTotalChars++;
		} else {
			char* pRunning = fileBuff;
			bool bIsNull = false;
			while (*pRunning != '\n' && nNumChars < LINE_BUFF_SIZE && nTotalChars < nRealFileSizeChars) {
				if (*pRunning == 0) {
					bIsNull = true;
				}
				pRunning++; nNumChars++; nTotalChars++;
			}
			bValid = nNumChars < LINE_BUFF_SIZE && !bIsNull;
			if (bValid) {
				size_t nDummy;
				mbstowcs_s(&nDummy, lineBuff, LINE_BUFF_SIZE, fileBuff, pRunning - fileBuff);
			}
			fileBuff = pRunning+1;
			nTotalChars++;
		}
		if (!bValid) {
			// the file contains very long lines or null characters, it is most likely not a text file
			fclose(fptr);
			delete[] fileBuffOrig;
			return false;
		}
		// one line in lineBuff, check if there is a valid file name
		bool bRelativePath = false;
		lineBuff[nNumChars] = _T('\n'); // force end
		lineBuff[nNumChars + 1] = 0; // terminate line string
		TCHAR* pStart = _tcsstr(lineBuff, _T("\\\\"));
		if (pStart == NULL) {
			pStart = _tcsstr(lineBuff+1, _T(":\\"));
			if (pStart != NULL) {
				pStart--;
			} else {
				// try to find file name with relative path
				// E9: one in-place scan of the line (FindSlideShowEndingDot)
				// instead of one stristr() per suffix, which copied the whole
				// line and built a temporary CString on every call.
				pStart = FindSlideShowEndingDot(lineBuff);
				if (pStart != NULL) {
					while (pStart >= lineBuff && *pStart != _T('"') && *pStart != _T('>')) pStart--;
					pStart++;
					bRelativePath = true;
				}
			}
			
		}
		if (pStart != NULL) {
			// extract file name and add to list of files to show
			TCHAR* pEnd = pStart;
			while (*pEnd != _T('\r') && *pEnd != _T('\n') && *pEnd != _T('"') && *pEnd != _T('<')) {
				pEnd++;
			}
			*pEnd = 0;
			CFindFile fileFind;
			CString sPath = bRelativePath ? (m_sDirectory + _T('\\') + pStart) : pStart;
			if (fileFind.FindFile(sPath)) {
				AddToFileList(m_fileList, fileFind, NULL);
			}
		}
	} while (nTotalChars < nRealFileSizeChars);

	fclose(fptr);
	delete[] fileBuffOrig;
	return true;
}
