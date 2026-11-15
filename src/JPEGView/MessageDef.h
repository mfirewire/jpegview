#pragma once

// Message posted when image (of any format, not only JPEG) has finished loading.
// LPARAM is the request handle to retrieve the image
#define WM_IMAGE_LOAD_COMPLETED (WM_APP + 6)

// Message posted when the currently shown imae has been changed on disk and needs to be reloaded
#define WM_DISPLAYED_FILE_CHANGED_ON_DISK (WM_APP + 7)

// Message posted when the files in the current directory have been added or removed and thus the
// list of images in the directory needs to be reloaded
#define WM_ACTIVE_DIRECTORY_FILELIST_CHANGED (WM_APP + 8)

// Posted to main dialog for asynchronously loading the image with file name CMainDlg::m_sStartupFile
#define WM_LOAD_FILE_ASYNCH (WM_APP + 24)

// wParam of WM_LOAD_FILE_ASYNCH: WM_LOAD_FILE_ASYNCH_EMPTY_PAYLOAD (1) means
// the COPYDATA payload that triggered it was empty, so the message must only
// bring the existing window to the front; 0 means m_sStartupFile holds the
// file to open. The flag travels with the message, so it stays correct even
// if a file was opened before (#76).
#define WM_LOAD_FILE_ASYNCH_EMPTY_PAYLOAD 1

// Posted to main dialog when OCR text recognition on the current image has completed.
#define WM_OCR_COMPLETED (WM_APP + 25)

#define KEY_MAGIC 2978465