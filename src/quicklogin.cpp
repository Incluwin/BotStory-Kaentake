#include "pch.h"
#include "hook.h"
#include "ztl/ztl.h"
#include "wvs/wnd.h"
#include "wvs/wvsapp.h"

// ============================================================================
// [Section 1] UI Logic - Auto-focus Channel Selection
// ============================================================================

class CUIChannelSelect : public TSingleton<CUIChannelSelect, 0x00BEDA58> {
public:
    // [0x04] UI message handler interface (handles focus, key input, etc.)
    MEMBER_AT(IUIMsgHandler, 0x04, m_handler)    
};

class CWndMan : public CWnd, public TSingleton<CWndMan, 0x00BEC20C> {
public:
    void SetFocus(IUIMsgHandler* pHandler) {
        reinterpret_cast<void(__thiscall*)(CWndMan*, IUIMsgHandler*)>(0x009E3264)(this, pHandler);
    }
};

class CUIWorldSelect {
public:
    MEMBER_HOOK(void, 0x006283C1, UserLimitResult, int bOverUserLimit, int bPopulateLevel)
};


// Auto-focus the channel selection window right after a world is selected.
// This allows players to instantly press 'Enter' to join channel 1 without clicking.
void CUIWorldSelect::UserLimitResult_hook(int bOverUserLimit, int bPopulateLevel) {
    UserLimitResult(this, bOverUserLimit, bPopulateLevel);

    // Safely check if the channel window was actually created.
    auto pChannelSelect = CUIChannelSelect::GetInstance();
    if (pChannelSelect && CWndMan::GetInstance())
        CWndMan::GetInstance()->SetFocus(&pChannelSelect->m_handler);
}

// ============================================================================
// [Section 2] System Logic - CLogin Stability Fix
// ============================================================================


class CLogin {
public:
    // [0x23C] Flag for new character creation (contains a missing initialization bug)
    MEMBER_AT(int, 0x23C, m_bNewCharPending)
    MEMBER_HOOK(CLogin*, 0x005F3C59, Constructor)
};

// Fixes intermittent focus issues by clearing heap garbage and forcing window activation via ALT-key bypass.
CLogin* CLogin::Constructor_hook() {
    auto ret = Constructor(this);

    // This prevents ghost focus or UI bugs caused by residual heap memory (garbage values).
    this->m_bNewCharPending = 0; 

    // [OS Focus] Bypass Windows focus restrictions by simulating a system key (ALT)
    keybd_event(VK_MENU, 0, 0, 0); // ALT down
    SetForegroundWindow(CWvsApp::GetInstance()->m_hWnd);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0); // ALT up

    return ret;
}

// ============================================================================
// [Section 3] Entry Point
// ============================================================================

void AttachQuickLoginMod() {
    ATTACH_HOOK(CUIWorldSelect::UserLimitResult, CUIWorldSelect::UserLimitResult_hook);
    ATTACH_HOOK(CLogin::Constructor, CLogin::Constructor_hook);   
}
