#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "wvs/wnd.h"
#include "wvs/iteminfo.h"
#include "wvs/packet.h"
#include "wvs/util.h"
#include "wvs/wvsapp.h"
#include "ztl/ztl.h"

#include <windows.h>


// ===========================================================================
// v83 Addresses (image base 0x400000)
// ===========================================================================

static constexpr uintptr_t kAddr_AvatarLook_Load              = 0x004E72AD;
static constexpr uintptr_t kAddr_ItemSlotBase_Decode          = 0x004E33F9;
static constexpr uintptr_t kAddr_InPacket_Decode4             = 0x00406629;
static constexpr uintptr_t kAddr_SendConsumeCash              = 0x00A0A63F;
static constexpr uintptr_t kAddr_SendEtcCash                  = 0x00A1DC5B;
static constexpr uintptr_t kAddr_DraggableItem_OnDropped      = 0x004EF140;
static constexpr uintptr_t kAddr_DraggableItem_OnDoubleClick  = 0x004EFD25;
static constexpr uintptr_t kAddr_get_consume_cash_item_type   = 0x004863D5;
static constexpr uintptr_t kAddr_play_ui_sound                = 0x00989588;
static constexpr uintptr_t kAddr_ClientSocket_SendPacket      = 0x0049637B;
static constexpr uintptr_t kAddr_ClientSocket_Instance        = 0x00BE7914;
static constexpr uintptr_t kAddr_CWvsContext_Instance         = 0x00BE7918;
static constexpr uintptr_t kOffset_CharacterData_InContext    = 0x20B8;
static constexpr uintptr_t kAddr_CharacterData_GetItem        = 0x004282F7;
static constexpr uintptr_t kAddr_TSecType_long_GetData        = 0x0042873D;
static constexpr uintptr_t kAddr_get_weapon_type              = 0x00460AA0;
static constexpr uintptr_t kAddr_get_bodypart_from_item       = 0x004606A0;

// Struct-size immediate patch sites (push 0F9h, opcode 0x68, imm32 follows).
static constexpr uintptr_t kPatch_GetItemSlotSizeImm          = 0x005D6053;
static constexpr uintptr_t kPatch_SubtypeAllocSizeImm         = 0x004E3580;

// Extend GW_ItemSlotEquip: nAnvilItemID at offset 0xF9, new size 0xFD.
static constexpr uint32_t  kNewItemSlotEquipSize              = 0xFD;
static constexpr size_t    kOffset_nAnvilItemID               = 0xF9;

// Fusion Anvil cash item (Item.wz/Cash/0590.img/05900000.img)
static constexpr int32_t   kFusionAnvilItemID                 = 5900000;
// Send opcode: 0x4F (CP_UserConsumeCashItemUseRequest).
static constexpr int       kOpcode_UseCashItem                = 0x4F;

// play_ui_sound prepends "Sound.wz/UI.img/" via string pool SP_2193.
static auto play_ui_sound =
    reinterpret_cast<void(__cdecl*)(const wchar_t*)>(kAddr_play_ui_sound);


// ===========================================================================
// Type fragments
// ===========================================================================

struct GW_ItemSlotBase : public ZRefCounted {
    virtual ~GW_ItemSlotBase() = 0;
    virtual int IsProtectedItem() = 0;
    virtual int IsPreventSlipItem() = 0;
    virtual int IsSupportWarmItem() = 0;
    virtual int IsBindedItem() = 0;
    virtual int IsPossibleTradingItem() = 0;
    virtual int GetType() = 0; // 1 = Equip
};

struct GW_ItemSlotEquip {
    MEMBER_AT(int32_t, kOffset_nAnvilItemID, nAnvilItemID)
};

struct AvatarLook {
    MEMBER_ARRAY_AT(int32_t, 0x1D, anHairEquip, 51)
};

// TSecType<long>::GetData is a const __thiscall method with no args, returns
// the decrypted plaintext int. The encrypted slot lives at &pItem[0x0C].
typedef int32_t (__thiscall* t_TSecType_long_GetData)(const void* pTSec);
static auto TSecType_long_GetData =
    reinterpret_cast<t_TSecType_long_GetData>(kAddr_TSecType_long_GetData);

static int32_t DecodeItemID(GW_ItemSlotBase* pItem) {
    if (!pItem) return 0;
    return TSecType_long_GetData(reinterpret_cast<const char*>(pItem) + 0x0C);
}


// ===========================================================================
// CDraggableItem view — fields at 0x18/0x1C/0x20.
// ===========================================================================

class CDraggableItem {
public:
    MEMBER_AT(int32_t, 0x18, m_nItemTI)
    MEMBER_AT(int32_t, 0x1C, m_nSlotPosition)
    MEMBER_AT(int32_t, 0x20, m_nIdx)
};


// ===========================================================================
// CharacterData::GetItem — fetch GW_ItemSlotBase* for a given (TI, POS).
// Signature: __thiscall(CharacterData* this, ZRef* outRef, int nTI, int nPOS).
// ZRef layout: { vtable_or_unused_at_0, item_ptr_at_4 } (8 bytes).
// ===========================================================================

struct ZRefOut {
    void* m_unused;
    GW_ItemSlotBase* m_pItem;
};

typedef void(__thiscall* t_CharacterData_GetItem)(
    void* pCharData, ZRefOut* outRef, int nTI, int nPOS);
static auto CharacterData_GetItem =
    reinterpret_cast<t_CharacterData_GetItem>(kAddr_CharacterData_GetItem);

static GW_ItemSlotBase* FetchInventoryItem(int nTI, int nPOS) {
    void* pCtx = *reinterpret_cast<void**>(kAddr_CWvsContext_Instance);
    if (!pCtx) return nullptr;
    void* pCharData = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(pCtx) + kOffset_CharacterData_InContext);
    if (!pCharData) return nullptr;
    ZRefOut out = {};
    CharacterData_GetItem(pCharData, &out, nTI, nPOS);
    return out.m_pItem;
}


// ===========================================================================
// Socket send — direct call into CClientSocket::SendPacket.
// ===========================================================================

typedef void(__thiscall* t_SendPacket)(void* pClientSocket, COutPacket* pPacket);
static auto ClientSocket_SendPacket =
    reinterpret_cast<t_SendPacket>(kAddr_ClientSocket_SendPacket);

static void SendOutPacket(COutPacket& oPacket) {
    void* pClientSocket = *reinterpret_cast<void**>(kAddr_ClientSocket_Instance);
    if (pClientSocket) {
        ClientSocket_SendPacket(pClientSocket, &oPacket);
    }
}


// ===========================================================================
// CUIFusionAnvil dialog — CWnd subclass.
// Loads UI.wz/UIWindow.img/Synthesizing sprites, renders 2 item slots +
// OK/Cancel/Exit buttons. Routes drops via the CDraggableItem::OnDropped hook.
// ===========================================================================

typedef int(__cdecl* t_get_bodypart)(int nItemID, int nGender, int* pnBodyPart, int bAll);
typedef int(__cdecl* t_get_weapon_type)(int nItemID);
static auto get_bodypart_from_item =
    reinterpret_cast<t_get_bodypart>(kAddr_get_bodypart_from_item);
static auto get_weapon_type =
    reinterpret_cast<t_get_weapon_type>(kAddr_get_weapon_type);

class CUIFusionAnvil : public CWnd {
public:
    inline static CUIFusionAnvil* ms_pInstance = nullptr;
    // Parent = nullptr: v83's CWnd::ms_RTTI_CWnd address in kaentake's wnd.h
    // points into string-literal memory in this build, so walking the chain
    // crashes. We own identity — CDraggableItem::OnDropped only needs
    // pTo->IsKindOf(&our_ms_RTTI) to match against *this* RTTI.
    inline static CRTTI           ms_RTTI{nullptr};

    // Canvas pointers FIRST — they are loaded once and read on every Draw.
    // Keeping them before mutable state guards against any stray write in
    // PutItem accidentally stomping a sprite pointer.
    IWzCanvasPtr      m_pBgCanvas;
    IWzCanvasPtr      m_pBgOverlay;   // backgrnd1 (inner frame)
    IWzCanvasPtr      m_pSubTitle;
    IWzCanvasPtr      m_pBtOkN, m_pBtOkP, m_pBtOkD, m_pBtOkM;
    IWzCanvasPtr      m_pBtCancelN, m_pBtCancelP, m_pBtCancelM;
    IWzCanvasPtr      m_pBtExitN, m_pBtExitP, m_pBtExitM;
    IWzCanvasPtr      m_pEffect[12];  // Effect/0..11 decorations

    // Mutable item-slot state (modified by PutItem).
    int               m_nItemPOS;
    int               m_nItemID;
    GW_ItemSlotBase*  m_apItem[2];
    int               m_anItemPos[2];
    int               m_anItemID[2];

    int               m_bBtOkEnable;
    int               m_nPressedBtn; // 0=none, 1=OK, 2=Cancel, 3=Exit
    int               m_nHoveredBtn; // 0=none, 1=OK, 2=Cancel, 3=Exit
    int               m_bDragging;
    int               m_nDragAnchorX;
    int               m_nDragAnchorY;
    RECT              m_rcSlot[2];
    RECT              m_rcBtOk, m_rcBtCancel, m_rcBtExit;

    CUIFusionAnvil(int nPOS, int nID);
    virtual ~CUIFusionAnvil() override;

    virtual void Draw(const RECT* pRect) override;
    virtual void OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) override;
    virtual int  OnMouseMove(int rx, int ry) override;
    virtual void OnDestroy() override;
    virtual void Update() override;

    virtual const CRTTI* GetRTTI() const override { return &ms_RTTI; }
    virtual int IsKindOf(const CRTTI* pRTTI) const override {
        return ms_RTTI.IsKindOf(pRTTI);
    }

    int  PutItem(GW_ItemSlotBase* pItem, int nTI, int nSlotPos, int rx, int ry);
    void SendRequestPacket();

    static IWzCanvasPtr LoadSprite(const wchar_t* sPath);
};

IWzCanvasPtr CUIFusionAnvil::LoadSprite(const wchar_t* sPath) {
    // Let _com_ptr_t's assignment operator do the QueryInterface + AddRef.
    // Explicit QueryInterface with a _com_ptr_t* argument writes through a
    // mismatched type and leaves the canvas pointer un-initialised.
    IWzCanvasPtr pCanvas;
    // Try Custom.wz overlay first — Custom.wz overrides only kick in when the
    // original WZ lookup fails, so new children under existing imgs (e.g.
    // UIWindow.img/Synthesizing) must be fetched via an explicit Custom/ prefix.
    try {
        std::wstring sCustom = std::wstring(L"Custom/") + sPath;
        pCanvas = get_unknown(get_rm()->GetObjectA(const_cast<wchar_t*>(sCustom.c_str())));
    } catch (...) {}
    if (!pCanvas) {
        try {
            pCanvas = get_unknown(get_rm()->GetObjectA(const_cast<wchar_t*>(sPath)));
        } catch (...) {}
    }
    return pCanvas;
}

CUIFusionAnvil::CUIFusionAnvil(int nPOS, int nID)
    : m_nItemPOS(nPOS), m_nItemID(nID), m_bBtOkEnable(0), m_nPressedBtn(0),
      m_nHoveredBtn(0),
      m_bDragging(0), m_nDragAnchorX(0), m_nDragAnchorY(0)
{
    m_apItem[0] = m_apItem[1] = nullptr;
    m_anItemPos[0] = m_anItemPos[1] = 0;
    m_anItemID[0] = m_anItemID[1] = 0;

    // Register as the live instance BEFORE CreateWnd — the game may invoke
    // Draw/HitTest on `this` during the CreateWnd call.
    ms_pInstance = this;

    // Create the game window FIRST so whatever CWnd::CreateWnd does to our
    // memory happens before we populate sprite pointers.
    int w = 186, h = 195;
    int x = (get_screen_width() - w) / 2;
    int y = (get_screen_height() - h) / 2;
    CWnd::CreateWnd(this, x, y, w, h, 10, 1, nullptr, 1);

    // Opening-menu sound.
    play_ui_sound(L"MenuUp");

    // Now load sprites — after CreateWnd, so nothing the game does during
    // window setup can stomp these assignments.
    m_pBgCanvas   = LoadSprite(L"UI/UIWindow.img/Synthesizing/backgrnd");
    m_pBgOverlay  = LoadSprite(L"UI/UIWindow.img/Synthesizing/backgrnd1");
    m_pSubTitle   = LoadSprite(L"UI/UIWindow.img/Synthesizing/SubTitle");
    m_pBtOkN      = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtOk/normal/0");
    m_pBtOkP      = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtOk/pressed/0");
    m_pBtOkD      = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtOk/disabled/0");
    m_pBtOkM      = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtOk/mouseOver/0");
    m_pBtCancelN  = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtCancel/normal/0");
    m_pBtCancelP  = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtCancel/pressed/0");
    m_pBtCancelM  = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtCancel/mouseOver/0");
    m_pBtExitN    = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtExit/normal/0");
    m_pBtExitP    = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtExit/pressed/0");
    m_pBtExitM    = LoadSprite(L"UI/UIWindow.img/Synthesizing/BtExit/mouseOver/0");

    // Re-load Effect/0..11. We'll cycle through them one-per-tick in Draw to
    // emulate the native Gr2DLayer animation instead of stacking them.
    for (int i = 0; i < 12; ++i) {
        wchar_t sPath[128];
        swprintf_s(sPath, L"UI/UIWindow.img/Synthesizing/Effect/%d", i);
        m_pEffect[i] = LoadSprite(sPath);
    }

    // Compute a button/sprite rect at its WZ origin. Sprites are anchored such
    // that their origin (-cx, -cy) is the draw-top-left position within the
    // dialog canvas.
    auto RectFromCanvas = [](IWzCanvasPtr pCanvas, int fallbackLeft, int fallbackTop,
                             int fallbackW, int fallbackH) -> RECT {
        if (pCanvas) {
            UINT uW = 0, uH = 0;
            pCanvas->get_width(&uW);
            pCanvas->get_height(&uH);
            int l = -pCanvas->cx;
            int t = -pCanvas->cy;
            return { l, t, l + (int)uW, t + (int)uH };
        }
        return { fallbackLeft, fallbackTop, fallbackLeft + fallbackW, fallbackTop + fallbackH };
    };

    m_rcBtOk     = RectFromCanvas(m_pBtOkN,      43, 171, 40, 17);
    m_rcBtCancel = RectFromCanvas(m_pBtCancelN,  97, 171, 40, 17);
    m_rcBtExit   = RectFromCanvas(m_pBtExitN,   166,   6, 13, 13);

    // 43x43 hit rects at (37,98) and (106,98); icon draw baseline at y=135.
    m_rcSlot[0] = { 37,  98,  80, 141 };
    m_rcSlot[1] = {106,  98, 149, 141 };
}

CUIFusionAnvil::~CUIFusionAnvil() {
    if (ms_pInstance == this) ms_pInstance = nullptr;
}

void CUIFusionAnvil::OnDestroy() {
    if (ms_pInstance == this) ms_pInstance = nullptr;
    CWnd::OnDestroy();
}

void CUIFusionAnvil::Update() {
    InvalidateRect(nullptr);
}

void CUIFusionAnvil::Draw(const RECT* pRect) {
    if (this != ms_pInstance) return;
    CWnd::Draw(pRect);
    IWzCanvasPtr pCanvas = GetCanvas();
    if (!pCanvas) return;

    // CopyEx with CA_REMOVEALPHA forces opaque pixels — hides any alpha
    // gaps in the WZ canvases that were letting the game world bleed through.
    auto BlitAtOrigin = [&](IWzCanvasPtr pSprite) {
        if (!pSprite) return;
        pCanvas->CopyEx(-pSprite->cx, -pSprite->cy, pSprite,
                        CANVAS_ALPHATYPE::CA_REMOVEALPHA,
                        0, 0, 0, 0, 0, 0);
    };

    // Layered background: full bg, inner frame, title bar.
    BlitAtOrigin(m_pBgCanvas);
    BlitAtOrigin(m_pBgOverlay);
    BlitAtOrigin(m_pSubTitle);

    // Cycle through Effect/0..11 at ~10 FPS (100ms per frame) to emulate the
    // native Gr2DLayer animation — one frame per paint rather than all 12.
    int nEffectFrame = (int)(GetTickCount() / 100u) % 12;
    BlitAtOrigin(m_pEffect[nEffectFrame]);

    // Buttons — pick sprite based on state, blit at its own origin.
    // Priority: disabled > pressed > mouseOver > normal (fallback if
    // mouseOver is missing from WZ).
    auto pickBt = [](IWzCanvasPtr n, IWzCanvasPtr p, IWzCanvasPtr m,
                     IWzCanvasPtr d, bool enabled, bool pressed, bool hover) {
        if (!enabled && d) return d;
        if (pressed && p) return p;
        if (hover && m)   return m;
        return n;
    };
    BlitAtOrigin(pickBt(m_pBtOkN, m_pBtOkP, m_pBtOkM, m_pBtOkD,
                        m_bBtOkEnable != 0, m_nPressedBtn == 1,
                        m_nHoveredBtn == 1 && m_nPressedBtn == 0));
    BlitAtOrigin(pickBt(m_pBtCancelN, m_pBtCancelP, m_pBtCancelM, nullptr,
                        true, m_nPressedBtn == 2,
                        m_nHoveredBtn == 2 && m_nPressedBtn == 0));
    BlitAtOrigin(pickBt(m_pBtExitN, m_pBtExitP, m_pBtExitM, nullptr,
                        true, m_nPressedBtn == 3,
                        m_nHoveredBtn == 3 && m_nPressedBtn == 0));

    // Item icons — baseline y=135 in the dialog; x-centers at 42 and 111.
    static constexpr int kIconX[2] = { 42, 111 };
    static constexpr int kIconY    = 135;
    auto pItemInfo = CItemInfo::GetInstance();
    if (pItemInfo) {
        for (int i = 0; i < 2; ++i) {
            if (m_apItem[i] && m_anItemID[i]) {
                pItemInfo->DrawItemIconForSlot(
                    pCanvas, m_anItemID[i],
                    kIconX[i], kIconY,
                    0, 0, 0, 0, 0, 0);
            }
        }
    }
}

void CUIFusionAnvil::OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) {
    POINT pt{rx, ry};
    if (msg == WM_LBUTTONDOWN) {
        if (m_bBtOkEnable && PtInRect(&m_rcBtOk, pt)) {
            m_nPressedBtn = 1; play_ui_sound(L"BtMouseClick");
            InvalidateRect(nullptr); return;
        }
        if (PtInRect(&m_rcBtCancel, pt)) {
            m_nPressedBtn = 2; play_ui_sound(L"BtMouseClick");
            InvalidateRect(nullptr); return;
        }
        if (PtInRect(&m_rcBtExit, pt)) {
            m_nPressedBtn = 3; play_ui_sound(L"BtMouseClick");
            InvalidateRect(nullptr); return;
        }
        if (PtInRect(&m_rcSlot[0], pt) || PtInRect(&m_rcSlot[1], pt)) return;
        if (ry < 44 && !PtInRect(&m_rcBtExit, pt)) {
            m_bDragging = 1;
            m_nDragAnchorX = rx;
            m_nDragAnchorY = ry;
            return;
        }
    } else if (msg == WM_LBUTTONUP) {
        if (m_bDragging) {
            m_bDragging = 0;
            return;
        }
        int pressed = m_nPressedBtn;
        m_nPressedBtn = 0;
        InvalidateRect(nullptr);
        if (pressed == 1 && PtInRect(&m_rcBtOk, pt)) {
            if (m_bBtOkEnable) SendRequestPacket();
            return;
        }
        if (pressed == 2 && PtInRect(&m_rcBtCancel, pt)) { Destroy(); return; }
        if (pressed == 3 && PtInRect(&m_rcBtExit, pt)) {
            play_ui_sound(L"MenuDown");
            Destroy();
            return;
        }
    }
    CWnd::OnMouseButton(msg, wParam, rx, ry);
}

int CUIFusionAnvil::OnMouseMove(int rx, int ry) {
    if (this != ms_pInstance) return 0;
    if (m_bDragging) {
        int dx = rx - m_nDragAnchorX;
        int dy = ry - m_nDragAnchorY;
        if ((dx != 0 || dy != 0) && m_pLayer) {
            m_pLayer->RelOffset(dx, dy, Ztl_variant_t(), Ztl_variant_t());
        }
        return 1;
    }

    // Button hover-sound: play only on a 0 → button transition.
    POINT pt{rx, ry};
    int nNow = 0;
    if (m_bBtOkEnable && PtInRect(&m_rcBtOk, pt))          nNow = 1;
    else if (PtInRect(&m_rcBtCancel, pt))                  nNow = 2;
    else if (PtInRect(&m_rcBtExit, pt))                    nNow = 3;
    if (nNow != m_nHoveredBtn) {
        if (nNow != 0) play_ui_sound(L"BtMouseOver");
        m_nHoveredBtn = nNow;
    }
    return 0;
}

int CUIFusionAnvil::PutItem(GW_ItemSlotBase* pItem, int nTI, int nSlotPos, int rx, int ry) {
    if (!pItem) return 0;
    // Accept both currently-equipped (negative position) and bagged (positive)
    // equips. The server branch resolves position sign to the right inventory.
    int nItemID = DecodeItemID(pItem);
    int nBodyPart = 0;
    if (!get_bodypart_from_item(nItemID, 2, &nBodyPart, 0)) return 0;
    int nWeapon = get_weapon_type(nItemID);

    // Drop location decides left/right slot. 186 is the backgrnd width, so
    // halfway is 93. m_pBgCanvas->cx (origin x) is 0 for backgrnd, so that
    // would make every drop land in slot 1 — use the constant instead.
    int nIndex = rx >= 93 ? 1 : 0;
    int nOther = 1 - nIndex;

    if (m_apItem[nOther]) {
        int nOtherID = m_anItemID[nOther];
        if (nItemID == nOtherID) return 0;
        int nOtherBody = 0;
        if (!get_bodypart_from_item(nOtherID, 2, &nOtherBody, 0) || nOtherBody != nBodyPart) {
            return 0;
        }
        if (nWeapon && nWeapon != get_weapon_type(nOtherID)) return 0;
        m_bBtOkEnable = 1;
    }
    m_apItem[nIndex]    = pItem;
    m_anItemPos[nIndex] = nSlotPos;
    m_anItemID[nIndex]  = nItemID;
    play_ui_sound(L"DragEnd");
    InvalidateRect(nullptr);
    return 1;
}

void CUIFusionAnvil::SendRequestPacket() {
    int pos0 = m_anItemPos[0], pos1 = m_anItemPos[1];

    // play_ui_sound prepends "Sound.wz/UI.img/" via string pool SP_2193, so
    // only the leaf node name is needed here.
    play_ui_sound(L"anvil");

    // Body matches Cosmic's UseCashItemHandler layout (no tick prefix):
    //   short position, int itemId, short basePos, short skinPos
    COutPacket oPacket(kOpcode_UseCashItem);
    oPacket.Encode2(static_cast<unsigned short>(m_nItemPOS));
    oPacket.Encode4(m_nItemID);
    oPacket.Encode2(static_cast<unsigned short>(pos0));
    oPacket.Encode2(static_cast<unsigned short>(pos1));
    SendOutPacket(oPacket);
    Destroy();
}


// ===========================================================================
// Hook 1: GW_ItemSlotBase::Decode — static __cdecl factory.
// ===========================================================================

static auto CInPacket__Decode4 =
    reinterpret_cast<uint32_t(__thiscall*)(CInPacket*)>(kAddr_InPacket_Decode4);
static auto GW_ItemSlotBase__Decode =
    reinterpret_cast<int(__cdecl*)(void*, CInPacket*)>(kAddr_ItemSlotBase_Decode);

int __cdecl GW_ItemSlotBase__Decode_hook(void* pOutZRef, CInPacket* pPacket) {
    int ret = GW_ItemSlotBase__Decode(pOutZRef, pPacket);
    auto* pItem = *reinterpret_cast<GW_ItemSlotBase**>(
        reinterpret_cast<char*>(pOutZRef) + 4);
    if (!pItem) return ret;

    int nType = 0;
    __try { nType = pItem->GetType(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return ret; }
    if (nType != 1) return ret;

    uint32_t nAnvilItemID = 0;
    __try { nAnvilItemID = CInPacket__Decode4(pPacket); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return ret; }
    __try { reinterpret_cast<GW_ItemSlotEquip*>(pItem)->nAnvilItemID = nAnvilItemID; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ret;
}


// ===========================================================================
// Hook 2: AvatarLook::Load — post-hook overwrites anHairEquip[i-1] when the
// equipped item carries a non-zero nAnvilItemID.
// ===========================================================================

static auto AvatarLook__Load =
    reinterpret_cast<void(__thiscall*)(AvatarLook*, int, int, int)>(kAddr_AvatarLook_Load);

void __fastcall AvatarLook__Load_hook(AvatarLook* pThis, void* /*edx*/,
                                       int cs, int apEquipped, int apEquipped2)
{
    AvatarLook__Load(pThis, cs, apEquipped, apEquipped2);

    for (int i = 1; i <= 51; ++i) {
        GW_ItemSlotBase* pItem = nullptr;
        __try {
            pItem = *reinterpret_cast<GW_ItemSlotBase**>(apEquipped + 8 * i + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!pItem) continue;

        int nType = 0;
        __try { nType = pItem->GetType(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (nType != 1) continue;

        int32_t nAnvilItemID = 0;
        __try { nAnvilItemID = reinterpret_cast<GW_ItemSlotEquip*>(pItem)->nAnvilItemID; }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }

        if (nAnvilItemID) {
            pThis->anHairEquip[i - 1] = nAnvilItemID;
        }
    }
}


// ===========================================================================
// Hook 3: CWvsContext::SendConsumeCashItemUseRequest — intercept the Fusion
// Anvil cash item use; open the dialog instead of sending the default packet.
// ===========================================================================

class CWvsContext;
static auto CWvsContext__SendConsumeCashItemUseRequest =
    reinterpret_cast<void(__thiscall*)(CWvsContext*, int, int, int, ZXString<char>)>(kAddr_SendConsumeCash);

void __fastcall CWvsContext__SendConsumeCashItemUseRequest_hook(
    CWvsContext* pThis, void* /*edx*/, int nPOS, int nItemID, int a4, ZXString<char> a5)
{
    if (nItemID != kFusionAnvilItemID) {
        CWvsContext__SendConsumeCashItemUseRequest(pThis, nPOS, nItemID, a4, a5);
        return;
    }
    if (CUIFusionAnvil::ms_pInstance) {
        // Don't stack dialogs; close the existing one — next click opens fresh.
        CUIFusionAnvil::ms_pInstance->Destroy();
        CUIFusionAnvil::ms_pInstance = nullptr;
        return;
    }
    new CUIFusionAnvil(nPOS, nItemID);
}

// Fallback: Etc cash item use path. Some v83 cash items route here instead.
// Signature mirrors SendConsume: (this, pos, itemId).
static auto CWvsContext__SendEtcCashItemUseRequest =
    reinterpret_cast<void(__thiscall*)(CWvsContext*, int, int)>(kAddr_SendEtcCash);

void __fastcall CWvsContext__SendEtcCashItemUseRequest_hook(
    CWvsContext* pThis, void* /*edx*/, int nPOS, int nItemID)
{
    if (nItemID == kFusionAnvilItemID && !CUIFusionAnvil::ms_pInstance) {
        new CUIFusionAnvil(nPOS, nItemID);
        return;
    }
    CWvsContext__SendEtcCashItemUseRequest(pThis, nPOS, nItemID);
}

// The client dispatches cash-item double-clicks through get_consume_cash_item_type.
// It's a lookup in an internal table; unknown itemIds (like our 5900000) return
// 0 and the click is silently ignored. We intercept and return a nonzero token
// for our Fusion Anvil so the client routes through SendConsumeCashItemUseRequest.
static auto get_consume_cash_item_type =
    reinterpret_cast<int32_t(__cdecl*)(int32_t)>(kAddr_get_consume_cash_item_type);

int32_t __cdecl get_consume_cash_item_type_hook(int32_t nItemID) {
    if (nItemID == kFusionAnvilItemID) {
        // Any nonzero classification token works — the client just checks truthiness.
        return 1;
    }
    return get_consume_cash_item_type(nItemID);
}

static auto CDraggableItem__OnDoubleClicked =
    reinterpret_cast<int(__thiscall*)(CDraggableItem*)>(kAddr_DraggableItem_OnDoubleClick);

int __fastcall CDraggableItem__OnDoubleClicked_hook(CDraggableItem* pThis, void* /*edx*/) {
    return CDraggableItem__OnDoubleClicked(pThis);
}


// ===========================================================================
// Hook 4: CDraggableItem::OnDropped — route drops into our dialog.
// ===========================================================================

static auto CDraggableItem__OnDropped =
    reinterpret_cast<int(__thiscall*)(CDraggableItem*, IUIMsgHandler*, IUIMsgHandler*, int, int)>(
        kAddr_DraggableItem_OnDropped);

int __fastcall CDraggableItem__OnDropped_hook(
    CDraggableItem* pThis, void* /*edx*/,
    IUIMsgHandler* pFrom, IUIMsgHandler* pTo, int rx, int ry)
{
    if (pTo && pTo->IsKindOf(&CUIFusionAnvil::ms_RTTI)) {
        // `pTo` arrives as the IUIMsgHandler sub-object pointer (offset +4
        // inside the real CUIFusionAnvil layout). static_cast tells the
        // compiler about the inheritance so it adjusts the pointer back to
        // the real `this`. reinterpret_cast would NOT adjust, causing all
        // field accesses to land 4 bytes past their true offsets.
        CUIFusionAnvil* pDialog = static_cast<CUIFusionAnvil*>(
            static_cast<CWnd*>(static_cast<IUIMsgHandler*>(pTo)));
        // Defensive fallback: if the static_cast didn't land on our instance
        // (e.g., MSVC layout surprise), try the manual -4 adjustment.
        if (pDialog != CUIFusionAnvil::ms_pInstance) {
            auto* alt = reinterpret_cast<CUIFusionAnvil*>(
                reinterpret_cast<char*>(pTo) - 4);
            if (alt == CUIFusionAnvil::ms_pInstance) {
                pDialog = alt;
            } else {
                return 0;
            }
        }
        GW_ItemSlotBase* pItem = nullptr;
        __try {
            pItem = FetchInventoryItem(pThis->m_nItemTI, pThis->m_nSlotPosition);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (!pItem) return 0;
        return pDialog->PutItem(pItem, pThis->m_nItemTI, pThis->m_nSlotPosition, rx, ry);
    }
    return CDraggableItem__OnDropped(pThis, pFrom, pTo, rx, ry);
}


// ===========================================================================
// Installer
// ===========================================================================

void AttachFusionAnvilMod() {
    Patch4(kPatch_GetItemSlotSizeImm, kNewItemSlotEquipSize);
    Patch4(kPatch_SubtypeAllocSizeImm, kNewItemSlotEquipSize);

    ATTACH_HOOK(GW_ItemSlotBase__Decode, GW_ItemSlotBase__Decode_hook);
    ATTACH_HOOK(AvatarLook__Load, AvatarLook__Load_hook);
    ATTACH_HOOK(CWvsContext__SendConsumeCashItemUseRequest,
                CWvsContext__SendConsumeCashItemUseRequest_hook);
    ATTACH_HOOK(CDraggableItem__OnDropped, CDraggableItem__OnDropped_hook);
    ATTACH_HOOK(CWvsContext__SendEtcCashItemUseRequest,
                CWvsContext__SendEtcCashItemUseRequest_hook);
    ATTACH_HOOK(CDraggableItem__OnDoubleClicked, CDraggableItem__OnDoubleClicked_hook);
    ATTACH_HOOK(get_consume_cash_item_type, get_consume_cash_item_type_hook);
}
