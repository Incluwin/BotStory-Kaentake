// Ported from https://gist.github.com/v3921358/9a1ce09a3b1205ddf65bbe3b0308963b
// Boss HP percentage display beneath the boss health bar (GMS v83).
// Original framework (stdafx.h / Memory::SetHook) replaced with Kaentake's pch.h + AttachHook.
#include "pch.h"
#include "hook.h"
#include "BossHP.h"
#include <cstdio>
#include <windows.h>

const DWORD dw_TSingleton_CUIMiniMap___ms_pInstance = 0x00BED788;
const DWORD dwCField__ShowMobHpTag = 0x005336CA;
const DWORD dwCField__Init = 0x00528DBC; // CField::CField
const DWORD dwCField__Dispose = 0x00529035; // CField::~CField
const DWORD dwCUIToolTip__SetToolTip_String = 0x008E6E7D;
const DWORD dwCUIToolTip__ClearToolTip = 0x008E6E23;
const DWORD dwCUIToolTip__DisposeToolTip = 0x008E6BA3; // CUIToolTip::~CUIToolTip
const DWORD dwCUIToolTip__CreateToolTip = 0x008E49B5; // CUIToolTip::CUIToolTip
const DWORD dwCUserLocal__Update = 0x0094A144;

char BossHP::aBossHpUIToolTip[1304];
double BossHP::dBossHpPercentage = 0;

void BossHP::Hook() { // main method
    HookInternal();
}

void BossHP::HookInternal() {
    HookUpdate();
    HookShowMobHPTag();
    BossHP::HookDisposeField();
    HookInitField();
}

void BossHP::HookUpdate() {
    typedef void(__fastcall* UserLocal__Update_type)(void* pThis, void* edx);
    static auto _UserLocal__Update = reinterpret_cast<UserLocal__Update_type>(dwCUserLocal__Update);

    static UserLocal__Update_type Hook = [](void* pThis, void* edx) -> void {
        _UserLocal__Update(pThis, edx);
        DrawBossHpNumberIfNeed();
    };

    AttachHook(reinterpret_cast<void**>(&_UserLocal__Update), CastHook(Hook));
}

void BossHP::HookShowMobHPTag() {
    typedef void(__fastcall* Field__ShowMobHPTag_type)(void* pThis, void* edx, unsigned int dwMobID, int nColor, int nBgColor, int nHP, int nMaxHP);
    static auto _Field__ShowMobHPTag = reinterpret_cast<Field__ShowMobHPTag_type>(dwCField__ShowMobHpTag);

    static Field__ShowMobHPTag_type Hook = [](void* pThis, void* edx, unsigned int dwMobID, int nColor, int nBgColor, int nHP, int nMaxHP) -> void {
        _Field__ShowMobHPTag(pThis, edx, dwMobID, nColor, nBgColor, nHP, nMaxHP);
        DrawBossHpNumber(nHP, nMaxHP);
    };
    AttachHook(reinterpret_cast<void**>(&_Field__ShowMobHPTag), CastHook(Hook));
}

void BossHP::HookInitField() {
    typedef void(__fastcall* Field__Init_Type)(void* pThis, void* edx);
    static auto _Field__Init = reinterpret_cast<Field__Init_Type>(dwCField__Init);

    static Field__Init_Type Hook = [](void* pThis, void* edx) -> void {
        if (dBossHpPercentage > 0) {
            BossHP::DisposeBossHpNumber();
        }
        BossHP::DisposeToolTip((int)&aBossHpUIToolTip);
        BossHP::CreateToolTip((int)&aBossHpUIToolTip);
        _Field__Init(pThis, edx);
    };
    AttachHook(reinterpret_cast<void**>(&_Field__Init), CastHook(Hook));
}

void BossHP::HookDisposeField() {
    typedef void(__fastcall* Field__Dispose_Type)(void* pThis, void* edx);
    static auto _Field__Dispose = reinterpret_cast<Field__Dispose_Type>(dwCField__Dispose);

    static Field__Dispose_Type Hook = [](void* pThis, void* edx) -> void {
        DisposeBossHpNumber();
        _Field__Dispose(pThis, edx);
    };
    AttachHook(reinterpret_cast<void**>(&_Field__Dispose), CastHook(Hook));
}

void BossHP::DrawBossHpNumberIfNeed() {
    if (dBossHpPercentage > 0) {
        char sToolTip[20];
        sprintf_s(sToolTip, "%.2f%%", dBossHpPercentage);
        BossHP::SetToolTip_String((int)&aBossHpUIToolTip, GetMiniMapWidth(), 37, sToolTip);
    }
}

void BossHP::DrawBossHpNumber(int nHP, int nMaxHP) {
    if (nHP > 0) {
        dBossHpPercentage = static_cast<double>(nHP) / nMaxHP * 100.0;
    }
    else {
        dBossHpPercentage = 0;
        BossHP::ClearToolTip((int)&aBossHpUIToolTip);
    }
}

void BossHP::DisposeBossHpNumber() {
    dBossHpPercentage = 0;
    BossHP::ClearToolTip((int)&aBossHpUIToolTip);
}

// it's ToolTip region

typedef void(__fastcall* UIToolTip__SetToolTip_String_Type)(int pThis, void* edx, int x, int y, const char* sToolTip);
static auto _UIToolTip__SetToolTip_String = reinterpret_cast<UIToolTip__SetToolTip_String_Type>(dwCUIToolTip__SetToolTip_String);

void BossHP::SetToolTip_String(int instance, int x, int y, const char* sToolTip) {
    _UIToolTip__SetToolTip_String(instance, 0, x, y, sToolTip);
}

typedef void(__fastcall* UIToolTip__ClearToolTip_Type)(int pThis, void* edx);
static auto _UIToolTip__ClearToolTip = reinterpret_cast<UIToolTip__ClearToolTip_Type>(dwCUIToolTip__ClearToolTip);

void BossHP::ClearToolTip(int instance) {
    _UIToolTip__ClearToolTip(instance, 0);
}

typedef void(__fastcall* UIToolTip__DisposeToolTip_Type)(int pThis, void* edx);
static auto _UIToolTip__DisposeToolTip = reinterpret_cast<UIToolTip__DisposeToolTip_Type>(dwCUIToolTip__DisposeToolTip);

void BossHP::DisposeToolTip(int instance) {
    _UIToolTip__DisposeToolTip(instance, 0);
}

typedef void(__fastcall* UIToolTip__CreateToolTip_Type)(int pThis, void* edx);
static auto _UIToolTip__CreateToolTip = reinterpret_cast<UIToolTip__CreateToolTip_Type>(dwCUIToolTip__CreateToolTip);

void BossHP::CreateToolTip(int instance) {
    _UIToolTip__CreateToolTip(instance, 0);
}

static int ReadInt(const DWORD dwAddress) {
    int nResult = -1;
    DWORD dwOldProtect;
    VirtualProtect((void*)dwAddress, sizeof(int), PAGE_EXECUTE_READ, &dwOldProtect);
    nResult = *reinterpret_cast<unsigned int*>(dwAddress);
    VirtualProtect((void*)dwAddress, sizeof(int), dwOldProtect, &dwOldProtect);
    return nResult;
}

int BossHP::GetMiniMapWidth() {
    return ReadInt(ReadInt(dw_TSingleton_CUIMiniMap___ms_pInstance) + 0x24);
}

// Kaentake entry point
void AttachBossHPMod() {
    BossHP::Hook();
}
