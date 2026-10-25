#include "pch.h"
#include "hook.h"
#include "clientloop.h"
#include "debug.h"
#include "features.h"
#include "serverpackets.h"
#include "setitemproto.h"
#include "setitems.h"
#include "wvs/iteminfo.h"
#include "wvs/tooltip.h"
#include "wvs/util.h"
#include "ztl/ztl.h"

#include <algorithm>
#include <string>
#include <vector>

// The module set_items: a set item's equip tooltip gets two rows (SET EFFECT, PIECES EQUIPPED), and a second
// tooltip beside it lists the set's members (worn ones green) and each tier's bonus (reached ones green). The sets
// come from the BotStory server's SET_ITEM_SYNC (0x186, server mod set_items): its own scripts/config/set-items.yml,
// filtered to the character's job, sent whole after every equip change. Display only: the bonus is the server's.
//
// tool_tip owns the two equip tooltip hooks and calls OnEquipBasic / OnDrawEquip (setitems.h), so this module hooks
// only CUIToolTip::ClearToolTip, to hide its tooltip when the equip tooltip goes.
//
// Ported 2026-10-25 from the community release "set equip V3" (no licence or author line), client part only (its
// tooltip_set_item_snippet.cpp). Changes here: the sets come from a server push instead of a 5-second polling thread
// that sent packets off the game thread; the fonts are made through IWzFont::Create instead of a client address; the
// second tooltip moves to the left of the equip tooltip when it would leave the screen; PIECES EQUIPPED counts against
// the set's slot total, not its member list (which also holds alternate and Reverse items); a font that cannot be made
// leaves the plain native tooltip text. Its Ability window patch (setstatbonus.cpp) is not ported: it changes the
// stats the client computes damage from.

namespace {

using setitemproto::Set;

std::vector<Set> g_sets; // the last SET_ITEM_SYNC
bool g_bHaveSets = false;

constexpr uintptr_t kCInputSystem = 0x00BEC33C; // CInputSystem*; its HWND is at +0
constexpr uintptr_t kGetItemName = 0x005CF63E;  // CItemInfo::GetItemName(ZXString*, nItemID), as tooltip.cpp

CUIToolTip* g_pMainTip = nullptr; // the equip tooltip instance, captured by OnEquipBasic
int g_nPendingItem = 0;           // the set item OnEquipBasic last added rows for

// ---- the set tooltip ----------------------------------------------------------------------------------------

alignas(8) unsigned char g_aTipBuf[0x600]; // a second native CUIToolTip, as worldmapinfo.cpp makes its own
bool g_bTipMade = false;
int g_nShownItem = 0;
DWORD g_dwShownTick = 0;
constexpr DWORD kOrphanMs = 20000; // only if ClearToolTip never comes (the window lost focus mid-hover)

// What the last build drew, so a frame that changes nothing redraws nothing.
int g_nBuiltItem = 0;
int g_nBuiltX = INT_MIN;
int g_nBuiltY = INT_MIN;
int g_nBuiltPieces = -1;
unsigned g_uBuiltSync = 0;
unsigned g_uSync = 0; // bumped by every SET_ITEM_SYNC

// The cursor, sampled once per hovered item: GetCursorPos jitters a pixel even on a still hand.
int g_nAnchorItem = 0;
POINT g_ptAnchor = {};

CUIToolTip* SetTip() {
    if (!g_bTipMade) {
        reinterpret_cast<void(__thiscall*)(void*)>(0x008E49B5)(g_aTipBuf); // CUIToolTip::CUIToolTip
        g_bTipMade = true;
    }
    return reinterpret_cast<CUIToolTip*>(g_aTipBuf);
}

void HideSetTip() {
    if (g_nShownItem == 0) {
        return;
    }
    SetTip()->ClearToolTip();
    g_nShownItem = 0;
    g_nAnchorItem = 0;
    g_nBuiltItem = 0;
}

// ---- styled drawing ---------------------------------------------------------------------------------------------

enum class Ink { title, worn, missing, tier, reached, unreached };

constexpr unsigned long kInkColor[] = {
    0xFFFFD45A, // title
    0xFF6EDB7E, // worn
    0xFF8A8A8A, // missing
    0xFFC79CFF, // tier
    0xFF6EDB7E, // reached
    0xFF8A8A8A, // unreached
};
constexpr bool kInkBold[] = {true, false, false, true, false, false};
constexpr unsigned long kShadowColor = 0xFF000000;
constexpr unsigned long kFillColor = 0xFF000000;
constexpr unsigned long kTitleFillColor = 0xFF1E1E1E;
constexpr unsigned long kRuleColor = 0xFF6B4FA0;
constexpr unsigned long kBorderColor = 0xFFB8A0E0;
constexpr int kMinWidth = 220;
constexpr int kMaxWidth = 320;
constexpr int kWidthPad = 26;
constexpr int kLeft = 12;
constexpr int kContentTop = 34;
constexpr int kLineH = 15;
constexpr int kRuleH = 10;
constexpr int kTitleBarH = 20;
constexpr int kTitleTop = 9;
constexpr int kGap = 6;

struct Line {
    std::string text;
    Ink ink = Ink::missing;
    bool rule = false;
};

IWzFontPtr g_apInk[6];
IWzFontPtr g_pShadow;
bool g_bFontsTried = false;
bool g_bFontsOk = false;

IWzFontPtr MakeFont(unsigned long color, bool bold) {
    IWzFontPtr p;
    try {
        PcCreateObject<IWzFontPtr>(L"Canvas#Font", p, nullptr);
        if (p) {
            Ztl_variant_t style(bold ? L"B" : L"");
            if (FAILED(p->raw_Create(Ztl_bstr_t(L"Dotum"), 12, color, style))) {
                p = nullptr;
            }
        }
    } catch (...) {
        p = nullptr;
    }
    return p;
}

bool Fonts() {
    if (!g_bFontsTried) {
        g_bFontsTried = true;
        g_bFontsOk = true;
        for (int i = 0; i < 6; ++i) {
            g_apInk[i] = MakeFont(kInkColor[i], kInkBold[i]);
            g_bFontsOk = g_bFontsOk && g_apInk[i];
        }
        g_pShadow = MakeFont(kShadowColor, false);
        g_bFontsOk = g_bFontsOk && g_pShadow;
        if (!g_bFontsOk) {
            LogMessage("set_items: could not make the tooltip fonts; the set tooltip shows plain text");
        }
    }
    return g_bFontsOk;
}

int TextW(Ink ink, const std::string& s) {
    if (s.empty() || !g_apInk[static_cast<int>(ink)]) {
        return 0;
    }
    try {
        return static_cast<int>(g_apInk[static_cast<int>(ink)]->CalcTextWidth(Ztl_bstr_t(s.c_str()), Ztl_variant_t()));
    } catch (...) {
        return static_cast<int>(s.size()) * 7;
    }
}

void Text(IWzCanvasPtr& c, Ink ink, int x, int y, const std::string& s) {
    IWzFontPtr& f = g_apInk[static_cast<int>(ink)];
    if (s.empty() || !f) {
        return;
    }
    try {
        c->DrawTextA(x + 1, y + 1, Ztl_bstr_t(s.c_str()), g_pShadow, Ztl_variant_t(), Ztl_variant_t());
        c->DrawTextA(x, y, Ztl_bstr_t(s.c_str()), f, Ztl_variant_t(), Ztl_variant_t());
    } catch (...) {
    }
}

std::string ItemName(int nItemID) {
    ZXString<char> s;
    try {
        reinterpret_cast<ZXString<char>*(__thiscall*)(CItemInfo*, ZXString<char>*, int)>(kGetItemName)(
                CItemInfo::GetInstance(), &s, nItemID);
    } catch (...) {
    }
    const char* p = s;
    if (!p || !*p) {
        return std::to_string(nItemID);
    }
    return p;
}

std::string Title(const Set& set) {
    return set.name + " (" + std::to_string(set.pieces) + "/" + std::to_string(set.total) + ")";
}

// Members (worn green), a rule, then per tier its header and stats (reached green).
std::vector<Line> Lines(const Set& set) {
    std::vector<Line> lines;
    for (const setitemproto::Member& m : set.members) {
        lines.push_back({ItemName(m.itemId), m.worn ? Ink::worn : Ink::missing});
    }
    lines.push_back({std::string(), Ink::missing, true});
    for (const setitemproto::Tier& tier : set.tiers) {
        bool reached = set.pieces >= tier.pieces;
        lines.push_back({std::to_string(tier.pieces) + " Set Items", Ink::tier});
        for (const setitemproto::StatValue& sv : tier.stats) {
            const char* label = setitemproto::StatLabel(sv.stat);
            if (label) {
                lines.push_back({std::string(label) + " +" + std::to_string(sv.value),
                                 reached ? Ink::reached : Ink::unreached});
            }
        }
    }
    return lines;
}

int ContentHeight(const std::vector<Line>& lines) {
    int h = 0;
    for (const Line& l : lines) {
        h += l.rule ? kRuleH : kLineH;
    }
    return h;
}

void Draw(CUIToolTip* tip, const std::string& title, const std::vector<Line>& lines) {
    if (!tip->m_pLayer) {
        return;
    }
    try {
        Ztl_variant_t vIdx;
        V_VT(&vIdx) = VT_I4;
        V_I4(&vIdx) = 0;
        IWzCanvasPtr c = tip->m_pLayer->Getcanvas(vIdx);
        int w = tip->m_nWidth;
        int h = tip->m_nHeight;
        if (!c || w <= 0 || h <= 0) {
            return;
        }
        c->DrawRectangle(0, 0, w, h, kFillColor);
        c->DrawRectangle(2, 2, (std::max)(0, w - 4), kTitleBarH, kTitleFillColor);
        c->DrawRectangle(6, kTitleBarH + 4, (std::max)(0, w - 12), 1, kRuleColor);
        c->DrawRectangle(0, 0, w, 1, kBorderColor);
        c->DrawRectangle(0, (std::max)(0, h - 1), w, 1, kBorderColor);
        c->DrawRectangle(0, 0, 1, h, kBorderColor);
        c->DrawRectangle((std::max)(0, w - 1), 0, 1, h, kBorderColor);
        Text(c, Ink::title, (std::max)(kLeft, (w - TextW(Ink::title, title)) / 2), kTitleTop, title);
        int y = kContentTop;
        for (const Line& l : lines) {
            if (l.rule) {
                c->DrawRectangle(kLeft, y + kRuleH / 2, (std::max)(0, w - kLeft * 2), 1, kRuleColor);
                y += kRuleH;
                continue;
            }
            Text(c, l.ink, kLeft, y, l.text);
            y += kLineH;
        }
    } catch (...) {
    }
}

void ShowSetTip(const Set& set, int nItemID, int x, int y) {
    if (nItemID == g_nBuiltItem && x == g_nBuiltX && y == g_nBuiltY && set.pieces == g_nBuiltPieces
            && g_uSync == g_uBuiltSync) {
        g_nShownItem = nItemID;
        g_dwShownTick = GetTickCount();
        return;
    }
    const int nAskedX = x; // what the next frame asks for, flipped or not
    std::vector<Line> lines = Lines(set);
    std::string title = Title(set);
    bool styled = Fonts();
    CUIToolTip* tip = SetTip();

    if (styled) {
        int width = TextW(Ink::title, title);
        for (const Line& l : lines) {
            width = (std::max)(width, TextW(l.ink, l.text));
        }
        width = (std::clamp)(width + kWidthPad, kMinWidth, kMaxWidth);
        int nScreenW = get_screen_width();
        if (x + width > nScreenW && g_ptAnchor.x - width - kGap >= 0) {
            x = g_ptAnchor.x - width - kGap; // no room on the right: the other side of the equip tooltip
        }
        // SetToolTip_String2 only sizes the box: one blank line per line, padded until its height covers ours.
        std::string blank;
        for (size_t i = 0; i < lines.size(); ++i) {
            blank += i ? (lines[i].rule ? "\n" : "\n ") : " ";
        }
        int target = kContentTop + ContentHeight(lines) + 6;
        for (int extra = 0; extra <= 24; ++extra) {
            tip->ClearToolTip();
            tip->SetToolTip_String2(x, y, ZXString<char>(" "), ZXString<char>(blank.c_str()), 0, 0, 0, width, 1, 0);
            if (tip->m_nHeight >= target) {
                break;
            }
            blank += "\n ";
        }
        Draw(tip, title, lines);
    } else {
        std::string desc;
        for (const Line& l : lines) {
            desc += (desc.empty() ? "" : "\n") + l.text;
        }
        tip->ClearToolTip();
        tip->SetToolTip_String2(x, y, ZXString<char>(title.c_str()), ZXString<char>(desc.c_str()), 0, 0, 0, kMinWidth,
                                1, 0);
    }
    g_nBuiltItem = nItemID;
    g_nBuiltX = nAskedX;
    g_nBuiltY = y;
    g_nBuiltPieces = set.pieces;
    g_uBuiltSync = g_uSync;
    g_nShownItem = nItemID;
    g_dwShownTick = GetTickCount();
}

bool Live() {
    return features::IsActive(FeatureId::set_items) && g_bHaveSets;
}

// ---- hooks and packets ------------------------------------------------------------------------------------------

auto CUIToolTip__ClearToolTip = reinterpret_cast<void(__thiscall*)(CUIToolTip*)>(0x008E6E23);

void __fastcall CUIToolTip__ClearToolTip_hook(CUIToolTip* pThis, void* /*edx*/) {
    CUIToolTip__ClearToolTip(pThis);
    if (pThis == g_pMainTip) {
        HideSetTip();
    }
}

void OnSync(const unsigned char* pBody, size_t uLength) {
    if (!setitemproto::ReadSync(pBody, uLength, g_sets)) {
        LOG_ONCE("set_items: a SET_ITEM_SYNC did not decode (%u bytes); the sets stay as they were",
                 static_cast<unsigned>(uLength));
        return;
    }
    g_bHaveSets = true;
    ++g_uSync;
}

void OnFrame() {
    if (g_nShownItem == 0) {
        return;
    }
    if (!features::IsActive(FeatureId::set_items) || GetTickCount() - g_dwShownTick > kOrphanMs) {
        HideSetTip();
    }
}

}  // namespace

namespace setitems {

void OnEquipBasic(CUIToolTip* pTip, int nItemID) {
    g_pMainTip = pTip;
    g_nPendingItem = 0;
    if (!Live()) {
        return;
    }
    const Set* set = setitemproto::FindSet(g_sets, nItemID);
    if (!set) {
        return;
    }
    pTip->AddInfoEx(14, 15, "SET EFFECT :", ZXString<char>(set->name.c_str()), 1, 1001);
    ZXString<char> sPieces;
    sPieces.Format("%d / %d", set->pieces, set->total);
    pTip->AddInfoEx(14, 15, "PIECES EQUIPPED :", sPieces, 1, 1001);
    g_nPendingItem = nItemID;
}

void OnDrawEquip(CUIToolTip* pTip, int nItemID) {
    if (!Live() || !pTip || !pTip->m_pLayer || nItemID == 0 || nItemID != g_nPendingItem) {
        return;
    }
    const Set* set = setitemproto::FindSet(g_sets, nItemID);
    if (!set) {
        return;
    }
    if (nItemID != g_nAnchorItem) {
        POINT pt = {};
        uintptr_t pInput = *reinterpret_cast<uintptr_t*>(kCInputSystem);
        HWND hWnd = pInput ? *reinterpret_cast<HWND*>(pInput) : nullptr;
        if (!hWnd || !::GetCursorPos(&pt) || !::ScreenToClient(hWnd, &pt)) {
            return;
        }
        g_nAnchorItem = nItemID;
        g_ptAnchor = pt;
    }
    // The equip tooltip's top left sits at the cursor (its layer's own x/y does not say where it was docked).
    ShowSetTip(*set, nItemID, g_ptAnchor.x + pTip->m_nWidth + kGap, g_ptAnchor.y);
}

}  // namespace setitems

void AttachSetItemsMod() {
    ATTACH_HOOK(CUIToolTip__ClearToolTip, CUIToolTip__ClearToolTip_hook);
    serverpackets::OnRecv(0x186, &OnSync);
    clientloop::OnFrame(&OnFrame);
}
