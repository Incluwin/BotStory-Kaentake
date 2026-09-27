#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "wvs/secure.h"
#include "wvs/tooltip.h"
#include "wvs/iteminfo.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include "ztl/zcom.h"
#include <vector>

// =============================================================
// Custom.wz cash-item string fallback
// CItemInfo eagerly caches String/<category>.img into a ZMap at +0x54 during
// startup. GetItemString @ 0x5CF6FE reads only that cache, so cash IDs added
// later under Custom.wz/String/Cash.img aren't visible. Hook falls back to a
// live GetObjectA lookup so tooltips resolve without mutating the cache.
// =============================================================
#define ADDR_GET_ITEM_STRING 0x005CF6FE
#define ADDR_ZXSTRING_CAT    0x00428D30

typedef ZXString<char>* (__thiscall* _GetItemString)(void* pThis, ZXString<char>* result, int nItemID, const char* sPropName);
static _GetItemString Original_GetItemString = reinterpret_cast<_GetItemString>(ADDR_GET_ITEM_STRING);

ZXString<char>* __fastcall Hook_GetItemString(void* pThis, void* /*edx*/, ZXString<char>* result, int nItemID, const char* sPropName) {
    Original_GetItemString(pThis, result, nItemID, sPropName);

    if (!result->IsEmpty() || !sPropName) {
        return result;
    }
    if (nItemID < 5000000 || nItemID >= 6000000) {
        return result;
    }

    wchar_t wProp[64];
    if (MultiByteToWideChar(CP_ACP, 0, sPropName, -1, wProp, _countof(wProp)) <= 0) {
        return result;
    }
    wchar_t wPath[192];
    _snwprintf_s(wPath, _countof(wPath), _TRUNCATE, L"Custom/String/Cash.img/%d/%s", nItemID, wProp);

    Ztl_variant_t vObj = get_rm()->GetObjectA(wPath);
    if (vObj.vt != VT_BSTR || !V_BSTR(&vObj)) {
        return result;
    }

    BSTR bs = V_BSTR(&vObj);
    int nWide = static_cast<int>(SysStringLen(bs));
    if (nWide <= 0) {
        return result;
    }
    int nNarrow = WideCharToMultiByte(CP_ACP, 0, bs, nWide, nullptr, 0, nullptr, nullptr);
    if (nNarrow <= 0) {
        return result;
    }
    std::vector<char> buf(nNarrow + 1, 0);
    WideCharToMultiByte(CP_ACP, 0, bs, nWide, buf.data(), nNarrow, nullptr, nullptr);

    reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(ADDR_ZXSTRING_CAT)(result, buf.data(), nNarrow);
    return result;
}


class GW_ItemSlotEquip {
public:
    MEMBER_AT(TSecType<int>, 0xC, nItemID)
    MEMBER_AT(ZtlSecurePacked<unsigned char>, 0x28, nRUC)
    MEMBER_AT(ZtlSecure<short>, 0x34, niSTR)
    MEMBER_AT(ZtlSecure<short>, 0x3C, niDEX)
    MEMBER_AT(ZtlSecure<short>, 0x44, niINT)
    MEMBER_AT(ZtlSecure<short>, 0x4C, niLUK)
    MEMBER_AT(ZtlSecure<short>, 0x54, niMaxHP)
    MEMBER_AT(ZtlSecure<short>, 0x5C, niMaxMP)
    MEMBER_AT(ZtlSecure<short>, 0x64, niPAD)
    MEMBER_AT(ZtlSecure<short>, 0x6C, niMAD)
    MEMBER_AT(ZtlSecure<short>, 0x74, niPDD)
    MEMBER_AT(ZtlSecure<short>, 0x7C, niMDD)
    MEMBER_AT(ZtlSecure<short>, 0x84, niACC)
    MEMBER_AT(ZtlSecure<short>, 0x8C, niEVA)
    MEMBER_AT(ZtlSecure<short>, 0x94, niCraft)
    MEMBER_AT(ZtlSecure<short>, 0x9C, niSpeed)
    MEMBER_AT(ZtlSecure<short>, 0xA4, niJump)
    MEMBER_AT(ZtlSecure<short>, 0xAC, nAttribute)
    // Fusion Anvil transmog: int at offset 0xF9 holds the "skin" item id.
    MEMBER_AT(int, 0xF9, nAnvilItemID)
};


void CUIToolTip::SetToolTip_Equip_Basic_hook(GW_ItemSlotEquip* pe) {
    int nItemID = pe->nItemID;
    auto pEquipItem = CItemInfo::GetInstance()->GetEquipItem(nItemID);
    if (!pEquipItem) {
        return;
    }
    // get_weapon_category_name
    ZXString<char> sWeaponCategory;
    reinterpret_cast<ZXString<char>*(__cdecl*)(ZXString<char>*, int)>(0x005C99FC)(&sWeaponCategory, nItemID);
    if (!sWeaponCategory.IsEmpty()) {
        AddInfoEx(14, 15, "CATEGORY :", sWeaponCategory, 1, 1001);
    }
    // get_item_category_name
    ZXString<char> sItemCategory;
    reinterpret_cast<ZXString<char>*(__cdecl*)(ZXString<char>*, int)>(0x005C9E61)(&sItemCategory, nItemID);
    if (!sItemCategory.IsEmpty()) {
        AddInfoEx(14, 15, "CATEGORY :", sItemCategory, 1, 1001);
    }
    // get_weapon_attack_speed
    ZXString<char> sAttackSpeed;
    reinterpret_cast<ZXString<char>*(__cdecl*)(ZXString<char>*, int)>(0x005C9AFA)(&sAttackSpeed, nItemID);
    if (!sAttackSpeed.IsEmpty()) {
        AddInfoEx(14, 15, "ATTACK SPEED :", sAttackSpeed, 1, 1001);
    }

    PrintValueEx(PT_INC, pe->niSTR, pEquipItem->niSTR, "STR :", 0);
    PrintValueEx(PT_INC, pe->niDEX, pEquipItem->niDEX, "DEX :", 0);
    PrintValueEx(PT_INC, pe->niINT, pEquipItem->niINT, "INT :", 0);
    PrintValueEx(PT_INC, pe->niLUK, pEquipItem->niLUK, "LUK :", 0);
    PrintValueEx(PT_INC, pe->niMaxHP, pEquipItem->niMaxHP, "HP :", 0);
    PrintValueEx(PT_INC, pe->niMaxMP, pEquipItem->niMaxMP, "MP :", 0);

    PrintValueEx(PT_VALUE, pe->niPAD, pEquipItem->niPAD, "WEAPON ATTACK :", 0);
    PrintValueEx(PT_VALUE, pe->niMAD, pEquipItem->niMAD, "MAGIC ATTCK :", 0);
    PrintValueEx(PT_VALUE, pe->niPDD, pEquipItem->niPDD, "WEAPON DEF. :", 0);
    PrintValueEx(PT_VALUE, pe->niMDD, pEquipItem->niMDD, "MAGIC DEF. :", 0);

    PrintValueEx(PT_INC, pe->niACC, pEquipItem->niACC, "ACCURACY :", 0);
    PrintValueEx(PT_INC, pe->niEVA, pEquipItem->niEVA, "AVOIDABILITY :", 0);
    PrintValueEx(PT_INC, pe->niCraft, pEquipItem->niCraft, "HANDS :", 0);
    PrintValueEx(PT_INC, pe->niSpeed, pEquipItem->niSpeed, "SPEED :", 0);
    PrintValueEx(PT_INC, pe->niJump, pEquipItem->niJump, "JUMP :", 0);

    PrintValue(PT_PERCENT, pEquipItem->nKnockback, "THE RATE OF KNOCK-BACK :", 0);
    if (pe->nAttribute & 2) {
        AddInfoEx(14, 15, "ADD PREVENT SLIPPING", "", 1, 1001);
    }
    if (pe->nAttribute & 4) {
        AddInfoEx(14, 15, "ADD PREVENT COLDNESS", "", 1, 1001);
    }
    if (pEquipItem->nRUC) {
        PrintValue(PT_VALUE, pe->nRUC, "NUMBER OF UPGRADES AVAILABLE :", 1);
    }

    // Fusion Anvil: append a line showing the skin item's name with a
    // "(Transmog)" tag so the player can see what the appearance is.
    if (pe->nAnvilItemID != 0) {
        ZXString<char> sSkinName;
        reinterpret_cast<ZXString<char>*(__thiscall*)(CItemInfo*, ZXString<char>*, int)>(0x005CF63E)(
            CItemInfo::GetInstance(), &sSkinName, pe->nAnvilItemID);

        ZXString<char> sLine;
        sLine.Format("%s (Transmog)", sSkinName);
        AddInfoEx(14, 15, "APPEARANCE :", sLine, 1, 1001);
    }
}


// ===========================================================================
// Transmog corner icon — draw the skin item's small icon in the top-right
// corner of the main equip tooltip's canvas after the engine has rendered it.
// ===========================================================================

static auto CUIToolTip__DrawToolTip_Equip =
    reinterpret_cast<void(__thiscall*)(CUIToolTip*, int, GW_ItemSlotEquip*)>(0x008ED0D2);

// Global default tooltip/basic font getter (v83 uses Dotum 12).
static auto get_basic_font =
    reinterpret_cast<IWzFontPtr*(__cdecl*)(IWzFontPtr*, int)>(0x0098A707);

void __fastcall CUIToolTip__DrawToolTip_Equip_hook(
    CUIToolTip* pThis, void* /*edx*/, int a2, GW_ItemSlotEquip* pe)
{
    CUIToolTip__DrawToolTip_Equip(pThis, a2, pe);
    if (!pe || !pe->nAnvilItemID || !pThis || !pThis->m_pLayer) return;
    try {
        Ztl_variant_t vIdx;
        V_VT(&vIdx) = VT_I4;
        V_I4(&vIdx) = 0;
        IWzCanvasPtr pCanvas = pThis->m_pLayer->Getcanvas(vIdx);
        if (!pCanvas) return;

        // Top-right corner — icons are anchored at bottom-left, so y is the
        // baseline. 32x32 icon, 14px padding from right edge, 6px from top.
        int iconX = pThis->m_nWidth - 32 - 14;
        int iconBaselineY = 6 + 32;
        CItemInfo::GetInstance()->DrawItemIconForSlot(
            pCanvas, pe->nAnvilItemID, iconX, iconBaselineY, 0, 0, 0, 1, 0, 1);

        // "Transmog" label directly under the icon.
        IWzFontPtr pFont;
        get_basic_font(std::addressof(pFont), 0);
        if (pFont) {
            pCanvas->DrawTextA(
                iconX - 12, iconBaselineY + 2,
                Ztl_bstr_t(L"Transmog"),
                pFont, Ztl_variant_t(), Ztl_variant_t());
        }
    } catch (...) {}
}


void AttachToolTipMod() {
    // Resolve custom cash item name/desc via Custom.wz/String/Cash.img
    ATTACH_HOOK(Original_GetItemString, Hook_GetItemString);
    ATTACH_HOOK(CUIToolTip::SetToolTip_Equip_Basic, CUIToolTip::SetToolTip_Equip_Basic_hook);
    ATTACH_HOOK(CUIToolTip__DrawToolTip_Equip, CUIToolTip__DrawToolTip_Equip_hook);
}