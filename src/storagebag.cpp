#include "pch.h"
#include "hook.h"
#include "clientloop.h"
#include "constants.h"
#include "features.h"
#include "serverpackets.h"
#include "debug.h"
#include "v83_offsets.h"
#include "v83_version_guard.h"
#include "wvs/iteminfo.h"
#include "wvs/packet.h"
#include "wvs/util.h"
#include "wvs/wnd.h"
#include "wvs/wndman.h"
#include "ztl/ztl.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

// The Storage Bag window (BotStory server docs/STORAGE_BAG.md): four 200-slot bags per character (ores, scrolls,
// chairs, mounts), opened by a BAG button on the item inventory. The server owns every rule; the window shows the bag
// the server last sent and sends each drag, click and button back.
//
// Ported from the community "Storage Bag" release, its per-character "Repair" version (MapleServer/new client
// stuff/storage bag; the intake analysis is _analysis/inventory-shop.md section 1). Changed for BotStory: our opcodes,
// through serverpackets; the art in Custom.wz under UI/UIWindow.img/StorageBag (tools/make_storage_bag_art.py: the
// release's English art and a MOUNT tab); English text; the BAG button only once the server allowed storage_bag in the
// feature handshake (a server without the bag never shows it); a CLOSE request, so the server stops sending Auto
// pickups to a closed window; a withdraw always lands like a pickup. Left out: the release's slot locks
// (slotlock.cpp) and its unused inventory-button injection.
//
// Protocol (server terms): the client sends Recv opcode 0x119 (byte action, byte bag, then per action); the server
// answers every request but CLOSE with Send opcode 0x17E (byte 1, byte bag, short n, n x (short slot, item),
// n x short quantity, byte auto).
//
// Hooks (addresses: v83_offsets.h bag, checked on an unpacked dump of the shipped build):
//   CUIItem::Draw                the BAG button in the item inventory's title bar
//   CField::OnKey                typing into the search box
//   clientloop::OnItemDropped    an inventory item dropped on the bag (deposit); a bag item dropped on the bag (move) or
//                                anywhere else (withdraw)
//   clientloop::OnLeaveGame      the window closes
//   clientloop::OnClearFieldUI   the window closes (Cash Shop, MTS)
//   clientloop::OnMessage        the BAG button's clicks (the inventory's title bar never passes them to CUIItem)
//   serverpackets 0x17E          the bag's snapshot

namespace {

using namespace v83::bag;
namespace crew = v83::crew;

auto play_ui_sound = reinterpret_cast<void(__cdecl*)(const wchar_t*)>(crew::kPlayUiSound);
auto get_basic_font = reinterpret_cast<IWzFontPtr*(__cdecl*)(IWzFontPtr*, int)>(crew::kGetBasicFont);

// ---- protocol ---------------------------------------------------------------------------------------------------

constexpr int kReqOpen = 0, kReqWithdraw = 1, kReqDeposit = 2, kReqOrganize = 3, kReqSetAuto = 4, kReqMove = 5,
              kReqClose = 6;
constexpr int kRespSnapshot = 1;
constexpr int kKindCount = 4; // ore, scroll, chair, mount: the tabs, in the server's order

// The server answered the handshake and allowed storage_bag on this connection.
bool Enabled() {
    return FeatureHandshakeAllows("storage_bag");
}

// True from a transfer request until its snapshot arrives: the slots may move, so the grid takes no clicks meanwhile.
// Also tells a reply from an Auto pickup the server pushed (only the latter makes the new-item glow).
bool g_bAwaiting = false;

void Send(int action, int kind, std::initializer_list<int> shorts = {}) {
    COutPacket o(kSendOpcode);
    o.Encode1(static_cast<unsigned char>(action));
    o.Encode1(static_cast<unsigned char>(kind));
    for (int v : shorts) {
        o.Encode2(static_cast<unsigned short>(v));
    }
    serverpackets::Send(o);
}

void SendSetAuto(int kind, bool on) {
    COutPacket o(kSendOpcode);
    o.Encode1(static_cast<unsigned char>(kReqSetAuto));
    o.Encode1(static_cast<unsigned char>(kind));
    o.Encode1(on ? 1 : 0);
    serverpackets::Send(o);
}

void SendTransfer(int action, int kind, std::initializer_list<int> shorts) {
    Send(action, kind, shorts);
    g_bAwaiting = true;
}

// ---- engine helpers ---------------------------------------------------------------------------------------------

// Absolute (screen) cursor position from the input system: the same one the drag uses, so the BAG button's hit test
// and the drop point agree with what the player sees.
bool GetAbsCursor(POINT& sp) {
    sp = {0, 0};
    void* input = *reinterpret_cast<void**>(kInputSystem);
    if (!input) {
        return false;
    }
    reinterpret_cast<void(__thiscall*)(void*, POINT*)>(kGetCursorPos)(input, &sp);
    return true;
}

// The cursor sprite (UI/Basic.img/Cursor): our window and the BAG button set their own, as the engine never sees them.
constexpr int kCursorArrow = 0, kCursorAskClick = 1, kCursorItemHand = 5, kCursorScrollHover = 7, kCursorScrollV = 9,
              kCursorItemGrab = 11, kCursorButtonPress = 12;
void SetCursorState(int state) {
    void* input = *reinterpret_cast<void**>(kInputSystem);
    if (input) {
        reinterpret_cast<void(__thiscall*)(void*, int)>(kSetCursorState)(input, state);
    }
}

// The item inventory window (CUIItem), or null while it is closed.
void* SehItemWnd() {
    void* p = nullptr;
    __try {
        p = *reinterpret_cast<void**>(kItemWnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p;
}

int SehWndWidth(void* pWnd) {
    int w = 0;
    __try {
        w = reinterpret_cast<CWnd*>(pWnd)->m_width;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        w = 0;
    }
    return w;
}

void SehWndAbs(void* pWnd, int& l, int& t) {
    l = 0;
    t = 0;
    __try {
        void* handler = reinterpret_cast<char*>(pWnd) + 4; // the IUIMsgHandler part, whose vtable has these two
        l = reinterpret_cast<int(__thiscall*)(void*)>(kGetAbsLeft)(handler);
        t = reinterpret_cast<int(__thiscall*)(void*)>(kGetAbsTop)(handler);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        l = 0;
        t = 0;
    }
}

void SehInvalidate(void* pWnd) {
    __try {
        reinterpret_cast<CWnd*>(pWnd)->InvalidateRect(nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Whether the item inventory is on screen: closing it destroys CUIItem (null), hiding it clears its layer's visible flag.
// An unreadable answer counts as shown, so the bag never closes by mistake.
bool IsInventoryShown() {
    void* inv = SehItemWnd();
    if (!inv) {
        return false;
    }
    try {
        IWzGr2DLayerPtr layer = reinterpret_cast<CWnd*>(inv)->m_pLayer;
        return !layer || layer->visible != 0;
    } catch (...) {
        return true;
    }
}

// True while the engine has any drag in flight (from the bag or into it): CWndMan keeps its source until the drop.
bool IsEngineDragActive() {
    if (!CWndMan::IsInstantiated()) {
        return false;
    }
    void* src = nullptr;
    __try {
        src = *reinterpret_cast<void**>(reinterpret_cast<char*>(CWndMan::GetInstance()) + kWndManDragSource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        src = nullptr;
    }
    return src != nullptr;
}

// ---- items ------------------------------------------------------------------------------------------------------

struct GW_ItemSlotBase : public ZRefCounted {
    virtual ~GW_ItemSlotBase() = 0;
};

int SehItemId(void* pItem) {
    if (!pItem) {
        return 0;
    }
    int id = 0;
    __try {
        id = reinterpret_cast<int(__thiscall*)(const void*)>(kItemIdGetData)(reinterpret_cast<char*>(pItem) + 0xC);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id = 0;
    }
    return id;
}

// One item with the client's own decoder (which fusion_anvil_field extends): a reference we own, or null.
GW_ItemSlotBase* SehDecodeItem(CInPacket* pPacket) {
    struct {
        void* unused;
        void* item;
    } out = {};
    __try {
        reinterpret_cast<int(__cdecl*)(void*, CInPacket*)>(kDecodeItem)(&out, pPacket);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.item = nullptr;
    }
    return reinterpret_cast<GW_ItemSlotBase*>(out.item);
}

// An item's display name from String.wz, lower-cased and cached (the search box matches against it).
const char* ItemCategoryImg(int id) {
    switch (id / 1000000) {
        case 1: return "Eqp";     // Eqp.img/Eqp/<category>/<id> (mounts under Taming)
        case 2: return "Consume"; // Consume.img/<id>
        case 3: return "Ins";     // Ins.img/<id>
        case 4: return "Etc";     // Etc.img/Etc/<id>
        default: return nullptr;
    }
}

const std::string& ItemNameLower(int id) {
    static std::unordered_map<int, std::string> s_cache;
    auto it = s_cache.find(id);
    if (it != s_cache.end()) {
        return it->second;
    }
    std::string name;
    const char* img = ItemCategoryImg(id);
    if (img) {
        try {
            std::wstring wimg(img, img + strlen(img));
            IWzPropertyPtr root = get_rm()->GetObjectA(Ztl_bstr_t((L"String/" + wimg + L".img").c_str())).GetUnknown();
            Ztl_bstr_t sId = std::to_wstring(id).c_str();
            IWzPropertyPtr node;
            if (root && id / 1000000 == 1) {
                IWzPropertyPtr eqp = root->item[wimg.c_str()].GetUnknown();
                static const wchar_t* kCategories[] = {L"Taming", L"Weapon", L"Cap", L"Accessory", L"Coat", L"Longcoat",
                        L"Pants", L"Shoes", L"Glove", L"Shield", L"Cape", L"Ring", L"PetEquip"};
                for (const wchar_t* c : kCategories) {
                    IWzPropertyPtr cat = eqp ? IWzPropertyPtr(eqp->item[c].GetUnknown()) : nullptr;
                    if (cat) {
                        node = cat->item[sId].GetUnknown();
                        if (node) {
                            break;
                        }
                    }
                }
            } else if (root) {
                node = root->item[sId].GetUnknown();
                if (!node) {
                    IWzPropertyPtr wrap = root->item[wimg.c_str()].GetUnknown();
                    if (wrap) {
                        node = wrap->item[sId].GetUnknown();
                    }
                }
            }
            if (node) {
                Ztl_variant_t v = node->item[L"name"];
                if (v.vt == VT_BSTR) {
                    name = static_cast<const char*>(_bstr_t(v));
                }
            }
        } catch (...) {
        }
    }
    for (char& c : name) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return s_cache.emplace(id, std::move(name)).first->second;
}

// ---- the bags the server sent -------------------------------------------------------------------------------------

constexpr int kSlots = 200;

struct BagStore {
    ZRef<GW_ItemSlotBase> item[kSlots];
    int id[kSlots] = {};
    int qty[kSlots] = {};
    bool isNew[kSlots] = {}; // arrived by an Auto pickup: glows until hovered or picked up
    int count = 0;           // one past the highest slot in use
    bool ready = false;
    bool autoOn = false;
};
BagStore g_bags[kKindCount];

void FreeBag(int kind) {
    BagStore& b = g_bags[kind];
    for (int i = 0; i < kSlots; ++i) {
        b.item[i] = ZRef<GW_ItemSlotBase>();
        b.id[i] = 0;
        b.qty[i] = 0;
        b.isNew[i] = false;
    }
    b.count = 0;
    b.ready = false;
}

// ---- the drag icon ----------------------------------------------------------------------------------------------

constexpr int kDragIconSize = 44, kDragIconZ = 9999;
IWzGr2DLayer* g_pDragIcon = nullptr; // our reference; the engine keeps its own in the draggable
bool g_bItemDragging = false;        // a drag that started in the bag is in flight

IWzGr2DLayer* CreateDragIconLayer(int itemId, int x, int y, IUnknown* pOrigin) {
    try {
        CItemInfo* info = CItemInfo::GetInstance();
        if (!itemId || !info || !get_gr()) {
            return nullptr;
        }
        IWzCanvasPtr canvas;
        PcCreateObject<IWzCanvasPtr>(L"Canvas", canvas, nullptr);
        if (!canvas) {
            return nullptr;
        }
        canvas->Create(kDragIconSize, kDragIconSize, vtMissing, vtMissing);
        info->DrawItemIconForSlot(canvas, itemId, 6, kDragIconSize - 6, 0, 0, 0, 0, 0, 0);
        IWzGr2DLayerPtr layer = get_gr()->CreateLayer(0, 0, kDragIconSize, kDragIconSize, kDragIconZ,
                static_cast<IUnknown*>(canvas), vtMissing);
        if (!layer) {
            return nullptr;
        }
        if (pOrigin) {
            layer->origin = pOrigin;
        }
        layer->color = 0xFFFFFFFF;
        layer->RelMove(x - kDragIconSize / 2, y - kDragIconSize / 2);
        layer->visible = 1;
        IWzGr2DLayer* raw = layer;
        raw->AddRef();
        return raw;
    } catch (...) {
        return nullptr;
    }
}

void EndDragIcon() {
    g_bItemDragging = false;
    if (!g_pDragIcon) {
        return;
    }
    try {
        g_pDragIcon->visible = 0;
    } catch (...) {
    }
    g_pDragIcon->Release();
    g_pDragIcon = nullptr;
}

// The stock item inventory's own drag start (CUIItem::OnMouseButton, 0x0081D7D6): a CDraggableItem from the game's
// allocator, the icon, the source slot at +0x1C and the source handler at +0x24, then CWndMan::BeginDragDrop. The
// drag ends on the next click, which fires CDraggableItem::OnDropped (hooked below).
bool SehBeginDrag(void* source, IWzGr2DLayer* pIcon, int slot) {
    void* d = nullptr;
    __try {
        d = reinterpret_cast<void*(__thiscall*)(void*, size_t)>(kZAllocAlloc)(reinterpret_cast<void*>(kZAllocInstance),
                0x28);
        if (!d) {
            return false;
        }
        reinterpret_cast<void*(__thiscall*)(void*, void*)>(kDraggableInit)(d, pIcon);
        *reinterpret_cast<int*>(static_cast<char*>(d) + 0x18) = 0;
        *reinterpret_cast<int*>(static_cast<char*>(d) + 0x1C) = slot;
        *reinterpret_cast<int*>(static_cast<char*>(d) + 0x20) = 0;
        *reinterpret_cast<void**>(static_cast<char*>(d) + 0x24) = source;
        *reinterpret_cast<void**>(d) = reinterpret_cast<void*>(kDraggableItemVtbl);
        reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(kBeginDragDrop)(CWndMan::GetInstance(), source, d);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- the window -------------------------------------------------------------------------------------------------

// Layout on the art (Custom.wz UI/UIWindow.img/StorageBag/backgrnd, 207x248): a title strip, the tabs below it, a 5x5
// grid of 31 px wells (36 x 34 pitch), the scrollbar on the right and the search box at the bottom.
constexpr int kWndW = 207, kWndH = 248, kTitleH = 20;
constexpr int kCols = 5, kVisRows = 5, kVisCount = kCols * kVisRows;
constexpr int kCell = 31, kColW = 36, kRowH = 34, kGridLeft = 8, kGridTop = 50;
constexpr int kScrollW = 15, kScrollX = 186, kArrowH = 13, kScrollTop = kGridTop;
constexpr int kScrollBot = kGridTop + (kVisRows - 1) * kRowH + kCell;
constexpr int kThumbMinH = 20;
constexpr int kSearchL = 8, kSearchR = 199, kSearchTop = 222, kSearchBot = 242, kSearchTextX = 6, kSearchTextY = 227;
constexpr int kSearchMax = 32;
constexpr RECT kRcAuto = {150, 5, 171, 17}, kRcSort = {173, 5, 185, 17}, kRcClose = {187, 5, 199, 17};
constexpr int kTabTop = 22, kTabBot = 40, kPillTop = 21, kPillH = 19, kPillPad = 2;
constexpr int kTabLeft[kKindCount] = {8, 56, 104, 152};
constexpr int kTabRight[kKindCount] = {56, 104, 152, 200};
const wchar_t* const kTabArt[kKindCount] = {L"Ore", L"Scroll", L"Chair", L"Mount"};
const wchar_t* const kSearchHint[kKindCount] = {L"Search ores...", L"Search scrolls...", L"Search chairs...",
        L"Search mounts..."};

bool g_bSavedPos = false;
int g_nSavedX = 0, g_nSavedY = 0, g_nSavedKind = 0;

class CBagWnd : public CWnd {
public:
    inline static CBagWnd* ms_pInstance = nullptr;
    inline static CRTTI ms_RTTI{reinterpret_cast<const CRTTI*>(crew::kRttiCWnd)};

    int m_nKind;
    int m_nScroll = 0;
    bool m_bTell = true;               // OnDestroy sends CLOSE (not while the connection goes)
    int m_nCloseHover = 0, m_nClosePressed = 0, m_nSortHover = 0, m_nSortPressed = 0, m_nAutoHover = 0,
        m_nAutoPressed = 0;
    bool m_bScrollDrag = false;
    int m_nScrollGrabDY = 0;
    int m_nCursor = 0;

    alignas(8) unsigned char m_aTip[0x600]; // a native CUIToolTip, as setitems.cpp makes its own
    bool m_bTip = false;
    int m_nTipKey = 0;

    IWzFontPtr m_pFont, m_pFontHint;
    IWzCanvasPtr m_pBg, m_pTab[kKindCount][2], m_pPill[3][2], m_pClose[2], m_pSort[3], m_pAuto[4];
    IWzCanvasPtr m_pScrollPrev, m_pScrollNext, m_pScrollBase, m_pScrollThumb, m_pDigit[10], m_pGlow[4];
    int m_aDigitW[10] = {};

    bool m_bSearch = false;
    char m_sSearch[kSearchMax + 1] = {};
    int m_nSearchLen = 0;
    int m_aShown[kSlots] = {}; // while searching: the matching slots, packed
    int m_nShown = 0;

    CBagWnd(int kind, int x, int y) : m_nKind(kind) {
        ms_pInstance = this;
        LoadArt();
        CWnd::CreateWnd(this, x, y, kWndW, kWndH, 10, 1, nullptr, 0);
        play_ui_sound(L"MenuUp");
        g_bAwaiting = false;
        Send(kReqOpen, m_nKind);
    }
    virtual ~CBagWnd() override {
        if (ms_pInstance == this) {
            ms_pInstance = nullptr;
        }
    }

    static IWzCanvasPtr Sprite(const wchar_t* sPath) {
        IWzCanvasPtr p;
        try {
            p = get_unknown(get_rm()->GetObjectA(Ztl_bstr_t(sPath)));
        } catch (...) {
        }
        return p;
    }
    static IWzFontPtr Font(int nType) {
        IWzFontPtr p;
        try {
            get_basic_font(std::addressof(p), nType);
        } catch (...) {
        }
        return p;
    }
    static int Width(IWzCanvasPtr& c, int fallback) {
        try {
            return c ? static_cast<int>(c->width) : fallback;
        } catch (...) {
            return fallback;
        }
    }
    static int Height(IWzCanvasPtr& c, int fallback) {
        try {
            return c ? static_cast<int>(c->height) : fallback;
        } catch (...) {
            return fallback;
        }
    }

    void LoadArt() {
        const wchar_t* art = L"Custom/UI/UIWindow.img/StorageBag/";
        auto own = [&](const std::wstring& s) { return Sprite((art + s).c_str()); };
        m_pBg = own(L"backgrnd");
        for (int k = 0; k < kKindCount; ++k) {
            m_pTab[k][0] = own(std::wstring(L"tab") + kTabArt[k] + L"Off");
            m_pTab[k][1] = own(std::wstring(L"tab") + kTabArt[k]);
        }
        const wchar_t* pill[3] = {L"left", L"fill", L"right"};
        for (int i = 0; i < 3; ++i) {
            for (int s = 0; s < 2; ++s) {
                m_pPill[i][s] = Sprite((std::wstring(L"UI/Basic.img/Tab2/") + pill[i] + std::to_wstring(s)).c_str());
            }
        }
        m_pClose[0] = own(L"BtClose/normal/0");
        m_pClose[1] = own(L"BtClose/mouseOver/0");
        m_pSort[0] = Sprite(L"UI/UIWindow.img/Item/BtSort/normal/0");
        m_pSort[1] = Sprite(L"UI/UIWindow.img/Item/BtSort/pressed/0");
        m_pSort[2] = Sprite(L"UI/UIWindow.img/Item/BtSort/mouseOver/0");
        m_pAuto[0] = own(L"BtAuto/normal/0");
        m_pAuto[1] = own(L"BtAuto/pressed/0");
        m_pAuto[2] = own(L"BtAuto/mouseOver/0");
        m_pAuto[3] = own(L"BtAuto/disabled/0");
        m_pScrollPrev = Sprite(L"UI/Basic.img/VScr4/enabled/prev0");
        m_pScrollNext = Sprite(L"UI/Basic.img/VScr4/enabled/next0");
        m_pScrollBase = Sprite(L"UI/Basic.img/VScr4/enabled/base");
        m_pScrollThumb = Sprite(L"UI/Basic.img/VScr4/enabled/thumb0");
        for (int i = 0; i < 10; ++i) {
            m_pDigit[i] = Sprite((L"UI/Basic.img/ItemNo/" + std::to_wstring(i)).c_str());
            int w = Width(m_pDigit[i], 0);
            m_aDigitW[i] = w > 0 && w < 32 ? w : (i == 1 ? 5 : 8);
        }
        for (int i = 0; i < 4; ++i) {
            m_pGlow[i] = Sprite((L"UI/UIWindow.img/Item/New/inventory/" + std::to_wstring(i)).c_str());
        }
        m_pFont = Font(crew::kFontBlack);
        m_pFontHint = Font(crew::kFontDarkGrey);
    }

    static void Blit(IWzCanvasPtr& dst, IWzCanvasPtr& src, int x, int y, bool opaque = false) {
        if (dst && src) {
            try {
                dst->CopyEx(x, y, src, opaque ? CANVAS_ALPHATYPE::CA_REMOVEALPHA : CANVAS_ALPHATYPE::CA_OVERWRITE,
                        0, 0, 0, 0, 0, 0);
            } catch (...) {
            }
        }
    }

    BagStore& Bag() const {
        return g_bags[m_nKind];
    }
    // The window's screen position (the engine moves it by its title bar, see HitTest).
    int AbsX() {
        try {
            return GetAbsLeft();
        } catch (...) {
            return 0;
        }
    }
    int AbsY() {
        try {
            return GetAbsTop();
        } catch (...) {
            return 0;
        }
    }

    // ---- search ----
    bool Filtering() const {
        return m_nSearchLen > 0;
    }
    bool Matches(int id) const {
        if (!id) {
            return false;
        }
        const std::string& name = ItemNameLower(id);
        if (!name.empty() && strstr(name.c_str(), m_sSearch)) {
            return true;
        }
        char ids[16];
        _snprintf_s(ids, sizeof(ids), _TRUNCATE, "%d", id);
        return strstr(ids, m_sSearch) != nullptr; // an item without a name still matches its id
    }
    void Refilter() {
        m_nShown = 0;
        if (Filtering()) {
            BagStore& b = Bag();
            for (int i = 0; i < b.count && i < kSlots; ++i) {
                if (Matches(b.id[i])) {
                    m_aShown[m_nShown++] = i;
                }
            }
        }
        ClampScroll();
    }
    void SearchClear() {
        m_nSearchLen = 0;
        m_sSearch[0] = 0;
        m_nScroll = 0;
        Refilter();
    }
    // A key while the search box has the focus: true when it was for the box (the game never sees it).
    bool OnSearchKey(unsigned int vk) {
        if (vk == VK_ESCAPE) {
            SearchClear();
            m_bSearch = false;
            return true;
        }
        if (vk == VK_RETURN) {
            m_bSearch = false;
            return true;
        }
        if (vk == VK_BACK) {
            if (m_nSearchLen > 0) {
                m_sSearch[--m_nSearchLen] = 0;
                m_nScroll = 0;
                Refilter();
            }
            return true;
        }
        BYTE keys[256];
        WORD out = 0;
        if (!GetKeyboardState(keys) || ToAscii(vk, MapVirtualKey(vk, MAPVK_VK_TO_VSC), keys, &out, 0) != 1) {
            return false; // arrows, F-keys: the game keeps them (moving still works)
        }
        char ch = static_cast<char>(tolower(out & 0xFF));
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7E) {
            return true;
        }
        if (m_nSearchLen < kSearchMax) {
            m_sSearch[m_nSearchLen++] = ch;
            m_sSearch[m_nSearchLen] = 0;
            m_nScroll = 0;
            Refilter();
        }
        return true;
    }
    // Width of the typed text in the 12 px font, for the caret (the canvas tells the height only).
    static int TextWidth(const char* s, int n) {
        int w = 0;
        for (int i = 0; i < n; ++i) {
            switch (s[i]) {
                case ' ': w += 3; break;
                case 'i': case 'j': case 'l': case '.': case ',': case '\'': case '!': case '|': case ':': case ';':
                    w += 2; break;
                case 'f': case 'r': case 't': case '(': case ')': w += 4; break;
                case 'm': case 'w': w += 8; break;
                default: w += 6; break;
            }
        }
        return w;
    }

    // ---- grid and scrollbar ----
    int ShownSlot(int g) const {
        if (Filtering()) {
            return g >= 0 && g < m_nShown ? m_aShown[g] : -1;
        }
        return g >= 0 && g < kSlots ? g : -1;
    }
    int TotalRows() const {
        int items = Filtering() ? m_nShown : kSlots;
        int rows = (items + kCols - 1) / kCols;
        return rows < kVisRows ? kVisRows : rows;
    }
    int MaxScroll() const {
        return (std::max)(0, TotalRows() - kVisRows);
    }
    void ClampScroll() {
        m_nScroll = (std::max)(0, (std::min)(m_nScroll, MaxScroll()));
    }
    void ThumbGeom(int& origin, int& thumbH, int& travel) const {
        int top = kScrollTop + kArrowH, span = (kScrollBot - kArrowH) - top;
        thumbH = (std::max)(kThumbMinH, (std::min)(span, span * kVisRows / TotalRows()));
        origin = top;
        travel = span - thumbH;
    }
    RECT ThumbRect() const {
        int origin, thumbH, travel;
        ThumbGeom(origin, thumbH, travel);
        int top = origin + (MaxScroll() > 0 ? travel * m_nScroll / MaxScroll() : 0);
        return {kScrollX, top, kScrollX + kScrollW, top + thumbH};
    }
    static RECT CellRect(int v) {
        int left = kGridLeft + (v % kCols) * kColW, top = kGridTop + (v / kCols) * kRowH;
        return {left, top, left + kCell, top + kCell};
    }
    // The bag slot under a point, honouring the search; -1 while a transfer is on its way (the slots may move).
    int HitSlot(int rx, int ry) const {
        if (g_bAwaiting) {
            return -1;
        }
        POINT pt{rx, ry};
        for (int v = 0; v < kVisCount; ++v) {
            RECT rc = CellRect(v);
            if (PtInRect(&rc, pt)) {
                return ShownSlot(m_nScroll * kCols + v);
            }
        }
        return -1;
    }
    // The cell under the cursor as a drop target (empty cells too); -1 while searching (cells are not slots then).
    int DropSlot() {
        POINT sp;
        if (Filtering() || !GetAbsCursor(sp)) {
            return -1;
        }
        POINT pt{sp.x - AbsX(), sp.y - AbsY()};
        for (int v = 0; v < kVisCount; ++v) {
            RECT rc = CellRect(v);
            if (PtInRect(&rc, pt)) {
                return m_nScroll * kCols + v;
            }
        }
        return -1;
    }

    // ---- tooltip ----
    void ShowTip(int x, int y, void* pItem) {
        if (!m_bTip) {
            reinterpret_cast<void(__thiscall*)(void*)>(0x008E49B5)(m_aTip); // CUIToolTip::CUIToolTip
            m_bTip = true;
        }
        SehShowTip(m_aTip, x, y, pItem);
    }
    static void SehShowTip(void* tip, int x, int y, void* pItem) {
        __try {
            reinterpret_cast<void(__thiscall*)(void*, int, int, void*, void*, void*, int, int, int)>(kShowItemToolTip)(
                    tip, x, y, pItem, nullptr, nullptr, 0, 0, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    void HideTip() {
        if (m_bTip) {
            SehClearTip(m_aTip);
        }
        m_nTipKey = 0;
    }
    static void SehClearTip(void* tip) {
        __try {
            reinterpret_cast<void(__thiscall*)(void*)>(0x008E6E23)(tip); // CUIToolTip::ClearToolTip
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // ---- actions ----
    void SwitchTab(int kind) {
        if (kind == m_nKind) {
            return;
        }
        m_nKind = kind;
        m_nScroll = 0;
        SearchClear();
        HideTip();
        play_ui_sound(L"BtMouseClick");
        Send(kReqOpen, kind);
    }
    void BeginItemDrag(int slot, int rx, int ry) {
        int id = Bag().id[slot];
        if (g_bAwaiting || !id || !CWndMan::IsInstantiated()) {
            return;
        }
        Bag().isNew[slot] = false;
        m_nCursor = -1;
        EndDragIcon();
        IUnknown* origin = nullptr;
        try {
            origin = m_pLayer ? static_cast<IUnknown*>(m_pLayer->origin) : nullptr;
        } catch (...) {
        }
        IWzGr2DLayer* icon = CreateDragIconLayer(id, AbsX() + rx, AbsY() + ry, origin);
        if (!icon) {
            return; // BeginDragDrop throws without an icon
        }
        g_pDragIcon = icon;
        g_bItemDragging = true;
        if (!SehBeginDrag(static_cast<IUIMsgHandler*>(this), icon, slot)) {
            EndDragIcon();
            return;
        }
        SetCursorState(kCursorItemGrab);
        HideTip();
    }
    void Withdraw(int slot) {
        if (!g_bAwaiting && slot >= 0 && slot < kSlots && Bag().id[slot]) {
            SendTransfer(kReqWithdraw, m_nKind, {slot, -1});
            play_ui_sound(L"DragEnd");
            HideTip();
        }
    }

    // ---- CWnd ----
    virtual void Update() override {
        if (!IsInventoryShown()) { // the bag closes with the inventory
            Destroy();
            return;
        }
        InvalidateRect(nullptr); // the new-item glow and the caret move
    }
    virtual void OnKey(unsigned int wParam, unsigned int lParam) override {
        void* ctx = *reinterpret_cast<void**>(crew::kWvsContextInstance);
        if (ctx) { // the game's own window keys (Esc and the window hotkeys)
            reinterpret_cast<int(__thiscall*)(void*, unsigned int, unsigned int)>(crew::kContextOnKey)(ctx, wParam,
                    lParam);
        }
    }
    virtual int OnSetFocus(int) override {
        return 0; // a focused window would stop the character's movement keys
    }
    virtual int HitTest(int rx, int ry, CCtrlWnd** ppCtrl) override {
        int hit = CWnd::HitTest(rx, ry, ppCtrl);
        if (!hit) {
            return 0;
        }
        if (ppCtrl && *ppCtrl) {
            return hit;
        }
        // as the Crew window: the title bar moves the window (1), the rest is ours (2), the title buttons too
        POINT pt{rx, ry};
        bool button = PtInRect(&kRcAuto, pt) || PtInRect(&kRcSort, pt) || PtInRect(&kRcClose, pt);
        return ry >= 0 && ry < kTitleH && !button ? 1 : 2;
    }
    virtual const CRTTI* GetRTTI() const override {
        return &ms_RTTI;
    }
    virtual int IsKindOf(const CRTTI* pRTTI) const override {
        return ms_RTTI.IsKindOf(pRTTI);
    }

    virtual void Draw(const RECT* pRect) override;
    virtual void OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) override;
    virtual int OnMouseMove(int rx, int ry) override;
    virtual int OnMouseWheel(int rx, int ry, int nWheel) override;
    virtual void OnMouseEnter(int bEnter) override;
    virtual void OnDestroy() override;
};

void CBagWnd::Draw(const RECT* pRect) {
    CWnd::Draw(pRect);
    IWzCanvasPtr c = GetCanvas();
    if (!c) {
        return;
    }
    Blit(c, m_pBg, 0, 0, true);

    // tabs: the inventory's pink pill behind the shown bag, grey behind the others, the label centred on each
    for (int k = 0; k < kKindCount; ++k) {
        int sel = k == m_nKind ? 1 : 0;
        int x = kTabLeft[k] + kPillPad, w = kTabRight[k] - kTabLeft[k] - 2 * kPillPad;
        int lw = Width(m_pPill[0][sel], 4), rw = Width(m_pPill[2][sel], 4);
        Blit(c, m_pPill[0][sel], x, kPillTop);
        if (m_pPill[1][sel]) {
            try {
                c->CopyEx(x + lw, kPillTop, m_pPill[1][sel], CANVAS_ALPHATYPE::CA_OVERWRITE,
                        (std::max)(1, w - lw - rw), kPillH, 0, 0, 0, 0);
            } catch (...) {
            }
        }
        Blit(c, m_pPill[2][sel], x + w - rw, kPillTop);
        IWzCanvasPtr& label = m_pTab[k][sel];
        Blit(c, label, kTabLeft[k] + (kTabRight[k] - kTabLeft[k] - Width(label, 0)) / 2,
                kPillTop + (kPillH - Height(label, 5)) / 2);
    }

    // items, with the stack count bottom-left like the inventory
    BagStore& b = Bag();
    CItemInfo* info = CItemInfo::GetInstance();
    for (int v = 0; v < kVisCount; ++v) {
        int slot = ShownSlot(m_nScroll * kCols + v);
        if (slot < 0 || !b.ready || !b.id[slot]) {
            continue;
        }
        RECT rc = CellRect(v);
        if (info) {
            info->DrawItemIconForSlot(c, b.id[slot], rc.left, rc.top + kCell - 1, 0, 0, 0, 0, 0, 0);
        }
        if (b.isNew[slot] && m_pGlow[0]) {
            int f = static_cast<int>((GetTickCount() / 150) % 4);
            Blit(c, m_pGlow[f] ? m_pGlow[f] : m_pGlow[0], rc.left + (kCell - 36) / 2, rc.top + (kCell - 36) / 2);
        }
        if (b.qty[slot] > 1) {
            char num[12];
            _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", b.qty[slot]);
            int x = rc.left + 1;
            for (const char* p = num; *p; ++p) {
                int d = *p - '0';
                Blit(c, m_pDigit[d], x, rc.bottom - 14);
                x += m_aDigitW[d];
            }
        }
    }

    // scrollbar: track, arrows, and the thumb 3-sliced from its 25 px art
    auto stretch = [&](IWzCanvasPtr& sp, int y, int h, int sy, int sh) {
        if (sp && h > 0) {
            try {
                c->CopyEx(kScrollX, y, sp, CANVAS_ALPHATYPE::CA_OVERWRITE, kScrollW, h, 0, sy, sh ? kScrollW : 0, sh);
            } catch (...) {
            }
        }
    };
    stretch(m_pScrollBase, kScrollTop + kArrowH, (kScrollBot - kArrowH) - (kScrollTop + kArrowH), 0, 0);
    Blit(c, m_pScrollPrev, kScrollX, kScrollTop);
    Blit(c, m_pScrollNext, kScrollX, kScrollBot - kArrowH);
    RECT th = ThumbRect();
    int thH = th.bottom - th.top;
    constexpr int kThumbSrcH = 25, kCap = 6;
    if (thH < 2 * kCap + 2) {
        stretch(m_pScrollThumb, th.top, thH, 0, 0);
    } else {
        stretch(m_pScrollThumb, th.top, kCap, 0, kCap);
        stretch(m_pScrollThumb, th.top + kCap, thH - 2 * kCap, kCap, kThumbSrcH - 2 * kCap);
        stretch(m_pScrollThumb, th.bottom - kCap, kCap, kThumbSrcH - kCap, kCap);
    }

    // title-bar buttons: Auto (lit while on for this bag), Organize, close
    bool on = b.autoOn;
    Blit(c, m_nAutoPressed ? m_pAuto[1] : on ? (m_nAutoHover ? m_pAuto[2] : m_pAuto[0]) : m_pAuto[3], kRcAuto.left,
            kRcAuto.top);
    Blit(c, m_nSortPressed ? m_pSort[1] : m_nSortHover ? m_pSort[2] : m_pSort[0], kRcSort.left, kRcSort.top);
    int sink = m_nClosePressed ? 1 : 0;
    Blit(c, m_nCloseHover ? m_pClose[1] : m_pClose[0], kRcClose.left + sink, kRcClose.top + sink);
    if (m_nClosePressed || m_nSortPressed || m_nAutoPressed) {
        SetCursorState(kCursorButtonPress); // the engine resets the sprite every frame
    } else if (g_bItemDragging) {
        SetCursorState(kCursorItemGrab);
    }

    // search box: the typed text or a hint, a blinking caret while it has the focus, and the number of matches
    try {
        if (m_nSearchLen > 0 && m_pFont) {
            c->DrawTextA(kSearchTextX, kSearchTextY, Ztl_bstr_t(m_sSearch), m_pFont, Ztl_variant_t(), Ztl_variant_t());
        } else if (!m_bSearch && m_pFontHint) {
            c->DrawTextA(kSearchTextX, kSearchTextY, Ztl_bstr_t(kSearchHint[m_nKind]), m_pFontHint, Ztl_variant_t(),
                    Ztl_variant_t());
        }
        if (m_bSearch && ((GetTickCount() / 500) & 1) == 0) {
            c->DrawRectangle(kSearchTextX + TextWidth(m_sSearch, m_nSearchLen), kSearchTextY, 1, 12, 0xFF202020);
        }
        if (Filtering() && m_pFont) {
            char n[16];
            _snprintf_s(n, sizeof(n), _TRUNCATE, "%d", m_nShown);
            c->DrawTextA(kSearchR - static_cast<int>(strlen(n)) * 6 - 6, kSearchTextY, Ztl_bstr_t(n), m_pFont,
                    Ztl_variant_t(), Ztl_variant_t());
        }
    } catch (...) {
    }
}

void CBagWnd::OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) {
    POINT pt{rx, ry};
    RECT search = {kSearchL, kSearchTop, kSearchR, kSearchBot};
    if (msg == WM_LBUTTONDOWN) {
        m_bSearch = PtInRect(&search, pt) != 0;
        if (m_bSearch) {
            play_ui_sound(L"BtMouseClick");
            return;
        }
        // the title-bar buttons sink on press and act on release over them, like the game's own
        auto press = [&](int& pressed) {
            pressed = 1;
            SetCursorState(kCursorButtonPress);
            m_nCursor = kCursorButtonPress;
            play_ui_sound(L"BtMouseClick");
        };
        if (PtInRect(&kRcClose, pt)) {
            press(m_nClosePressed);
            return;
        }
        if (PtInRect(&kRcSort, pt)) {
            press(m_nSortPressed);
            return;
        }
        if (PtInRect(&kRcAuto, pt)) {
            press(m_nAutoPressed);
            return;
        }
        for (int k = 0; k < kKindCount; ++k) {
            RECT tab = {kTabLeft[k], kTabTop, kTabRight[k], kTabBot};
            if (PtInRect(&tab, pt)) {
                SwitchTab(k);
                return;
            }
        }
        if (!g_bItemDragging && !IsEngineDragActive() && MaxScroll() > 0 && rx >= kScrollX && rx < kScrollX + kScrollW
                && ry >= kScrollTop && ry < kScrollBot) {
            if (ry < kScrollTop + kArrowH) {
                --m_nScroll;
            } else if (ry >= kScrollBot - kArrowH) {
                ++m_nScroll;
            } else {
                RECT thumb = ThumbRect();
                if (PtInRect(&thumb, pt)) {
                    m_bScrollDrag = true;
                    m_nScrollGrabDY = ry - thumb.top;
                    SetCursorState(kCursorScrollV);
                    m_nCursor = kCursorScrollV;
                }
            }
            ClampScroll();
            return;
        }
        // one click picks an item up, the next puts it down (the engine's drag; OnDropped decides where it went)
        int slot = HitSlot(rx, ry);
        if (slot >= 0 && Bag().id[slot]) {
            BeginItemDrag(slot, rx, ry);
            return;
        }
    } else if (msg == WM_RBUTTONDOWN) {
        Withdraw(HitSlot(rx, ry)); // right-click takes the stack back
    } else if (msg == WM_LBUTTONUP) {
        m_bScrollDrag = false;
        if (m_nSortPressed) {
            m_nSortPressed = 0;
            if (PtInRect(&kRcSort, pt) && !g_bAwaiting) {
                SendTransfer(kReqOrganize, m_nKind, {});
            }
        }
        if (m_nAutoPressed) {
            m_nAutoPressed = 0;
            if (PtInRect(&kRcAuto, pt)) {
                SendSetAuto(m_nKind, !Bag().autoOn);
            }
        }
        if (m_nClosePressed) {
            m_nClosePressed = 0;
            if (PtInRect(&kRcClose, pt)) {
                Destroy(); // nothing may touch the window after this
                return;
            }
        }
        if (m_nCursor == kCursorButtonPress || m_nCursor == kCursorScrollV) {
            m_nCursor = kCursorArrow;
            SetCursorState(kCursorArrow);
        }
    }
    CWnd::OnMouseButton(msg, wParam, rx, ry);
}

int CBagWnd::OnMouseMove(int rx, int ry) {
    if (m_bScrollDrag) {
        int origin, thumbH, travel;
        ThumbGeom(origin, thumbH, travel);
        m_nScroll = travel > 0 ? ((ry - m_nScrollGrabDY - origin) * MaxScroll() + travel / 2) / travel : 0;
        ClampScroll();
        HideTip();
        return 1;
    }
    POINT pt{rx, ry};
    m_nCloseHover = PtInRect(&kRcClose, pt) ? 1 : 0;
    int sortHover = PtInRect(&kRcSort, pt) ? 1 : 0, autoHover = PtInRect(&kRcAuto, pt) ? 1 : 0;
    if ((sortHover && !m_nSortHover) || (autoHover && !m_nAutoHover)) {
        play_ui_sound(L"BtMouseOver");
    }
    m_nSortHover = sortHover;
    m_nAutoHover = autoHover;

    BagStore& b = Bag();
    int slot = HitSlot(rx, ry);
    if (slot >= 0 && b.id[slot]) {
        b.isNew[slot] = false; // hovering an item ends its glow
    }
    int key = slot >= 0 && b.id[slot] ? slot + 1 : 0;
    if (key != m_nTipKey) {
        GW_ItemSlotBase* item = key ? static_cast<GW_ItemSlotBase*>(b.item[slot]) : nullptr;
        if (item) {
            ShowTip(AbsX() + rx + 12, AbsY() + ry, item);
            m_nTipKey = key;
        } else {
            HideTip();
        }
    }
    if (!g_bItemDragging && !IsEngineDragActive()) {
        int want = m_nClosePressed || m_nSortPressed || m_nAutoPressed ? kCursorButtonPress
                : MaxScroll() > 0 && rx >= kScrollX && rx < kScrollX + kScrollW && ry >= kScrollTop && ry < kScrollBot
                        ? kCursorScrollHover
                : slot >= 0 ? kCursorItemHand : kCursorArrow;
        if (want != m_nCursor) {
            m_nCursor = want;
            SetCursorState(want);
        }
    }
    return 1;
}

int CBagWnd::OnMouseWheel(int rx, int ry, int nWheel) {
    if (!g_bItemDragging && !IsEngineDragActive()) {
        m_nScroll += nWheel > 0 ? -1 : 1;
        ClampScroll();
        HideTip();
    }
    return 1;
}

void CBagWnd::OnMouseEnter(int bEnter) {
    CWnd::OnMouseEnter(bEnter);
    if (!bEnter) {
        m_nCloseHover = m_nSortHover = m_nAutoHover = 0;
        HideTip();
        if (m_nCursor != kCursorArrow) {
            m_nCursor = kCursorArrow;
            SetCursorState(kCursorArrow);
        }
    }
}

void CBagWnd::OnDestroy() {
    EndDragIcon();
    if (m_bTell) {
        Send(kReqClose, m_nKind);
    }
    g_bSavedPos = true;
    g_nSavedX = AbsX();
    g_nSavedY = AbsY();
    g_nSavedKind = m_nKind;
    if (m_bTip) {
        SehClearTip(m_aTip);
        reinterpret_cast<void(__thiscall*)(void*)>(0x008E6BA3)(m_aTip); // CUIToolTip::~CUIToolTip
        m_bTip = false;
    }
    for (int k = 0; k < kKindCount; ++k) {
        FreeBag(k);
    }
    if (ms_pInstance == this) {
        ms_pInstance = nullptr;
    }
    CWnd::OnDestroy();
}

// Docked to the right of the item inventory (or to its left when the screen ends), else where it was last closed, else
// centred.
void OpenWindow() {
    int x = 0, y = 0;
    bool placed = false;
    if (void* inv = SehItemWnd()) {
        int w = SehWndWidth(inv), l, t;
        SehWndAbs(inv, l, t);
        if (w >= 80 && w <= 2000 && (l > 0 || t > 0)) {
            x = l + w + 3 + kWndW > get_screen_width() ? l - kWndW - 3 : l + w + 3;
            y = t;
            placed = true;
        }
    }
    if (!placed) {
        x = g_bSavedPos ? g_nSavedX : (get_screen_width() - kWndW) / 2;
        y = g_bSavedPos ? g_nSavedY : 110;
    }
    x = (std::max)(0, (std::min)(x, get_screen_width() - kWndW));
    y = (std::max)(0, (std::min)(y, get_screen_height() - kWndH));
    new CBagWnd(g_nSavedKind, x, y);
}

void ToggleWindow() {
    if (CBagWnd::ms_pInstance) {
        CBagWnd::ms_pInstance->Destroy();
    } else {
        OpenWindow();
    }
}

void CloseQuietly() {
    if (CBagWnd* w = CBagWnd::ms_pInstance) {
        w->m_bTell = false; // the connection is going: no CLOSE
        w->Destroy();
        CBagWnd::ms_pInstance = nullptr;
    }
}

// ---- the snapshot -----------------------------------------------------------------------------------------------

// A bag from the server: the window shows it if it is open on that bag (a push never opens the window). Read with
// the packet's own offset, which the item decoder advances.
void ReadSnapshot(CInPacket* pPacket) {
    unsigned char* base = reinterpret_cast<unsigned char*>(pPacket);
    unsigned char* data = *reinterpret_cast<unsigned char**>(base + 0x8);  // m_aRecvBuff
    unsigned short length = *reinterpret_cast<unsigned short*>(base + 0xC); // m_uLength
    size_t& offset = *reinterpret_cast<size_t*>(base + 0x14);              // m_uOffset
    auto u8 = [&]() -> int { return offset + 1 <= length ? data[offset++] : -1; };
    auto i16 = [&]() -> int {
        if (offset + 2 > length) {
            return -1;
        }
        short v = *reinterpret_cast<short*>(data + offset);
        offset += 2;
        return v;
    };
    if (u8() != kRespSnapshot) {
        return;
    }
    int kind = u8();
    if (kind < 0 || kind >= kKindCount) {
        return;
    }
    BagStore& b = g_bags[kind];
    bool glow = b.ready && !g_bAwaiting; // an Auto pickup the server pushed, not the answer to a request
    g_bAwaiting = false;
    static int s_oldId[kSlots], s_oldQty[kSlots];
    static bool s_oldNew[kSlots];
    for (int i = 0; i < kSlots; ++i) {
        s_oldId[i] = b.id[i];
        s_oldQty[i] = b.qty[i];
        s_oldNew[i] = b.isNew[i];
    }
    FreeBag(kind);
    int n = (std::max)(0, i16());
    static short s_slots[kSlots];
    int decoded = 0;
    for (int i = 0; i < n && i < kSlots; ++i) {
        int slot = i16();
        GW_ItemSlotBase* item = SehDecodeItem(pPacket);
        if (slot < 0 || offset > length) {
            ZRef<GW_ItemSlotBase> drop(item, false);
            break;
        }
        s_slots[decoded++] = static_cast<short>(slot);
        if (slot < kSlots) {
            b.item[slot] = ZRef<GW_ItemSlotBase>(item, false);
            b.id[slot] = SehItemId(item);
            b.count = (std::max)(b.count, slot + 1);
        } else {
            ZRef<GW_ItemSlotBase> drop(item, false);
        }
    }
    for (int i = 0; i < decoded; ++i) {
        int q = i16();
        if (s_slots[i] < kSlots) {
            b.qty[s_slots[i]] = (std::max)(0, q);
        }
    }
    int autoOn = u8();
    b.autoOn = autoOn > 0;
    b.ready = true;
    for (int s = 0; s < kSlots; ++s) {
        if (!b.id[s]) {
            b.isNew[s] = false;
        } else if (s_oldId[s] == b.id[s]) {
            b.isNew[s] = s_oldNew[s] || (glow && b.qty[s] > s_oldQty[s]);
        } else {
            b.isNew[s] = glow;
        }
    }
    if (CBagWnd* w = CBagWnd::ms_pInstance) {
        if (w->m_nKind == kind) {
            w->Refilter();
            w->InvalidateRect(nullptr);
        }
    }
}

void SehReadSnapshot(CInPacket* pPacket) {
    __try {
        ReadSnapshot(pPacket);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_bAwaiting = false;
    }
}

void OnSnapshot(CInPacket* pPacket) {
    if (features::IsActive(FeatureId::storage_bag)) {
        SehReadSnapshot(pPacket);
    }
}

// ---- the BAG button on the item inventory ----------------------------------------------------------------------

constexpr int kBagBtnW = 25, kBagBtnH = 12, kBagBtnRightMargin = 78, kBagBtnY = 6; // left of the inventory's expand button
IWzCanvasPtr g_pBagBtn[3]; // normal, pressed, mouseOver
bool g_bBagBtnLoaded = false;
bool g_bBagBtnPressed = false, g_bBagBtnHover = false;

// The button's screen rectangle, read from the live inventory (right after the inventory moved too).
bool BagButtonRect(RECT& out) {
    void* inv = SehItemWnd();
    if (!inv) {
        return false;
    }
    int w = SehWndWidth(inv), l, t;
    if (w < 80 || w > 2000) {
        return false;
    }
    SehWndAbs(inv, l, t);
    int x = l + w - kBagBtnRightMargin;
    out = {x, t + kBagBtnY, x + kBagBtnW, t + kBagBtnY + kBagBtnH};
    return true;
}

auto CUIItem__Draw = reinterpret_cast<void(__thiscall*)(void*, const RECT*)>(kItemWndDraw);
void __fastcall CUIItem__Draw_hook(void* pThis, void* /*edx*/, const RECT* pRect) {
    CUIItem__Draw(pThis, pRect);
    if (!Enabled()) {
        return;
    }
    if (!g_bBagBtnLoaded) {
        g_bBagBtnLoaded = true;
        g_pBagBtn[0] = CBagWnd::Sprite(L"Custom/UI/UIWindow.img/StorageBag/BtBag/normal/0");
        g_pBagBtn[1] = CBagWnd::Sprite(L"Custom/UI/UIWindow.img/StorageBag/BtBag/pressed/0");
        g_pBagBtn[2] = CBagWnd::Sprite(L"Custom/UI/UIWindow.img/StorageBag/BtBag/mouseOver/0");
    }
    if (!g_pBagBtn[0]) {
        return; // no art: a Custom.wz without the bag
    }
    if (g_bBagBtnPressed && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        g_bBagBtnPressed = false; // a release the window never saw
    }
    if (g_bBagBtnPressed) {
        SetCursorState(kCursorButtonPress);
    } else if (g_bItemDragging) {
        SetCursorState(kCursorItemGrab);
    } else if (g_bBagBtnHover) {
        SetCursorState(kCursorAskClick);
    }
    try {
        IWzCanvasPtr c = reinterpret_cast<CWnd*>(pThis)->GetCanvas();
        int w = SehWndWidth(pThis);
        if (!c || w < 80) {
            return;
        }
        IWzCanvasPtr& art = g_bBagBtnPressed && g_pBagBtn[1] ? g_pBagBtn[1]
                : g_bBagBtnHover && g_pBagBtn[2] ? g_pBagBtn[2] : g_pBagBtn[0];
        c->CopyEx(w - kBagBtnRightMargin, kBagBtnY, art, CANVAS_ALPHATYPE::CA_OVERWRITE, 0, 0, 0, 0, 0, 0);
    } catch (...) {
    }
}

// ---- messages: the BAG button (in the inventory's title bar, whose clicks never reach CUIItem) -------------------

bool OnMessage(UINT uMsg, WPARAM /*wParam*/, LPARAM /*lParam*/, LRESULT* plResult) {
    if (!Enabled()) {
        return false;
    }
    if (uMsg == WM_MOUSEMOVE) {
        RECT rc;
        POINT sp;
        bool over = BagButtonRect(rc) && GetAbsCursor(sp) && PtInRect(&rc, sp);
        if (over != g_bBagBtnHover) {
            g_bBagBtnHover = over;
            if (void* inv = SehItemWnd()) {
                SehInvalidate(inv);
            }
        }
    }
    if (uMsg == WM_LBUTTONDOWN && !g_bItemDragging) {
        RECT rc;
        POINT sp;
        if (BagButtonRect(rc) && GetAbsCursor(sp) && PtInRect(&rc, sp)) {
            g_bBagBtnPressed = true;
            if (void* inv = SehItemWnd()) {
                SehInvalidate(inv);
            }
            SetCursorState(kCursorButtonPress);
            play_ui_sound(L"BtMouseClick");
            *plResult = 0;
            return true; // the inventory must not start moving under the button
        }
    } else if (uMsg == WM_LBUTTONUP && g_bBagBtnPressed) {
        g_bBagBtnPressed = false;
        if (void* inv = SehItemWnd()) {
            SehInvalidate(inv);
        }
        RECT rc;
        POINT sp;
        if (BagButtonRect(rc) && GetAbsCursor(sp) && PtInRect(&rc, sp)) {
            ToggleWindow();
        }
        *plResult = 0;
        return true;
    }
    return false;
}

// ---- hooks ------------------------------------------------------------------------------------------------------

auto CField__OnKey = reinterpret_cast<void(__thiscall*)(void*, unsigned int, int)>(kFieldOnKey);
void __fastcall CField__OnKey_hook(void* pThis, void* /*edx*/, unsigned int wParam, int lParam) {
    CBagWnd* w = CBagWnd::ms_pInstance;
    if (w && w->m_bSearch && !(lParam & 0x80000000) && w->OnSearchKey(wParam)) {
        return; // typed into the search box, so no skill fires
    }
    CField__OnKey(pThis, wParam, lParam);
}

bool SehReadDraggable(void* pDraggable, void*& source, int& type, int& pos) {
    __try {
        source = *reinterpret_cast<void**>(static_cast<char*>(pDraggable) + 0x24);
        type = *reinterpret_cast<int*>(static_cast<char*>(pDraggable) + 0x18);
        pos = *reinterpret_cast<int*>(static_cast<char*>(pDraggable) + 0x1C);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A drag ended: an inventory item over the bag is deposited; a bag item over the bag moves, anywhere else comes out.
bool OnItemDropped(void* pDraggable, void* /*pFrom*/, void* pTo, int /*rx*/, int /*ry*/, int* pnResult) {
    g_bItemDragging = false;
    CBagWnd* w = CBagWnd::ms_pInstance;
    void* source = nullptr;
    int type = 0, pos = 0;
    if (!pDraggable || !w || !SehReadDraggable(pDraggable, source, type, pos)) {
        return false;
    }
    void* ours = static_cast<IUIMsgHandler*>(w);
    bool ontoBag = pTo == ours;
    if (source == ours) { // a bag item: moved within the bag, or taken out
        if (ontoBag) {
            int dst = w->DropSlot();
            if (dst >= 0 && dst != pos && !g_bAwaiting) {
                SendTransfer(kReqMove, w->m_nKind, {pos, dst});
            }
        } else {
            w->Withdraw(pos);
        }
        EndDragIcon();
        *pnResult = 1;
        return true;
    }
    if (ontoBag && !g_bAwaiting) { // an inventory item (type, slot) put into the shown bag
        SendTransfer(kReqDeposit, w->m_nKind, {type, pos, w->DropSlot()});
        play_ui_sound(L"DragEnd");
        *pnResult = 1;
        return true;
    }
    return false;
}

}  // namespace


void AttachStorageBagMod() {
    if (!v83::CheckClientVersion()) {
        LogMessage("[storage_bag] off: MapleStory.exe is not the build these offsets come from");
        return;
    }
    serverpackets::OnRecvPacket(kRecvOpcode, OnSnapshot);
    clientloop::OnMessage(OnMessage);
    clientloop::OnItemDropped(OnItemDropped);
    clientloop::OnLeaveGame(CloseQuietly);
    clientloop::OnClearFieldUI(CloseQuietly);
    ATTACH_HOOK(CUIItem__Draw, CUIItem__Draw_hook);
    ATTACH_HOOK(CField__OnKey, CField__OnKey_hook);
}
