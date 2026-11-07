#pragma once

#include "PanelController.h"

class CImageProcessingPanel;
class CImageProcessingParams;

// Implements functionality of the image processing panel
class CImageProcPanelCtl : public CPanelController
{
public:
	// The given image processing parameters are modified by this panel, the same applied to the given boolean values
	CImageProcPanelCtl(CMainDlg* pMainDlg, CImageProcessingParams* pParams, bool* pEnableLDC, bool* pEnableContrastCorr);
	virtual ~CImageProcPanelCtl();

	virtual float DimFactor() { return 0.5f; }

	virtual bool IsVisible() { return m_bVisible && m_bEnabled && !m_pMainDlg->IsInSlideShowWithTransition() ; }
	virtual bool IsActive() { return m_bEnabled; }

	virtual void SetVisible(bool bVisible);
	virtual void SetActive(bool bActive); // toggled from the context menu / KeyMap (Ctrl+E); INI ShowBottomPanel is the initial value only

	virtual void AfterNewImageLoaded();

	virtual bool OnTimer(int nTimerId);
	virtual bool OnMouseMove(int nX, int nY);

	// Called by main dialog e.g. when saving or deleting parameter DB entry
	void ShowHideSaveDBButtons();

	bool EnterRenameCurrentFile();

	CRect GetUnsharpMaskButtonRect();

private:
	bool m_bEnabled;
	bool m_bVisible;
	bool m_bPinned; // true when shown by an explicit menu / KeyMap command (Ctrl+E): stays visible until toggled off, mouse-leave auto-hide is suppressed
	int m_nOldMouseY;
	CImageProcessingPanel* m_pImageProcPanel;

	bool RenameCurrentFile(LPCTSTR sNewFileTitle);
	bool CanMakeVisible();

	static void OnUnsharpMask(void* pContext, int nParameter, CButtonCtrl & sender);
	static bool OnRenameFile(void* pContext, CTextCtrl & sender, LPCTSTR sChangedText);
};