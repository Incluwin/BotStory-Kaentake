#include "pch.h"
#include "hook.h"
#include "wvs/secure.h"
#include "ztl/ztl.h"
#include "ztl/zstr.h"

// Forward-declared — defined in cosmicSso.cpp. Non-custodial SSO bridge:
// posts the same credentials the user just typed to Cosmic's loopback
// control port and drops the ed25519-signed envelope in %APPDATA%\Cosmic
// for MapleForge to pick up. No secret is held here.
void DispatchCosmicSsoWrite(const std::string& account, const std::string& password);

class CCtrlEdit {
public:
    // _DWORD __stdcall CCtrlEdit::GetText(void *arg0)
    ZXString<char>* GetText(ZXString<char>* arg0) {
        return reinterpret_cast<ZXString<char>*(__thiscall*)(CCtrlEdit*, ZXString<char>*)>(0x00471353)(this, arg0);
    }

    // void __thiscall CCtrlEdit::SetText(CCtrlEdit *this, const char *sText)
    void SetText(const char* sText) {
        return reinterpret_cast<void(__thiscall*)(CCtrlEdit*, const char*)>(0x004CC512)(this, sText);
    }
};

class CConfig : public TSingleton<CConfig, 0x00BEBF9C> {
public:
    enum {
        GLOBAL_OPT = 0x0,
        LAST_CONNECT_INFO = 0x1,
        CHARACTER_OPT = 0x2,
    };

    // int __thiscall CConfig::GetOpt_String(CConfig *this, int a2, int pThis, int sKey, char *s)
    ZXString<char>* GetOpt_String(ZXString<char>* result, int nType, const char* sKey, const char* sDefault) {
        return reinterpret_cast<ZXString<char>*(__thiscall*)(CConfig*, ZXString<char>*, int, const char*, const char*)>(0x0049EE54)(this, result, nType, sKey, sDefault);
    }
    // int __stdcall CConfig::SetOpt_String(int nType, int a2, char *a3)
    int SetOpt_String(int nType, const char* sKey, const char* sValue) {
        return reinterpret_cast<int(__thiscall*)(CConfig*, int, const char*, const char*)>(0x0049EF0D)(this, nType, sKey, sValue);
    }
};

class CUITitle {
public:
    MEMBER_AT(CCtrlEdit*, 0xCC, m_pId)        // ID input field
    MEMBER_AT(CCtrlEdit*, 0xD4, m_pPassword)  // Password input field

    // void __thiscall CUITitle::OnCreate(CUITitle *this, void *pData)
    MEMBER_HOOK(void, 0x0062054A, OnCreate, void* pData)  // Called when login screen is created
    // void __thiscall CUITitle::SetRet(CUITitle *this, int nRet)
    MEMBER_HOOK(void, 0x00620E3C, SetRet, int nRet)       // Called on login attempt (button or enter key)
};

// Registry key used to store the saved password
static constexpr auto sLastPasswordKey = "LastPassword";

// Hook: auto-fills the password field with the previously saved password on login screen open
void CUITitle::OnCreate_hook(void* pData) {
    OnCreate(this, pData);  // Run original first to ensure UI is fully initialized

    ZXString<char> result;
    CConfig::GetInstance()->GetOpt_String(&result, CConfig::GLOBAL_OPT, sLastPasswordKey, "");
    m_pPassword->SetText(result);
}


// Hook: saves the password to registry when a login attempt is made
// nRet == 1: login (button click or enter), nRet == 2: cancel (ESC)
void CUITitle::SetRet_hook(int nRet) {
    if (nRet == 1) {
        ZXString<char> id;
        ZXString<char> pw;
        m_pId->GetText(&id);
        m_pPassword->GetText(&pw);
        CConfig::GetInstance()->SetOpt_String(CConfig::GLOBAL_OPT, sLastPasswordKey, pw);

        // Fire-and-forget the MapleForge SSO bridge. If Cosmic rejects the
        // credentials or the control port is offline, nothing gets written
        // and the user falls back to the normal MapleForge password flow.
        const char* idStr = static_cast<const char*>(id);
        const char* pwStr = static_cast<const char*>(pw);
        DispatchCosmicSsoWrite(idStr ? idStr : "", pwStr ? pwStr : "");
    }
    SetRet(this, nRet);  // Call original to proceed with login
}

void AttachRememberPasswordMod() {
    ATTACH_HOOK(CUITitle::OnCreate, CUITitle::OnCreate_hook);
    ATTACH_HOOK(CUITitle::SetRet, CUITitle::SetRet_hook);
}
