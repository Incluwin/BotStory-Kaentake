#include "pch.h"
#include "hook.h"
#include "constants.h"
#include "v83_offsets.h"
#include "v83_version_guard.h"
#include "wvs/wnd.h"
#include "wvs/packet.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include "crewproto.h"
#include <cstdarg>
#include <cstdio>

// The native Crew window (BotStory server docs/BOT_MANAGEMENT_P10_PLAN.md): the player's companions out with them and
// on the bench, with the everyday rotation (call, bench, swap, card) on the mouse. The server owns every rule; this
// window only shows its snapshot and sends the click back.
//
// Protocol (server terms): the client sends Recv opcode 0x117, the server sends Send opcode 0x17A. The server sends
// 0x17A only to a connection that sent HELLO, and the window opens only after its WELCOME said enabled, so an old
// server or a switched-off one leaves the client exactly as before.
//
// Hooks (addresses from an unpacked dump of the shipped v83 build, see v83_offsets.h):
//   CClientSocket::ProcessPacket  0x17A is parsed here and never reaches the game; HELLO follows the first SET_FIELD
//                                 of each connection.
//   CWndMan::ProcessKey           the hotkey, only while no edit box has the focus (so never while chatting) and only
//                                 when the key is unbound in the player's key map.
//   CWvsContext::OnLeaveGame      the window closes; WELCOME is forgotten.
//   CWvsContext::ClearFieldUI     the window closes (Cash Shop, MTS).
// config.ini [config]: crew_window=0 skips every hook; crew_hotkey=<letter or digit> (default U).

using namespace v83::crew;
using namespace crewproto;

namespace {

void crew_log(const char* fmt, ...) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, "kaentake_diag.txt", "a") != 0 || !fp) {
        return;
    }
    fputs("[crew] ", fp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

auto get_basic_font = reinterpret_cast<IWzFontPtr*(__cdecl*)(IWzFontPtr*, int)>(kGetBasicFont);
auto play_ui_sound = reinterpret_cast<void(__cdecl*)(const wchar_t*)>(kPlayUiSound);

// ---- protocol ---------------------------------------------------------------------------------------------------

bool g_bHooksOn = false;
bool g_bHelloSent = false;  // on this connection
bool g_bEnabled = false;    // the last WELCOME said this character may use the window
Snapshot g_snap;
UINT g_uHotkeyVk = 'U';
bool g_bSelfTest = false;   // config.ini crew_selftest=1: the hotkey opens a sample crew without a server (drawing checks)

void EncodeStr(COutPacket& o, const std::string& s) {
    o.Encode2(static_cast<unsigned short>(s.size()));
    if (!s.empty()) o.EncodeBuffer(s.data(), s.size());
}

void SendOut(COutPacket& o) {
    void* pSocket = *reinterpret_cast<void**>(kClientSocketInstance);
    if (pSocket) {
        reinterpret_cast<void(__thiscall*)(void*, COutPacket*)>(kSendPacket)(pSocket, &o);
    }
}

void SendHello() {
    COutPacket o(kSendOpcode);
    o.Encode1(OP_HELLO);
    o.Encode2(kProtocol);
    o.Encode4(kDllBuild);
    SendOut(o);
    crew_log("HELLO sent (protocol %d, build %d)", kProtocol, kDllBuild);
}

void SendSimple(unsigned char op) {
    COutPacket o(kSendOpcode);
    o.Encode1(op);
    SendOut(o);
}

void SendAction(Verb verb, const Row* a, const Row* b) {
    COutPacket o(kSendOpcode);
    o.Encode1(OP_ACTION);
    o.Encode1(verb);
    o.Encode4(static_cast<unsigned int>(g_snap.ownerVersion));
    EncodeStr(o, a ? a->id : std::string());
    o.Encode4(static_cast<unsigned int>(a ? a->version : 0));
    EncodeStr(o, b ? b->id : std::string());
    o.Encode4(static_cast<unsigned int>(b ? b->version : 0));
    SendOut(o);
}

// ---- the window -------------------------------------------------------------------------------------------------

// Layout on the Crew background (Custom.wz UI/UIWindow.img/CrewWnd/backgrnd, 312 x 430: the UserList background with
// 41 more panel rows and the title MAPLE CREW): title bar 0-25 (drag), white panel 47 to height-58, button bar below.
// Without Custom.wz art it falls back to the 312 x 389 UserList background and draws the title itself.
constexpr int kWndW = 312;
constexpr int kWndHDefault = 430;
constexpr int kTitleH = 25;
constexpr int kLeft = 10;
constexpr int kRight = 302;
constexpr int kPartyTop = 50;
constexpr int kRowH = 28;
constexpr int kBtnW = 46;
constexpr int kBtnH = 18;
constexpr int kCloseX = 291;
constexpr int kCloseY = 7;
int g_nWndH = kWndHDefault;  // the background's height: the panel and the button bar follow it
inline int PanelBottom() { return g_nWndH - 57; }
inline int BtnRow1() { return g_nWndH - 47; }
inline int BtnRow2() { return g_nWndH - 25; }

enum Button { BT_NONE = 0, BT_CALL, BT_BENCH, BT_SWAP, BT_CARD, BT_USUAL, BT_MORE,
              BT_FOLLOW, BT_WAIT, BT_GATHER, BT_REBUFF, BT_CLOSE, BT_COUNT };

const wchar_t* const kButtonLabel[BT_COUNT] = {
    L"", L"Call", L"Bench", L"Swap", L"Card", L"Usual", L"More",
    L"Follow", L"Wait", L"Gather", L"Rebuff", L"",
};

int g_nSavedLeft = INT_MIN;
int g_nSavedTop = INT_MIN;

class CCrewWnd : public CWnd {
public:
    inline static CCrewWnd* ms_pInstance = nullptr;
    // Our own RTTI node under the client's real CWnd node, so the game treats this window like any CWnd.
    inline static CRTTI ms_RTTI{reinterpret_cast<const CRTTI*>(kRttiCWnd)};

    IWzCanvasPtr m_pBg;
    IWzCanvasPtr m_pClose[3];  // normal, mouseOver, pressed
    IWzFontPtr m_pFontBold;
    IWzFontPtr m_pFont;
    IWzFontPtr m_pFontGrey;
    IWzFontPtr m_pFontLight;

    std::string m_selParty;     // persona ids, "" = none
    std::string m_selBench;
    std::string m_lastPicked;   // the row clicked last (Card acts on it)
    int m_nBenchScroll = 0;
    int m_nHoverBtn = BT_NONE;
    int m_nPressedBtn = BT_NONE;
    std::string m_hoverRow;
    bool m_bClosing = false;
    bool m_bOwnTitle = false;   // no Custom.wz art: the stock background's title is covered and "Crew" drawn

    // a press on a row that may become a drag
    std::string m_dragId;
    bool m_dragFromParty = false;
    bool m_bDragging = false;
    int m_nDownX = 0, m_nDownY = 0, m_nMouseX = 0, m_nMouseY = 0;
    DWORD m_dwLastClick = 0;
    std::string m_lastClickId;

    struct RowHit {
        const Row* row = nullptr;
        bool party = false;
    };

    CCrewWnd() {
        ms_pInstance = this;
        LoadArt(); // before CreateWnd: the background decides the height
        int x = g_nSavedLeft != INT_MIN ? g_nSavedLeft : (get_screen_width() - kWndW) / 2;
        int y = g_nSavedTop != INT_MIN ? g_nSavedTop : (get_screen_height() - g_nWndH) / 2;
        x = (std::max)(0, (std::min)(x, get_screen_width() - kWndW));
        y = (std::max)(0, (std::min)(y, get_screen_height() - g_nWndH));
        CWnd::CreateWnd(this, x, y, kWndW, g_nWndH, 10, 1, nullptr, 0);
        play_ui_sound(L"MenuUp");
        InvalidateRect(nullptr);
    }
    virtual ~CCrewWnd() override {
        if (ms_pInstance == this) ms_pInstance = nullptr;
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
    void LoadArt() {
        m_pBg = Sprite(L"Custom/UI/UIWindow.img/CrewWnd/backgrnd");
        m_bOwnTitle = !m_pBg;
        if (!m_pBg) m_pBg = Sprite(L"UI/UIWindow.img/UserList/backgrnd");
        g_nWndH = 389;
        if (m_pBg) {
            try {
                UINT h = 0;
                m_pBg->get_height(&h);
                if (h >= 300 && h <= 700) g_nWndH = static_cast<int>(h);
            } catch (...) {
            }
        }
        m_pClose[0] = Sprite(L"UI/Basic.img/BtClose/normal/0");
        m_pClose[1] = Sprite(L"UI/Basic.img/BtClose/mouseOver/0");
        m_pClose[2] = Sprite(L"UI/Basic.img/BtClose/pressed/0");
        m_pFontBold = Font(kFontBlackBold);
        m_pFont = Font(kFontBlack);
        m_pFontGrey = Font(kFontDarkGrey);
        m_pFontLight = Font(kFontLightGrey);
    }

    // ---- geometry ----

    int BenchHeaderY() const {
        int rows = (std::max)(1, static_cast<int>(g_snap.party.size()));
        return kPartyTop + 16 + rows * kRowH + 6;
    }
    int BenchTop() const { return BenchHeaderY() + 16; }
    int BenchVisible() const { return (std::max)(1, (PanelBottom() - BenchTop()) / kRowH); }
    void ClampScroll() {
        int maxScroll = (std::max)(0, static_cast<int>(g_snap.bench.size()) - BenchVisible());
        m_nBenchScroll = (std::max)(0, (std::min)(m_nBenchScroll, maxScroll));
    }

    static RECT ButtonRect(int bt) {
        if (bt == BT_CLOSE) return {kCloseX, kCloseY, kCloseX + 12, kCloseY + 12};
        int index = bt <= BT_MORE ? bt - BT_CALL : bt - BT_FOLLOW;
        int count = bt <= BT_MORE ? 6 : 4;
        int gap = 3;
        int total = count * kBtnW + (count - 1) * gap;
        int x = (kWndW - total) / 2 + index * (kBtnW + gap);
        int y = bt <= BT_MORE ? BtnRow1() : BtnRow2();
        return {x, y, x + kBtnW, y + kBtnH};
    }

    const Row* FindRow(const std::string& id, bool* pParty = nullptr) const {
        if (id.empty()) return nullptr;
        for (auto& r : g_snap.party) {
            if (r.id == id) { if (pParty) *pParty = true; return &r; }
        }
        for (auto& r : g_snap.bench) {
            if (r.id == id) { if (pParty) *pParty = false; return &r; }
        }
        return nullptr;
    }

    RowHit HitRow(int x, int y) const {
        RowHit hit;
        if (x < kLeft - 4 || x > kRight + 4) return hit;
        int top = kPartyTop + 16;
        for (size_t i = 0; i < g_snap.party.size(); ++i) {
            int ry = top + static_cast<int>(i) * kRowH;
            if (y >= ry && y < ry + kRowH) { hit.row = &g_snap.party[i]; hit.party = true; return hit; }
        }
        int btop = BenchTop();
        int visible = BenchVisible();
        for (int i = 0; i < visible; ++i) {
            size_t index = static_cast<size_t>(m_nBenchScroll + i);
            if (index >= g_snap.bench.size()) break;
            int ry = btop + i * kRowH;
            if (y >= ry && y < ry + kRowH) { hit.row = &g_snap.bench[index]; hit.party = false; return hit; }
        }
        return hit;
    }
    bool InPartyBlock(int y) const { return y >= kPartyTop && y < BenchHeaderY(); }
    bool InBenchBlock(int y) const { return y >= BenchHeaderY() && y < PanelBottom(); }

    // ---- what each button would do now ----

    // as the server: the one out may let someone take its place, or the one at camp may swap in for it
    static bool CanSwap(const Row* out, const Row* in) {
        return out && in && ((out->flags & ROW_SWAP_OUT) || (in->flags & ROW_SWAP_IN));
    }

    const Row* SelParty() const { return FindRow(m_selParty); }
    const Row* SelBench() const { return FindRow(m_selBench); }

    bool Enabled(int bt) const {
        if (!g_snap.valid) return bt == BT_MORE || bt == BT_CLOSE;
        const Row* p = SelParty();
        const Row* b = SelBench();
        switch (bt) {
        case BT_CALL: return (b && (b->flags & ROW_CALL)) || (!b && p && (p->flags & ROW_CALL));
        case BT_BENCH: return p && (p->flags & ROW_BENCH);
        case BT_SWAP: return CanSwap(p, b);
        case BT_CARD: return FindRow(m_lastPicked) != nullptr;
        case BT_USUAL: return (g_snap.flags & SNAP_USUAL) != 0;
        case BT_MORE: return true;
        case BT_FOLLOW: case BT_WAIT: case BT_GATHER: case BT_REBUFF: return (g_snap.flags & SNAP_SOMEONE_OUT) != 0;
        case BT_CLOSE: return true;
        default: return false;
        }
    }

    void Press(int bt) {
        const Row* p = SelParty();
        const Row* b = SelBench();
        switch (bt) {
        case BT_CALL: SendAction(VERB_CALL, b ? b : p, nullptr); break;
        case BT_BENCH: SendAction(VERB_BENCH, p, nullptr); break;
        case BT_SWAP: SendAction(VERB_SWAP, p, b); break;
        case BT_CARD: SendAction(VERB_CARD, FindRow(m_lastPicked), nullptr); break;
        case BT_USUAL: SendAction(VERB_USUAL, nullptr, nullptr); break;
        case BT_MORE: SendAction(VERB_MORE, nullptr, nullptr); break;
        case BT_FOLLOW: SendAction(VERB_FOLLOW, nullptr, nullptr); break;
        case BT_WAIT: SendAction(VERB_WAIT, nullptr, nullptr); break;
        case BT_GATHER: SendAction(VERB_GATHER, nullptr, nullptr); break;
        case BT_REBUFF: SendAction(VERB_REBUFF, nullptr, nullptr); break;
        case BT_CLOSE: Close(true); return;
        default: return;
        }
    }

    int ButtonAt(int x, int y) const {
        POINT pt{x, y};
        for (int bt = BT_CALL; bt < BT_COUNT; ++bt) {
            RECT rc = ButtonRect(bt);
            if (PtInRect(&rc, pt)) return bt;
        }
        return BT_NONE;
    }

    // ---- snapshot ----

    void OnSnapshot() {
        if (!FindRow(m_selParty)) m_selParty.clear();
        if (!FindRow(m_selBench)) m_selBench.clear();
        if (!FindRow(m_lastPicked)) m_lastPicked.clear();
        // a selection that moved blocks (called in, sent to the bench) keeps its row in the block it is in now
        bool inParty = false;
        if (FindRow(m_selParty, &inParty) && !inParty) { m_selBench = m_selParty; m_selParty.clear(); }
        if (FindRow(m_selBench, &inParty) && inParty) { m_selParty = m_selBench; m_selBench.clear(); }
        ClampScroll();
        InvalidateRect(nullptr);
    }

    void Close(bool bTellServer) {
        if (m_bClosing) return;
        m_bClosing = true;
        if (bTellServer && g_bEnabled) SendSimple(OP_CLOSE);
        play_ui_sound(L"MenuDown");
        Destroy();
    }

    // ---- drawing ----

    void Text(IWzCanvasPtr& c, IWzFontPtr& f, int x, int y, const std::wstring& s, int maxW = 0) {
        if (!f || s.empty()) return;
        try {
            std::wstring t = s;
            if (maxW > 0) {
                UINT w = f->CalcTextWidth(Ztl_bstr_t(t.c_str()), Ztl_variant_t());
                if (static_cast<int>(w) > maxW) {
                    while (!t.empty()) {
                        t.pop_back();
                        std::wstring e = t + L"...";
                        if (static_cast<int>(f->CalcTextWidth(Ztl_bstr_t(e.c_str()), Ztl_variant_t())) <= maxW) {
                            t = e;
                            break;
                        }
                    }
                }
            }
            c->DrawTextA(x, y, Ztl_bstr_t(t.c_str()), f, Ztl_variant_t(), Ztl_variant_t());
        } catch (...) {
        }
    }
    int TextW(IWzFontPtr& f, const std::wstring& s) {
        if (!f || s.empty()) return 0;
        try {
            return static_cast<int>(f->CalcTextWidth(Ztl_bstr_t(s.c_str()), Ztl_variant_t()));
        } catch (...) {
            return 0;
        }
    }
    static void Fill(IWzCanvasPtr& c, int x, int y, int w, int h, unsigned int color) {
        try {
            c->DrawRectangle(x, y, w, h, color);
        } catch (...) {
        }
    }

    void DrawRow(IWzCanvasPtr& c, const Row& r, int y, bool selected) {
        if (selected) {
            Fill(c, kLeft - 3, y, kRight - kLeft + 6, kRowH - 1, 0xFFC9DCF5);
        } else if (r.id == m_hoverRow) {
            Fill(c, kLeft - 3, y, kRight - kLeft + 6, kRowH - 1, 0xFFEEF3FA);
        }
        int nameW = TextW(m_pFontBold, r.name);
        Text(c, m_pFontBold, kLeft, y + 1, r.name, 120);
        Text(c, m_pFontGrey, kLeft + (std::min)(nameW, 120) + 6, y + 1, r.levelJob, 110);
        if (r.hp <= 100) {
            const int bx = kRight - 52, by = y + 5, bw = 50, bh = 5;
            Fill(c, bx - 1, by - 1, bw + 2, bh + 2, 0xFF5A5A5A);
            Fill(c, bx, by, bw, bh, 0xFFFFFFFF);
            if (r.hp > 0) Fill(c, bx, by, (std::max)(1, bw * r.hp / 100), bh, 0xFFE0403A);
        }
        IWzFontPtr& detailFont = (r.flags & ROW_COMMITTED) ? m_pFontLight : m_pFontGrey;
        Text(c, detailFont, kLeft + 4, y + 14, r.detail, kRight - kLeft - 6);
    }

    void DrawButton(IWzCanvasPtr& c, int bt) {
        RECT rc = ButtonRect(bt);
        bool on = Enabled(bt);
        unsigned int fill = !on ? 0xFFD8D8D8 : m_nPressedBtn == bt ? 0xFFB9CBE3 : m_nHoverBtn == bt ? 0xFFFFFFFF : 0xFFE6EDF6;
        Fill(c, rc.left, rc.top, kBtnW, kBtnH, 0xFF7D90AA);
        Fill(c, rc.left + 1, rc.top + 1, kBtnW - 2, kBtnH - 2, fill);
        IWzFontPtr& f = on ? m_pFont : m_pFontLight;
        std::wstring label = kButtonLabel[bt];
        int w = TextW(f, label);
        Text(c, f, rc.left + (kBtnW - w) / 2, rc.top + 3, label);
    }

    virtual void Draw(const RECT* pRect) override {
        if (this != ms_pInstance) return;
        IWzCanvasPtr c;
        try {
            c = GetCanvas();
        } catch (...) {
        }
        if (!c) return;
        try {
            if (m_pBg) {
                c->CopyEx(0, 0, m_pBg, CANVAS_ALPHATYPE::CA_OVERWRITE, 0, 0, 0, 0, 0, 0, Ztl_variant_t());
            } else {
                Fill(c, 0, 0, kWndW, g_nWndH, 0xFFF4F6FA);
            }
        } catch (...) {
        }
        if (m_bOwnTitle) { // the stock background's label is "MAPLE USER LIST & GUILD"
            Fill(c, 8, 10, 142, 7, 0xFFFFFFFF);
            Text(c, m_pFontBold, 8, 7, L"Crew");
        }

        IWzCanvasPtr pClose = m_pClose[m_nPressedBtn == BT_CLOSE ? 2 : m_nHoverBtn == BT_CLOSE ? 1 : 0];
        if (!pClose) pClose = m_pClose[0];
        if (pClose) {
            try {
                c->Copy(kCloseX, kCloseY, pClose, Ztl_variant_t());
            } catch (...) {
            }
        }

        if (!g_snap.valid) {
            Text(c, m_pFontGrey, kLeft, kPartyTop, L"Asking your crew...");
        } else {
            std::wstring head = L"Party " + std::to_wstring(g_snap.seats) + L"/" + std::to_wstring(g_snap.partyCap);
            Text(c, m_pFontBold, kLeft, kPartyTop, head);
            if (!g_snap.seatsNote.empty()) {
                Text(c, m_pFontGrey, kLeft + TextW(m_pFontBold, head) + 6, kPartyTop, g_snap.seatsNote, 200);
            }
            int y = kPartyTop + 16;
            if (g_snap.party.empty()) {
                Text(c, m_pFontLight, kLeft + 4, y + 7, L"(nobody out with me)");
            }
            for (auto& r : g_snap.party) {
                DrawRow(c, r, y, r.id == m_selParty);
                y += kRowH;
            }
            int bh = BenchHeaderY();
            Fill(c, kLeft - 3, bh - 4, kRight - kLeft + 6, 1, 0xFFB4C3D6);
            std::wstring bench = L"Bench (" + std::to_wstring(g_snap.bench.size()) + L" of "
                + std::to_wstring(g_snap.benchSeats) + L" bench seats)";
            Text(c, m_pFontBold, kLeft, bh, bench);
            int by = BenchTop();
            int visible = BenchVisible();
            if (g_snap.bench.empty()) {
                Text(c, m_pFontLight, kLeft + 4, by + 7, L"(nobody at camp)");
            }
            for (int i = 0; i < visible; ++i) {
                size_t index = static_cast<size_t>(m_nBenchScroll + i);
                if (index >= g_snap.bench.size()) break;
                DrawRow(c, g_snap.bench[index], by + i * kRowH, g_snap.bench[index].id == m_selBench);
            }
            if (m_nBenchScroll > 0) Text(c, m_pFontGrey, kRight - 8, bh, L"\x25B2");
            if (m_nBenchScroll + visible < static_cast<int>(g_snap.bench.size())) {
                Text(c, m_pFontGrey, kRight - 8, PanelBottom() - 14, L"\x25BC");
            }
        }
        for (int bt = BT_CALL; bt < BT_CLOSE; ++bt) {
            DrawButton(c, bt);
        }
        if (m_bDragging) {
            const Row* r = FindRow(m_dragId);
            if (r) {
                int w = TextW(m_pFontBold, r->name) + 8;
                Fill(c, m_nMouseX + 8, m_nMouseY + 4, w, 16, 0xFF7D90AA);
                Fill(c, m_nMouseX + 9, m_nMouseY + 5, w - 2, 14, 0xFFFFFFE0);
                Text(c, m_pFontBold, m_nMouseX + 12, m_nMouseY + 6, r->name);
            }
        }
    }

    // ---- input ----

    virtual int HitTest(int rx, int ry, CCtrlWnd** ppCtrl) override {
        int hit = CWnd::HitTest(rx, ry, ppCtrl);
        if (!hit) return 0;
        if (ppCtrl && *ppCtrl) return hit;
        // as CUIWnd: the title bar drags the window (1), the rest is ours (2); the close button is ours too
        POINT pt{rx, ry};
        RECT rcClose = ButtonRect(BT_CLOSE);
        return (ry >= 0 && ry < kTitleH && !PtInRect(&rcClose, pt)) ? 1 : 2;
    }
    virtual int OnSetFocus(int bFocus) override {
        return 0; // never takes the keyboard: the game keeps its keys and chat
    }
    virtual void OnKey(unsigned int wParam, unsigned int lParam) override {
        void* pContext = *reinterpret_cast<void**>(kWvsContextInstance);
        if (pContext) {
            reinterpret_cast<void(__thiscall*)(void*, unsigned int, unsigned int)>(kContextOnKey)(pContext, wParam, lParam);
        }
    }

    void Pick(const Row* r, bool party) {
        if (!r) return;
        std::string& sel = party ? m_selParty : m_selBench;
        sel = (sel == r->id) ? std::string() : r->id;
        m_lastPicked = sel.empty() ? std::string() : r->id;
        play_ui_sound(L"BtMouseClick");
    }

    void Drop(int x, int y) {
        const Row* from = FindRow(m_dragId);
        if (!from) return;
        RowHit target = HitRow(x, y);
        if (m_dragFromParty) {
            if (target.row && !target.party && CanSwap(from, target.row)) {
                SendAction(VERB_SWAP, from, target.row);
            } else if (InBenchBlock(y) && (from->flags & ROW_BENCH)) {
                SendAction(VERB_BENCH, from, nullptr);
            } else {
                return;
            }
        } else {
            if (target.row && target.party && CanSwap(target.row, from)) {
                SendAction(VERB_SWAP, target.row, from);
            } else if (InPartyBlock(y) && (from->flags & ROW_CALL)) {
                SendAction(VERB_CALL, from, nullptr);
            } else {
                return;
            }
        }
        play_ui_sound(L"DragEnd");
    }

    virtual void OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) override {
        if (this != ms_pInstance || m_bClosing) return;
        m_nMouseX = rx;
        m_nMouseY = ry;
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            int bt = ButtonAt(rx, ry);
            if (bt != BT_NONE) {
                if (Enabled(bt)) {
                    m_nPressedBtn = bt;
                    play_ui_sound(L"BtMouseClick");
                    InvalidateRect(nullptr);
                }
                return;
            }
            RowHit hit = HitRow(rx, ry);
            if (hit.row) {
                DWORD now = GetTickCount();
                if (hit.row->id == m_lastClickId && now - m_dwLastClick < 400) {
                    m_lastClickId.clear();
                    SendAction(VERB_CARD, hit.row, nullptr); // double-click: the card, as in the Crew window
                    return;
                }
                m_lastClickId = hit.row->id;
                m_dwLastClick = now;
                m_dragId = hit.row->id;
                m_dragFromParty = hit.party;
                m_nDownX = rx;
                m_nDownY = ry;
            }
            return;
        }
        if (msg == WM_LBUTTONUP) {
            if (m_bDragging) {
                m_bDragging = false;
                Drop(rx, ry);
                m_dragId.clear();
                InvalidateRect(nullptr);
                return;
            }
            if (!m_dragId.empty()) {
                bool party = m_dragFromParty;
                const Row* r = FindRow(m_dragId);
                m_dragId.clear();
                Pick(r, party);
                InvalidateRect(nullptr);
                return;
            }
            int pressed = m_nPressedBtn;
            m_nPressedBtn = BT_NONE;
            InvalidateRect(nullptr);
            if (pressed != BT_NONE && ButtonAt(rx, ry) == pressed && Enabled(pressed)) {
                Press(pressed);
            }
        }
    }

    virtual int OnMouseMove(int rx, int ry) override {
        if (this != ms_pInstance || m_bClosing) return 0;
        m_nMouseX = rx;
        m_nMouseY = ry;
        if (!m_dragId.empty() && !m_bDragging && (abs(rx - m_nDownX) > 4 || abs(ry - m_nDownY) > 4)) {
            m_bDragging = true;
            play_ui_sound(L"DragStart");
        }
        int bt = ButtonAt(rx, ry);
        if (bt != BT_NONE && !Enabled(bt)) bt = BT_NONE;
        RowHit hit = HitRow(rx, ry);
        std::string hover = hit.row ? hit.row->id : std::string();
        if (bt != m_nHoverBtn || hover != m_hoverRow || m_bDragging) {
            if (bt != BT_NONE && bt != m_nHoverBtn) play_ui_sound(L"BtMouseOver");
            m_nHoverBtn = bt;
            m_hoverRow = hover;
            InvalidateRect(nullptr);
        }
        return 0;
    }

    virtual int OnMouseWheel(int rx, int ry, int nWheel) override {
        if (this != ms_pInstance) return 0;
        if (InBenchBlock(ry)) {
            m_nBenchScroll += nWheel > 0 ? -1 : 1;
            ClampScroll();
            InvalidateRect(nullptr);
        }
        return 1;
    }

    virtual void OnMouseEnter(int bEnter) override {
        CWnd::OnMouseEnter(bEnter);
        if (!bEnter && (m_nHoverBtn != BT_NONE || !m_hoverRow.empty())) {
            m_nHoverBtn = BT_NONE;
            m_hoverRow.clear();
            InvalidateRect(nullptr);
        }
    }

    virtual void OnDestroy() override {
        if (ms_pInstance == this) {
            try {
                g_nSavedLeft = GetAbsLeft();
                g_nSavedTop = GetAbsTop();
            } catch (...) {
            }
            ms_pInstance = nullptr;
        }
        CWnd::OnDestroy();
    }

    virtual const CRTTI* GetRTTI() const override { return &ms_RTTI; }
    virtual int IsKindOf(const CRTTI* pRTTI) const override { return ms_RTTI.IsKindOf(pRTTI); }
};

Row SampleRow(const char* id, const wchar_t* name, const wchar_t* levelJob, const wchar_t* detail, int hp, int flags) {
    Row r;
    r.id = id;
    r.name = name;
    r.levelJob = levelJob;
    r.detail = detail;
    r.hp = hp;
    r.flags = flags;
    return r;
}

Snapshot SampleSnapshot() {
    Snapshot s;
    s.valid = true;
    s.seats = 4;
    s.partyCap = 6;
    s.benchSeats = 6;
    s.seatsNote = L"(1 seat kept)";
    s.flags = SNAP_USUAL | SNAP_SOMEONE_OUT;
    s.party.push_back(SampleRow("halvard", L"Halvard", L"Lv52 Crusader", L"Perion - HP 90%", 90, ROW_BENCH | ROW_SWAP_OUT));
    s.party.push_back(SampleRow("casse", L"Casse", L"Lv48 Priest", L"at the store - HP 100%", 100, ROW_BENCH | ROW_SWAP_OUT));
    s.party.push_back(SampleRow("kestrel", L"Kestrel", L"Lv50 Ranger", L"on the way (0:40)", 255, 0));
    s.bench.push_back(SampleRow("lioren", L"Lioren", L"Lv47 Wizard", L"train @ Ant Tunnel I - 12% EXP banked", 255, ROW_CALL | ROW_SWAP_IN));
    s.bench.push_back(SampleRow("icecookie", L"IceCookie", L"Lv45 Cleric", L"farm @ Sleepywood - 143/200 Stolen Fence - ready to level - 3,120 meso, 4 stacks", 255, ROW_CALL | ROW_SWAP_IN));
    s.bench.push_back(SampleRow("pell", L"Pell", L"Lv44 Bandit", L"away: rest @ Perion - resting", 255, ROW_CALL | ROW_SWAP_IN));
    s.bench.push_back(SampleRow("rurik", L"Rurik", L"Lv43 Page", L"sell - out of supplies", 255, ROW_CALL | ROW_SWAP_IN));
    s.bench.push_back(SampleRow("fenna", L"Fenna", L"Lv41 Assassin", L"gather @ Kerning City", 255, ROW_CALL | ROW_SWAP_IN));
    return s;
}

void OpenWindow() {
    if (CCrewWnd::ms_pInstance) return;
    if (g_bSelfTest && !g_bEnabled) {
        g_snap = SampleSnapshot();
        new CCrewWnd();
        return;
    }
    if (!g_bEnabled) return;
    g_snap = Snapshot();
    new CCrewWnd();
    SendSimple(OP_OPEN);
}

void CloseWindow(bool bTellServer) {
    if (CCrewWnd::ms_pInstance) {
        CCrewWnd::ms_pInstance->Close(bTellServer);
    }
}

// ---- packet handling ----------------------------------------------------------------------------------------------

void OnCrewPacket(const unsigned char* p, size_t n) {
    Reader r{p, n};
    int type = r.u8();
    if (!r.ok) return;
    switch (type) {
    case SV_WELCOME: {
        int protocol = r.u16();
        int enabled = r.u8();
        if (!r.ok) return;
        g_bEnabled = protocol == kProtocol && enabled != 0;
        crew_log("WELCOME protocol %d enabled %d", protocol, enabled);
        if (!g_bEnabled) {
            CloseWindow(false);
        } else if (CCrewWnd::ms_pInstance) {
            SendSimple(OP_OPEN); // open across a channel change: the new connection watches again
        }
        break;
    }
    case SV_SNAPSHOT: {
        Snapshot s;
        if (!ReadSnapshot(r, s)) {
            crew_log("bad SNAPSHOT (%u bytes)", static_cast<unsigned int>(n));
            return;
        }
        g_snap = std::move(s);
        if (CCrewWnd::ms_pInstance) CCrewWnd::ms_pInstance->OnSnapshot();
        break;
    }
    case SV_CLOSE:
        g_bEnabled = false;
        CloseWindow(false);
        break;
    case SV_OPEN:
        OpenWindow();
        break;
    default:
        break;
    }
}

// SEH must not share a frame with C++ objects that unwind, so the guarded call is its own function.
bool GuardedCrewPacket(const unsigned char* p, size_t n) {
    __try {
        OnCrewPacket(p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CInPacket as the client lays it out (CInPacket::Decode2 at 0x0042470C reads these fields).
struct InPacketView {
    int bLoopback;
    int nState;
    unsigned char* aRecvBuff;
    unsigned short uLength;
    unsigned short uRawSeq;
    unsigned short uDataLen;
    unsigned short uPad;
    unsigned int uOffset;
};
static_assert(offsetof(InPacketView, aRecvBuff) == 0x8);
static_assert(offsetof(InPacketView, uLength) == 0xC);
static_assert(offsetof(InPacketView, uOffset) == 0x14);

bool PeekOpcode(void* pPacket, unsigned short* pOp, const unsigned char** ppBody, size_t* pBodyLen) {
    __try {
        auto* v = static_cast<InPacketView*>(pPacket);
        if (!v || !v->aRecvBuff || v->uOffset + 2 > v->uLength) return false;
        const unsigned char* data = v->aRecvBuff + v->uOffset;
        *pOp = static_cast<unsigned short>(data[0] | (data[1] << 8));
        *ppBody = data + 2;
        *pBodyLen = static_cast<size_t>(v->uLength - v->uOffset - 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

auto CClientSocket__ProcessPacket = reinterpret_cast<void(__thiscall*)(void*, void*)>(kProcessPacket);

void __fastcall CClientSocket__ProcessPacket_hook(void* pThis, void* /*edx*/, void* pPacket) {
    unsigned short op = 0;
    const unsigned char* body = nullptr;
    size_t len = 0;
    bool peeked = PeekOpcode(pPacket, &op, &body, &len);
    if (peeked && op == kRecvOpcode) {
        if (!GuardedCrewPacket(body, len)) crew_log("exception while reading a crew packet");
        return; // never the game's: an unknown opcode is its stage's to ignore, and ours is fully read here
    }
    CClientSocket__ProcessPacket(pThis, pPacket);
    if (peeked && op == kSetFieldOpcode && !g_bHelloSent) {
        g_bHelloSent = true;
        SendHello();
    }
}

// ---- the hotkey -------------------------------------------------------------------------------------------------

bool KeyUnbound(int scan) {
    auto* pMan = *reinterpret_cast<unsigned char**>(kFuncKeyMappedMan);
    if (!pMan || scan <= 0 || scan >= kFuncKeySlots) return false;
    return pMan[kFuncKeyMappedOffset + scan * 5] == 0; // FUNCKEY_MAPPED { char nType; long nID; }, type 0 = empty
}

bool NoControlFocused(void* pWndMan) {
    void* pFocus = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pWndMan) + kWndManFocus);
    return pFocus == nullptr || pFocus == reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pWndMan) + 4);
}

auto CWndMan__ProcessKey = reinterpret_cast<int(__thiscall*)(void*, unsigned int, unsigned int, int)>(kProcessKey);

int __fastcall CWndMan__ProcessKey_hook(void* pThis, void* /*edx*/, unsigned int msg, unsigned int wParam, int lParam) {
    // the client sends a release as WM_KEYDOWN too, with bit 31 set (CWndMan::ProcessKey tests it first)
    if (msg == WM_KEYDOWN && wParam == g_uHotkeyVk && !(lParam & 0xC0000000) && (g_bEnabled || g_bSelfTest)) {
        int scan = (lParam >> 16) & 0xFF;
        static int s_nLogged = 0;
        if (s_nLogged < 5) {
            ++s_nLogged;
            crew_log("hotkey lParam=%08X scan=%d focus-free=%d unbound=%d", static_cast<unsigned int>(lParam), scan,
                     NoControlFocused(pThis) ? 1 : 0, KeyUnbound(scan) ? 1 : 0);
        }
        if (NoControlFocused(pThis) && KeyUnbound(scan)) {
            if (CCrewWnd::ms_pInstance) {
                CloseWindow(true);
            } else {
                OpenWindow();
            }
            return 0;
        }
    }
    return CWndMan__ProcessKey(pThis, msg, wParam, lParam);
}

// ---- teardown ---------------------------------------------------------------------------------------------------

auto CWvsContext__OnLeaveGame = reinterpret_cast<void(__thiscall*)(void*)>(kOnLeaveGame);
auto CWvsContext__ClearFieldUI = reinterpret_cast<void(__thiscall*)(void*)>(kClearFieldUI);

void __fastcall CWvsContext__OnLeaveGame_hook(void* pThis, void* /*edx*/) {
    CloseWindow(false);
    g_bEnabled = false;
    g_snap = Snapshot();
    CWvsContext__OnLeaveGame(pThis);
}

void __fastcall CWvsContext__ClearFieldUI_hook(void* pThis, void* /*edx*/) {
    CloseWindow(true);
    CWvsContext__ClearFieldUI(pThis);
}

// config.ini next to MapleStory.exe (the working directory is not always the game folder)
std::string ConfigPath() {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, n);
    size_t slash = s.find_last_of("\\/");
    return (slash == std::string::npos ? std::string(".") : s.substr(0, slash)) + "\\" CONSTANTS_CONFIG_NAME;
}

UINT ReadHotkey(const std::string& config) {
    char buf[16] = {};
    GetPrivateProfileStringA("config", "crew_hotkey", "U", buf, sizeof(buf), config.c_str());
    char ch = buf[0];
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return static_cast<UINT>(ch);
    return 'U';
}

}  // namespace


// Called by bypass.cpp's CClientSocket::Connect hook: a new connection has not said HELLO yet.
void CrewWindowOnConnect() {
    g_bHelloSent = false;
    g_bEnabled = false;
}

void AttachCrewWindowMod() {
    std::string config = ConfigPath();
    if (GetPrivateProfileIntA("config", "crew_window", 1, config.c_str()) == 0) {
        crew_log("off (config.ini crew_window=0)");
        return;
    }
    if (!v83::CheckClientVersion()) {
        crew_log("off: MapleStory.exe is not the build these offsets come from");
        return;
    }
    g_uHotkeyVk = ReadHotkey(config);
    g_bSelfTest = GetPrivateProfileIntA("config", "crew_selftest", 0, config.c_str()) != 0;
    ATTACH_HOOK(CClientSocket__ProcessPacket, CClientSocket__ProcessPacket_hook);
    ATTACH_HOOK(CWndMan__ProcessKey, CWndMan__ProcessKey_hook);
    ATTACH_HOOK(CWvsContext__OnLeaveGame, CWvsContext__OnLeaveGame_hook);
    ATTACH_HOOK(CWvsContext__ClearFieldUI, CWvsContext__ClearFieldUI_hook);
    g_bHooksOn = true;
    crew_log("on, hotkey %c%s", static_cast<char>(g_uHotkeyVk), g_bSelfTest ? ", self-test" : "");
}
