#include "pch.h"
#include "paths.h"
#include "hook.h"
#include "clientloop.h"
#include "constants.h"
#include "v83_offsets.h"
#include "v83_version_guard.h"
#include "wvs/wnd.h"
#include "wvs/packet.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include "crewproto.h"
#include "serverpackets.h"
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
//   CClientSocket::ProcessPacket  (serverpackets.cpp's, shared) 0x17A is parsed here and never reaches the game;
//                                 HELLO follows the first SET_FIELD of each connection.
//   CWndMan::ProcessKey           the hotkey, only while no edit box has the focus (so never while chatting) and only
//                                 when the key is unbound in the player's key map; Esc closes the window while the
//                                 mouse is over it or it was the last window clicked (the window still never takes
//                                 the keyboard focus).
//   clientloop::OnLeaveGame       the window closes; WELCOME is forgotten.
//   clientloop::OnClearFieldUI    the window closes (Cash Shop, MTS).
//   clientloop::OnFrame           the "server did not answer" timeout and the status line's notices.
//   clientloop::OnMessage         a click anywhere: whether the last window clicked was this one (for Esc).
// config.ini [config]: crew_window=0 skips every hook; crew_hotkey=<one letter or digit> (default U).
// config.ini [crew]: x, y  where the window was last closed (written on close, read at start).

using namespace v83::crew;
using namespace crewproto;

namespace {

void crew_log(const char* fmt, ...) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, kpaths::LogPath(), "a") != 0 || !fp) {
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
int g_nProtocol = 0;        // the protocol the last WELCOME agreed (1: More opens the NPC Crew window, 2: pages)
Snapshot g_snap;
Page g_page;                // the server's page shown over the crew, protocol 2 (valid = shown)
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

// The answer to a page, as the NPC reply: the page's stamp, the button (kReplyYes, kReplyNo, kReplyClose) and the
// option's selection value (-1 for a button).
void SendPageReply(int stamp, unsigned char action, int selection) {
    COutPacket o(kSendOpcode);
    o.Encode1(OP_PAGE_REPLY);
    o.Encode4(static_cast<unsigned int>(stamp));
    o.Encode1(action);
    o.Encode4(static_cast<unsigned int>(selection));
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
inline int StatusY() { return PanelBottom() - 14; }  // the status line, the panel's last line under the crew
inline int ListBottom() { return StatusY() - 3; }    // the crew rows end above it
inline int BtnRow1() { return g_nWndH - 47; }
inline int BtnRow2() { return g_nWndH - 25; }

enum Button { BT_NONE = 0, BT_CALL, BT_BENCH, BT_SWAP, BT_CARD, BT_USUAL, BT_MORE,
              BT_FOLLOW, BT_WAIT, BT_GATHER, BT_REBUFF, BT_CLOSE, BT_COUNT,
              // the buttons under a page
              PB_CLOSE = 100, PB_YES, PB_NO, PB_PREV, PB_NEXT };
// The tabs under the title, in the band the UserList background keeps for its own (Friend / Party / Guild): the crew
// out with the owner, the ones at camp, and the server's actions page (protocol 2: lineups, formation, hiring).
enum Tab { TAB_CREW = 0, TAB_CAMP, TAB_PLANS, TAB_COUNT };
constexpr int kTabTop = 28;
constexpr int kTabH = 18;
constexpr int kTabW = 74;
constexpr int kTabGap = 2;
constexpr int kCampRowH = 42;  // a camp row: the name line and its detail on two lines
constexpr DWORD kPlansWaitMs = 3000;  // the Plans tab says so when its page has not come this long after asking
constexpr int kPageBtnW = 60;
constexpr int kLineH = 14;
constexpr int kPageW = kRight - kLeft;
constexpr int kOptionIndent = 10;
constexpr DWORD kPageReplyWaitMs = 3000;  // a reply the server never answered (an older page's) frees the page again

const wchar_t* const kButtonLabel[BT_COUNT] = {
    L"", L"Call", L"Bench", L"Swap", L"Card", L"Usual", L"More",
    L"Follow", L"Wait", L"Gather", L"Rebuff", L"",
};

int g_nSavedLeft = INT_MIN;  // config.ini [crew] x, y: where the window was last closed
int g_nSavedTop = INT_MIN;
int g_nSavedTab = TAB_CREW;  // config.ini [crew] tab: Crew or Camp, the list the window last showed
bool g_bLastClickOurs = false;  // the last left click went to this window (Esc closes it then)

constexpr DWORD kNoAnswerMs = 6000;      // no snapshot this long after asking: say the server did not answer
constexpr DWORD kSentGoneMs = 8000;      // "Sent" with no snapshot back this long is cleared
constexpr DWORD kNoticeMs = 4000;        // a click on a switched-off button: how long its reason stays
constexpr int kRetryW = 80;

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
    int m_nTab = TAB_CREW;
    int m_nListTab = TAB_CREW;  // Crew or Camp: where a closed Plans page goes back to
    int m_nHoverTab = -1;
    int m_nPressedTab = -1;
    DWORD m_dwPlansAsked = 0;
    int m_nHoverBtn = BT_NONE;
    int m_nPressedBtn = BT_NONE;
    std::string m_hoverRow;
    bool m_bClosing = false;
    bool m_bOwnTitle = false;   // no Custom.wz art: the stock background's title is covered and "Crew" drawn
    bool m_bMouseInside = false;
    int m_nHoverAny = BT_NONE;  // the button under the mouse, switched off or not (the status line explains why)

    // waiting for the first snapshot after OPEN
    DWORD m_dwAsked = 0;
    bool m_bNoAnswer = false;
    bool m_bHoverRetry = false;
    bool m_bPressedRetry = false;

    // the status line's notice: after a press ("Sent..."), or why a clicked button is off
    std::wstring m_notice;
    DWORD m_dwNotice = 0;
    bool m_bNoticeSent = false;  // a press waiting for the next snapshot

    // a press on a row: the release on it picks it
    std::string m_dragId;
    bool m_dragFromParty = false;
    int m_nMouseX = 0, m_nMouseY = 0;
    DWORD m_dwLastClick = 0;
    std::string m_lastClickId;

    // the page view (protocol 2): the page laid out in runs of one font, relative to (kLeft, the page top)
    struct Run {
        int x, y;
        std::wstring text;
        int ink;
        bool bold;
    };
    struct OptionBox {
        int top, bottom;  // layout y, bottom exclusive
        size_t entry;     // index into g_page.entries
    };
    IWzFontPtr m_pInk[INK_COUNT][2];  // plain, bold
    std::vector<Run> m_runs;
    std::vector<OptionBox> m_boxes;
    int m_nPageH = 0;
    int m_nPageScroll = 0;
    bool m_bLayoutDirty = true;
    int m_nHoverOption = -1;   // index into m_boxes
    int m_nPressedOption = -1;
    bool m_bPageWaiting = false;  // a reply went out: the page takes no clicks until the server answers
    DWORD m_dwPageSent = 0;

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
        m_nTab = m_nListTab = g_nSavedTab == TAB_CAMP ? TAB_CAMP : TAB_CREW;
        m_dwAsked = GetTickCount();
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
        m_pInk[INK_BLACK][0] = m_pFont;
        m_pInk[INK_BLACK][1] = m_pFontBold;
        m_pInk[INK_BLUE][0] = Font(kFontBlue);
        m_pInk[INK_BLUE][1] = Font(kFontBlueBold);
        m_pInk[INK_RED][0] = Font(kFontRed);
        m_pInk[INK_RED][1] = Font(kFontRedBold);
        m_pInk[INK_PURPLE][0] = Font(kFontPurple);
        m_pInk[INK_GREEN][0] = Font(kFontGreen);
    }

    // the font for a run: its ink and weight, else its ink plain, else black
    IWzFontPtr& InkFont(int ink, bool bold) {
        if (ink < 0 || ink >= INK_COUNT) ink = INK_BLACK;
        if (m_pInk[ink][bold ? 1 : 0]) return m_pInk[ink][bold ? 1 : 0];
        if (m_pInk[ink][0]) return m_pInk[ink][0];
        return bold ? m_pFontBold : m_pFont;
    }

    // ---- geometry ----

    // Each list tab: its heading at kPartyTop, its rows from ListTop().
    static int ListTop() { return kPartyTop + 16; }
    int BenchVisible() const { return (std::max)(1, (ListBottom() - ListTop()) / kCampRowH); }

    static RECT TabRect(int tab) {
        int x = kLeft - 3 + tab * (kTabW + kTabGap);
        return {x, kTabTop, x + kTabW, kTabTop + kTabH};
    }
    static int TabAt(int x, int y) {
        POINT pt{x, y};
        for (int tab = 0; tab < TAB_COUNT; ++tab) {
            RECT rc = TabRect(tab);
            if (PtInRect(&rc, pt)) return tab;
        }
        return -1;
    }
    std::wstring TabLabel(int tab) const {
        switch (tab) {
        case TAB_CREW:
            return g_snap.valid ? L"Crew " + std::to_wstring(g_snap.seats) + L"/" + std::to_wstring(g_snap.partyCap)
                                : std::wstring(L"Crew");
        case TAB_CAMP:
            return g_snap.valid ? L"Camp " + std::to_wstring(g_snap.bench.size()) + L"/"
                                      + std::to_wstring(g_snap.benchSeats)
                                : std::wstring(L"Camp");
        default: return L"Plans";
        }
    }

    // The buttons a tab shows: the row over the bottom one, left to right.
    static const std::vector<int>& TabButtons(int tab, int row) {
        static const std::vector<int> crew1{BT_CALL, BT_BENCH, BT_SWAP, BT_CARD};
        static const std::vector<int> crew2{BT_FOLLOW, BT_WAIT, BT_GATHER, BT_REBUFF};
        static const std::vector<int> camp1{BT_CALL, BT_SWAP, BT_CARD};
        static const std::vector<int> none;
        if (tab == TAB_CREW) return row == 0 ? crew1 : crew2;
        if (tab == TAB_CAMP) return row == 0 ? camp1 : none;
        return none;
    }
    void ClampScroll() {
        int maxScroll = (std::max)(0, static_cast<int>(g_snap.bench.size()) - BenchVisible());
        m_nBenchScroll = (std::max)(0, (std::min)(m_nBenchScroll, maxScroll));
    }

    // Where a button stands on the tab shown; an empty rect for one the tab does not show.
    RECT ButtonRect(int bt) const {
        if (bt == BT_CLOSE) return {kCloseX, kCloseY, kCloseX + 12, kCloseY + 12};
        for (int row = 0; row < 2; ++row) {
            const std::vector<int>& buttons = TabButtons(m_nTab, row);
            int count = static_cast<int>(buttons.size());
            for (int index = 0; index < count; ++index) {
                if (buttons[static_cast<size_t>(index)] != bt) continue;
                int gap = 3;
                int total = count * kBtnW + (count - 1) * gap;
                int x = (kWndW - total) / 2 + index * (kBtnW + gap);
                int y = row == 0 ? BtnRow1() : BtnRow2();
                return {x, y, x + kBtnW, y + kBtnH};
            }
        }
        return {0, 0, 0, 0};
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
        int top = ListTop();
        if (m_nTab == TAB_CREW) {
            for (size_t i = 0; i < g_snap.party.size(); ++i) {
                int ry = top + static_cast<int>(i) * kRowH;
                if (y >= ry && y < ry + kRowH) { hit.row = &g_snap.party[i]; hit.party = true; return hit; }
            }
        } else if (m_nTab == TAB_CAMP) {
            int visible = BenchVisible();
            for (int i = 0; i < visible; ++i) {
                size_t index = static_cast<size_t>(m_nBenchScroll + i);
                if (index >= g_snap.bench.size()) break;
                int ry = top + i * kCampRowH;
                if (y >= ry && y < ry + kCampRowH) { hit.row = &g_snap.bench[index]; hit.party = false; return hit; }
            }
        }
        return hit;
    }

    // the "Ask again" button under "the server did not answer"
    static RECT RetryRect() { return {kLeft, kPartyTop + 50, kLeft + kRetryW, kPartyTop + 50 + kBtnH}; }
    bool InRetry(int x, int y) const {
        if (g_snap.valid || !m_bNoAnswer) return false;
        RECT rc = RetryRect();
        POINT pt{x, y};
        return PtInRect(&rc, pt) != 0;
    }

    // ---- what each button would do now ----

    // a pair one of the card's swap pickers lists: the one out makes room for one at camp who can come at once, or
    // the one at camp swaps in for one out with the owner
    static bool CanSwap(const Row* out, const Row* in) {
        return out && in && (((out->flags & ROW_SWAP_OUT) && (in->flags & ROW_CAN_COME))
                             || ((in->flags & ROW_SWAP_IN) && (out->flags & ROW_OUT)));
    }

    const Row* SelParty() const { return FindRow(m_selParty); }
    const Row* SelBench() const { return FindRow(m_selBench); }

    bool Enabled(int bt) const {
        if (!g_snap.valid) return bt == BT_CLOSE;
        const Row* p = SelParty();
        const Row* b = SelBench();
        switch (bt) {
        case BT_CALL: { // the one picked on the tab shown
            const Row* r = m_nTab == TAB_CAMP ? b : p;
            return r && (r->flags & ROW_CALL);
        }
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

    // Why a button is off, for the status line. Plain words; the button names stay as the server's menus use them.
    std::wstring WhyOff(int bt) const {
        if (!g_snap.valid) {
            return m_bNoAnswer ? L"The server did not answer, so this is off." : L"Waiting for your crew...";
        }
        const Row* p = SelParty();
        const Row* b = SelBench();
        switch (bt) {
        case BT_CALL:
            if (m_nTab == TAB_CAMP) {
                return b ? b->name + L" can't come right now." : std::wstring(L"Pick someone at camp to call.");
            }
            return p ? p->name + L" is already out with you." : std::wstring(L"Pick someone to call them to you.");
        case BT_BENCH:
            if (p) return p->name + L" can't go to camp right now.";
            return L"Pick someone to send to camp.";
        case BT_SWAP:
            if (p && b) return p->name + L" and " + b->name + L" can't swap right now.";
            if (p) return L"Now pick who takes " + p->name + L"'s place, on the Camp tab.";
            if (b) return L"Now pick who makes room for " + b->name + L", on the Crew tab.";
            return L"Pick one on the Crew tab and one on the Camp tab.";
        case BT_CARD: return L"Pick someone to see their card.";
        case BT_USUAL: return L"No usual lineup to call right now.";
        case BT_FOLLOW: case BT_WAIT: case BT_GATHER: case BT_REBUFF: return L"Nobody is out with you.";
        default: return L"";
        }
    }

    // ---- the status line ----

    void Notice(const std::wstring& text, bool bSent) {
        m_notice = text;
        m_dwNotice = GetTickCount();
        m_bNoticeSent = bSent;
        InvalidateRect(nullptr);
    }
    void Sent() {
        Notice(L"Sent. Waiting for the server...", true);
    }
    void ClearNotice() {
        if (m_notice.empty()) return;
        m_notice.clear();
        m_bNoticeSent = false;
        InvalidateRect(nullptr);
    }

    std::wstring StatusText() const {
        if (!g_snap.valid) return L"";
        if (m_nHoverAny != BT_NONE && m_nHoverAny != BT_CLOSE && !Enabled(m_nHoverAny)) return WhyOff(m_nHoverAny);
        if (!m_notice.empty()) return m_notice;
        if (!m_hoverRow.empty()) return L"Click to pick, double-click for the card.";
        return L"";
    }

    // Every frame: the no-answer timeout and the notices' lifetimes.
    void Tick() {
        DWORD now = GetTickCount();
        if (!g_snap.valid && !m_bNoAnswer && !g_page.valid && now - m_dwAsked > kNoAnswerMs) {
            m_bNoAnswer = true;
            crew_log("no SNAPSHOT %lu ms after OPEN", static_cast<unsigned long>(kNoAnswerMs));
            InvalidateRect(nullptr);
        }
        if (m_notice.empty()) return;
        // a press that brought no snapshot (the server may refuse without one) stops saying "Sent" after a while
        if (now - m_dwNotice > (m_bNoticeSent ? kSentGoneMs : kNoticeMs)) ClearNotice();
    }

    // "Ask again": the same OPEN the window sent when it opened.
    void AskAgain() {
        m_bNoAnswer = false;
        m_dwAsked = GetTickCount();
        crew_log("asking again (OPEN)");
        if (g_bEnabled) SendSimple(OP_OPEN);
        InvalidateRect(nullptr);
    }

    void Press(int bt) {
        const Row* p = SelParty();
        const Row* b = SelBench();
        if (bt != BT_CLOSE) Sent();
        switch (bt) {
        case BT_CALL: SendAction(VERB_CALL, m_nTab == TAB_CAMP ? b : p, nullptr); break;
        case BT_BENCH: SendAction(VERB_BENCH, p, nullptr); break;
        case BT_SWAP: SendAction(VERB_SWAP, p, b); break;
        case BT_CARD: SendAction(VERB_CARD, FindRow(m_lastPicked), nullptr); break;
        case BT_USUAL: SendAction(VERB_USUAL, nullptr, nullptr); break;
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

    // ---- tabs ----

    // A click on a tab. Crew and Camp show their list (and close a page that is open); Plans asks the server for its
    // actions page (protocol 2), or opens the NPC Crew window on a protocol 1 server and stays on the list.
    void ShowTab(int tab) {
        if (tab == TAB_PLANS) {
            if (g_bSelfTest && !g_bEnabled) {
                m_nTab = TAB_PLANS;
                ShowSamplePage(0);
                return;
            }
            if (g_nProtocol < 2) {
                SendAction(VERB_MORE, nullptr, nullptr);
                Sent();
                return;
            }
            m_nTab = TAB_PLANS;
            m_dwPlansAsked = GetTickCount();
            if (!g_page.valid) SendAction(VERB_ACTIONS, nullptr, nullptr);
            InvalidateRect(nullptr);
            return;
        }
        if (g_page.valid) { // leaving a page: as its Close button
            if (g_bSelfTest && !g_bEnabled) {
                g_page = Page();
            } else {
                SendPageReply(g_page.stamp, kReplyClose, -1);
                g_page = Page();
            }
            m_bPageWaiting = false;
        }
        m_nTab = m_nListTab = tab;
        m_nHoverBtn = m_nHoverAny = BT_NONE;
        m_hoverRow.clear();
        ClampScroll();
        InvalidateRect(nullptr);
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
        m_bNoAnswer = false;
        if (m_bNoticeSent) ClearNotice(); // the answer to the press is on screen now
        InvalidateRect(nullptr);
    }

    // ---- the page view (protocol 2) ----

    void OnPage(Page&& page) {
        g_page = std::move(page);
        m_bLayoutDirty = true;
        m_nPageScroll = 0;
        m_nHoverOption = -1;
        m_nPressedOption = -1;
        m_bPageWaiting = false;
        m_dragId.clear();
        if (m_bNoticeSent) ClearNotice();
        InvalidateRect(nullptr);
    }

    void OnPageClose(int stamp) {
        if (!g_page.valid || (stamp != 0 && stamp != g_page.stamp)) return;
        g_page = Page();
        m_bPageWaiting = false;
        m_nHoverBtn = BT_NONE;
        m_nPressedBtn = BT_NONE;
        if (m_nTab == TAB_PLANS) m_nTab = m_nListTab; // the page is the Plans tab: without it, back to the list
        InvalidateRect(nullptr);
    }

    int PageTop() const { return kPartyTop; }
    int PageVisibleH() const { return PanelBottom() - PageTop(); }
    void ClampPageScroll() {
        int maxScroll = (std::max)(0, m_nPageH - PageVisibleH());
        m_nPageScroll = (std::max)(0, (std::min)(m_nPageScroll, maxScroll));
    }

    // Lays spans out from (x0, y), word by word, wrapping at the page width; returns the y under the last line.
    int Flow(const std::vector<Span>& spans, int x0, int y) {
        if (spans.empty()) return y;
        int x = x0;
        for (const Span& sp : spans) {
            if (sp.lineBreak) {
                y += kLineH;
                x = x0;
                continue;
            }
            IWzFontPtr& f = InkFont(sp.ink, sp.bold);
            const std::wstring& t = sp.text;
            size_t i = 0;
            while (i < t.size()) {
                size_t j = i;
                while (j < t.size() && t[j] != L' ') ++j;
                size_t wordEnd = j;
                while (j < t.size() && t[j] == L' ') ++j;
                std::wstring word = t.substr(i, wordEnd - i);
                std::wstring spaces = t.substr(wordEnd, j - wordEnd);
                i = j;
                int w = TextW(f, word);
                if (x > x0 && x + w > kPageW) {
                    y += kLineH;
                    x = x0;
                }
                if (w > kPageW - x0) { // wider than a line: break it by characters
                    std::wstring part;
                    for (wchar_t ch : word) {
                        if (!part.empty() && x + TextW(f, part + ch) > kPageW) {
                            m_runs.push_back({x, y, part, sp.ink, sp.bold});
                            y += kLineH;
                            x = x0;
                            part.clear();
                        }
                        part.push_back(ch);
                    }
                    word = part;
                    w = TextW(f, word);
                }
                if (!word.empty()) m_runs.push_back({x, y, word, sp.ink, sp.bold});
                x += w + TextW(f, spaces);
            }
        }
        return y + kLineH;
    }

    void Layout() {
        m_runs.clear();
        m_boxes.clear();
        int y = Flow(ParseMarkup(g_page.text), 0, 0);
        for (size_t k = 0; k < g_page.entries.size(); ++k) {
            const PageEntry& e = g_page.entries[k];
            std::vector<Span> spans = ParseMarkup(e.label);
            if (!e.option) {
                y = spans.empty() ? y + kLineH : Flow(spans, 0, y);
                continue;
            }
            int top = y;
            y = Flow(spans, kOptionIndent, y + 2) + 2;
            m_boxes.push_back({top, y, k});
        }
        m_nPageH = y;
        m_bLayoutDirty = false;
        ClampPageScroll();
    }

    int OptionAt(int x, int y) const {
        if (!g_page.valid || x < kLeft - 4 || x > kRight + 4 || y < PageTop() || y >= PanelBottom()) return -1;
        int ly = y - PageTop() + m_nPageScroll;
        for (size_t b = 0; b < m_boxes.size(); ++b) {
            if (ly >= m_boxes[b].top && ly < m_boxes[b].bottom) return static_cast<int>(b);
        }
        return -1;
    }

    std::vector<int> PageButtons() const {
        switch (g_page.kind) {
        case PAGE_MENU: return {PB_CLOSE};
        case PAGE_YES_NO:
        case PAGE_ACCEPT_DECLINE: return {PB_YES, PB_NO};
        default: {
            std::vector<int> v;
            if (g_page.flags & PAGE_PREV) v.push_back(PB_PREV);
            v.push_back(PB_NEXT);
            return v;
        }
        }
    }

    const wchar_t* PageButtonLabel(int pb) const {
        bool accept = g_page.kind == PAGE_ACCEPT_DECLINE;
        switch (pb) {
        case PB_CLOSE: return L"Close";
        case PB_YES: return accept ? L"Accept" : L"Yes";
        case PB_NO: return accept ? L"Decline" : L"No";
        case PB_PREV: return L"Prev";
        case PB_NEXT: return (g_page.flags & PAGE_NEXT) ? L"Next" : L"OK";
        default: return L"";
        }
    }

    RECT PageButtonRect(int pb) const {
        std::vector<int> buttons = PageButtons();
        int count = static_cast<int>(buttons.size());
        int index = 0;
        for (int k = 0; k < count; ++k) {
            if (buttons[static_cast<size_t>(k)] == pb) index = k;
        }
        int gap = 8;
        int total = count * kPageBtnW + (count - 1) * gap;
        int x = (kWndW - total) / 2 + index * (kPageBtnW + gap);
        int y = BtnRow1() + (BtnRow2() - BtnRow1()) / 2;
        return {x, y, x + kPageBtnW, y + kBtnH};
    }

    int PageButtonAt(int x, int y) const {
        POINT pt{x, y};
        for (int pb : PageButtons()) {
            RECT rc = PageButtonRect(pb);
            if (PtInRect(&rc, pt)) return pb;
        }
        return BT_NONE;
    }

    bool PageWaiting() {
        if (m_bPageWaiting && GetTickCount() - m_dwPageSent > kPageReplyWaitMs) m_bPageWaiting = false;
        return m_bPageWaiting;
    }

    void Reply(unsigned char action, int selection) {
        if (PageWaiting()) return;
        if (g_bSelfTest && !g_bEnabled) {
            SampleReply(action, selection);
            return;
        }
        SendPageReply(g_page.stamp, action, selection);
        m_bPageWaiting = true;
        m_dwPageSent = GetTickCount();
        InvalidateRect(nullptr);
    }

    void PressPageButton(int pb) {
        switch (pb) {
        case PB_CLOSE: Reply(kReplyClose, -1); break;
        case PB_YES: Reply(kReplyYes, -1); break;
        case PB_NO: Reply(kReplyNo, -1); break;
        case PB_PREV: Reply(kReplyNo, -1); break;
        case PB_NEXT: Reply(kReplyYes, -1); break;
        default: break;
        }
    }

    void DrawPageButton(IWzCanvasPtr& c, int pb) {
        RECT rc = PageButtonRect(pb);
        bool on = !m_bPageWaiting;
        unsigned int fill = !on ? 0xFFD8D8D8 : m_nPressedBtn == pb ? 0xFFB9CBE3 : m_nHoverBtn == pb ? 0xFFFFFFFF : 0xFFE6EDF6;
        Fill(c, rc.left, rc.top, kPageBtnW, kBtnH, 0xFF7D90AA);
        Fill(c, rc.left + 1, rc.top + 1, kPageBtnW - 2, kBtnH - 2, fill);
        IWzFontPtr& f = on ? m_pFont : m_pFontLight;
        std::wstring label = PageButtonLabel(pb);
        Text(c, f, rc.left + (kPageBtnW - TextW(f, label)) / 2, rc.top + 3, label);
    }

    void DrawPage(IWzCanvasPtr& c) {
        if (m_bLayoutDirty) Layout();
        PageWaiting();
        int top = PageTop();
        int bottom = PanelBottom();
        int oy = top - m_nPageScroll;
        for (size_t b = 0; b < m_boxes.size(); ++b) {
            int y0 = oy + m_boxes[b].top;
            int y1 = oy + m_boxes[b].bottom;
            if (y1 <= top || y0 >= bottom) continue;
            int t = (std::max)(y0, top);
            int h = (std::min)(y1, bottom) - t;
            if (!m_bPageWaiting && static_cast<int>(b) == m_nPressedOption) {
                Fill(c, kLeft - 3, t, kPageW + 6, h, 0xFFC9DCF5);
            } else if (!m_bPageWaiting && static_cast<int>(b) == m_nHoverOption) {
                Fill(c, kLeft - 3, t, kPageW + 6, h, 0xFFEEF3FA);
            }
            if (y0 + 8 >= top && y0 + 11 <= bottom) Fill(c, kLeft + 2, y0 + 8, 3, 3, 0xFF7D90AA);
        }
        for (const Run& r : m_runs) {
            int y = oy + r.y;
            if (y < top || y + kLineH - 2 > bottom) continue;
            Text(c, InkFont(r.ink, r.bold), kLeft + r.x, y, r.text);
        }
        if (m_nPageScroll > 0) Text(c, m_pFontGrey, kRight - 8, top - 14, L"\x25B2");
        if (m_nPageScroll + PageVisibleH() < m_nPageH) Text(c, m_pFontGrey, kRight - 8, bottom - 2, L"\x25BC");
        for (int pb : PageButtons()) {
            DrawPageButton(c, pb);
        }
    }

    // ---- self-test pages (crew_selftest=1 and no server): the page view without a server ----

    static Page SamplePage(int which) {
        Page pg;
        pg.valid = true;
        pg.stamp = which + 1;
        auto option = [&](int selection, const wchar_t* label) { pg.entries.push_back({true, selection, label}); };
        auto heading = [&](const wchar_t* label) { pg.entries.push_back({false, 0, label}); };
        switch (which) {
        case 0:
            pg.kind = PAGE_MENU;
            pg.text = L"#eThe crew#n\r\n";
            option(1, L"Call my usual lineup");
            option(2, L"Remember this lineup");
            option(3, L"Everyone: follow me");
            option(3, L"Everyone: wait here");
            option(4, L"Formation...");
            option(5, L"Everyone on the bench...");
            option(5, L"Keep my crew fresh (off)");
            option(6, L"Make room for a friend...");
            option(7, L"Adventurers looking for a crew...");
            option(8, L"My lineups...");
            option(9, L"More...");
            break;
        case 1:
            pg.kind = PAGE_MENU;
            pg.text = L"The lineups I keep:\r\n#eHunting#n: Halvard, Casse, Kestrel\r\n#ePQ#n: (none yet)\r\n"
                      L"#eBoss#n: Halvard, Casse, Kestrel, Lioren, IceCookie, Pell and a #bvery long#k name list "
                      L"that has to wrap onto the next line\r\n";
            option(1, L"Call the Hunting lineup");
            option(1, L"Call the Boss lineup");
            heading(L"\r\nOr keep this crew:");
            option(1, L"Remember this crew as Hunting");
            option(1, L"Remember this crew as PQ");
            option(1, L"Remember this crew as Boss");
            option(0, L"Back");
            break;
        case 2:
            pg.kind = PAGE_YES_NO;
            pg.text = L"#eHalvard#n signed on for #r4 more hours#k. Benched now, he won't sit at camp for long. "
                      L"Make room anyway?";
            break;
        default:
            pg.kind = PAGE_OK;
            pg.flags = PAGE_PREV;
            pg.text = L"#bRemembered#k: Halvard, Casse and Kestrel are your #eHunting#n lineup.";
            break;
        }
        return pg;
    }

    void ShowSamplePage(int which) {
        OnPage(SamplePage(which));
    }

    void SampleReply(unsigned char action, int selection) {
        if (action == kReplyClose) {
            OnPageClose(0);
            return;
        }
        if (g_page.kind == PAGE_MENU) {
            if (selection == 0) ShowSamplePage(0);
            else if (selection == 8) ShowSamplePage(1);
            else if (selection == 6) ShowSamplePage(2);
            else ShowSamplePage(3);
            return;
        }
        if (g_page.kind == PAGE_OK && action == kReplyNo) {
            ShowSamplePage(1);
            return;
        }
        OnPageClose(0);
    }

    void Close(bool bTellServer) {
        if (m_bClosing) return;
        m_bClosing = true;
        g_page = Page(); // CLOSE ends the server's page too
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

    // A camp row: the name line, then the detail on two lines, broken at one of its " - " joints where the first
    // line still fits (the text itself is the server's, shown as it is).
    void DrawCampRow(IWzCanvasPtr& c, const Row& r, int y, bool selected) {
        if (selected) {
            Fill(c, kLeft - 3, y, kRight - kLeft + 6, kCampRowH - 1, 0xFFC9DCF5);
        } else if (r.id == m_hoverRow) {
            Fill(c, kLeft - 3, y, kRight - kLeft + 6, kCampRowH - 1, 0xFFEEF3FA);
        }
        int nameW = TextW(m_pFontBold, r.name);
        Text(c, m_pFontBold, kLeft, y + 1, r.name, 120);
        Text(c, m_pFontGrey, kLeft + (std::min)(nameW, 120) + 6, y + 1, r.levelJob, 150);
        IWzFontPtr& f = (r.flags & ROW_COMMITTED) ? m_pFontLight : m_pFontGrey;
        const int w = kRight - kLeft - 6;
        std::wstring first = r.detail, second;
        if (TextW(f, r.detail) > w) {
            size_t cut = std::wstring::npos;
            for (size_t at = r.detail.find(L" - "); at != std::wstring::npos; at = r.detail.find(L" - ", at + 3)) {
                if (TextW(f, r.detail.substr(0, at)) > w) break;
                cut = at;
            }
            if (cut != std::wstring::npos) {
                first = r.detail.substr(0, cut);
                second = r.detail.substr(cut + 3);
            }
        }
        Text(c, f, kLeft + 4, y + 14, first, w);
        Text(c, f, kLeft + 4, y + 27, second, w);
    }

    void DrawTabs(IWzCanvasPtr& c) {
        for (int tab = 0; tab < TAB_COUNT; ++tab) {
            RECT rc = TabRect(tab);
            bool on = tab == m_nTab;
            unsigned int fill = on ? 0xFFFFFFFF : m_nPressedTab == tab ? 0xFFB9CBE3 : m_nHoverTab == tab ? 0xFFEEF3FA
                                                                                                         : 0xFFD5DDE8;
            int top = on ? rc.top : rc.top + 2; // the tab shown stands a little taller
            Fill(c, rc.left, top, kTabW, rc.bottom - top, 0xFF7D90AA);
            Fill(c, rc.left + 1, top + 1, kTabW - 2, rc.bottom - top - (on ? 1 : 2), fill);
            IWzFontPtr& f = on ? m_pFontBold : m_pFontGrey;
            std::wstring label = TabLabel(tab);
            Text(c, f, rc.left + (kTabW - TextW(f, label)) / 2, top + 3, label);
        }
    }

    void DrawButton(IWzCanvasPtr& c, int bt) {
        RECT rc = ButtonRect(bt);
        if (rc.right <= rc.left) return; // not on this tab
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
        { // the key that opens and closes the window, beside the title (left of the close button)
            std::wstring key = L"Key: ";
            key.push_back(static_cast<wchar_t>(g_uHotkeyVk));
            Text(c, m_pFontGrey, kCloseX - 6 - TextW(m_pFontGrey, key), kCloseY, key);
        }

        IWzCanvasPtr pClose = m_pClose[m_nPressedBtn == BT_CLOSE ? 2 : m_nHoverBtn == BT_CLOSE ? 1 : 0];
        if (!pClose) pClose = m_pClose[0];
        if (pClose) {
            try {
                c->Copy(kCloseX, kCloseY, pClose, Ztl_variant_t());
            } catch (...) {
            }
        }

        DrawTabs(c);
        if (g_page.valid) { // a page covers the crew until it closes; snapshots keep arriving underneath
            DrawPage(c);
            return;
        }
        if (m_nTab == TAB_PLANS) { // its page has not come (yet)
            bool late = GetTickCount() - m_dwPlansAsked > kPlansWaitMs;
            Text(c, m_pFontGrey, kLeft, kPartyTop, late ? L"The server sent no plans. Click Plans to ask again."
                                                       : L"Asking for your plans...");
            return;
        }
        if (!g_snap.valid && !m_bNoAnswer) {
            Text(c, m_pFontGrey, kLeft, kPartyTop, L"Asking your crew...");
        } else if (!g_snap.valid) {
            Text(c, m_pFontBold, kLeft, kPartyTop, L"The server did not answer.");
            Text(c, m_pFontGrey, kLeft, kPartyTop + 18, L"It may be busy. Ask again, or close this window.");
            RECT rc = RetryRect();
            unsigned int fill = m_bPressedRetry ? 0xFFB9CBE3 : m_bHoverRetry ? 0xFFFFFFFF : 0xFFE6EDF6;
            Fill(c, rc.left, rc.top, kRetryW, kBtnH, 0xFF7D90AA);
            Fill(c, rc.left + 1, rc.top + 1, kRetryW - 2, kBtnH - 2, fill);
            std::wstring label = L"Ask again";
            Text(c, m_pFont, rc.left + (kRetryW - TextW(m_pFont, label)) / 2, rc.top + 3, label);
        } else if (m_nTab == TAB_CREW) {
            std::wstring head = L"Out with you";
            Text(c, m_pFontBold, kLeft, kPartyTop, head);
            if (!g_snap.seatsNote.empty()) {
                Text(c, m_pFontGrey, kLeft + TextW(m_pFontBold, head) + 6, kPartyTop, g_snap.seatsNote, 200);
            }
            int y = ListTop();
            if (g_snap.party.empty()) {
                Text(c, m_pFontLight, kLeft + 4, y + 7, L"Nobody is out with you.");
                Text(c, m_pFontLight, kLeft + 4, y + 21, L"Call someone from the Camp tab.");
            }
            for (auto& r : g_snap.party) {
                DrawRow(c, r, y, r.id == m_selParty);
                y += kRowH;
            }
        } else {
            Text(c, m_pFontBold, kLeft, kPartyTop, L"At camp");
            int by = ListTop();
            int visible = BenchVisible();
            if (g_snap.bench.empty()) {
                Text(c, m_pFontLight, kLeft + 4, by + 7, L"Nobody is at camp.");
            }
            for (int i = 0; i < visible; ++i) {
                size_t index = static_cast<size_t>(m_nBenchScroll + i);
                if (index >= g_snap.bench.size()) break;
                DrawCampRow(c, g_snap.bench[index], by + i * kCampRowH, g_snap.bench[index].id == m_selBench);
            }
            if (m_nBenchScroll > 0) Text(c, m_pFontGrey, kRight - 8, kPartyTop, L"\x25B2");
            if (m_nBenchScroll + visible < static_cast<int>(g_snap.bench.size())) {
                Text(c, m_pFontGrey, kRight - 8, ListBottom() - 14, L"\x25BC");
            }
        }
        std::wstring status = StatusText();
        if (!status.empty()) {
            Fill(c, kLeft - 3, StatusY() - 2, kRight - kLeft + 6, 1, 0xFFB4C3D6);
            Text(c, m_pFontGrey, kLeft, StatusY(), status, kRight - kLeft);
        }
        for (int bt = BT_CALL; bt < BT_CLOSE; ++bt) {
            DrawButton(c, bt);
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

    // A page takes the clicks: its options and buttons, and the title bar's close button.
    void PageMouseButton(unsigned int msg, int rx, int ry) {
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            POINT pt{rx, ry};
            RECT rcClose = ButtonRect(BT_CLOSE);
            int pb = PtInRect(&rcClose, pt) ? BT_CLOSE : PageButtonAt(rx, ry);
            if (pb != BT_NONE && (pb == BT_CLOSE || !PageWaiting())) {
                m_nPressedBtn = pb;
                play_ui_sound(L"BtMouseClick");
                InvalidateRect(nullptr);
                return;
            }
            int option = PageWaiting() ? -1 : OptionAt(rx, ry);
            if (option >= 0) {
                m_nPressedOption = option;
                InvalidateRect(nullptr);
            }
            return;
        }
        if (msg != WM_LBUTTONUP) return;
        int pressedBtn = m_nPressedBtn;
        int pressedOption = m_nPressedOption;
        m_nPressedBtn = BT_NONE;
        m_nPressedOption = -1;
        InvalidateRect(nullptr);
        if (pressedBtn == BT_CLOSE) {
            POINT pt{rx, ry};
            RECT rcClose = ButtonRect(BT_CLOSE);
            if (PtInRect(&rcClose, pt)) Close(true);
            return;
        }
        if (pressedBtn != BT_NONE && PageButtonAt(rx, ry) == pressedBtn) {
            PressPageButton(pressedBtn);
            return;
        }
        if (pressedOption >= 0 && OptionAt(rx, ry) == pressedOption) {
            const PageEntry& e = g_page.entries[m_boxes[static_cast<size_t>(pressedOption)].entry];
            play_ui_sound(L"BtMouseClick");
            Reply(kReplyYes, e.selection);
        }
    }

    virtual void OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) override {
        if (this != ms_pInstance || m_bClosing) return;
        m_nMouseX = rx;
        m_nMouseY = ry;
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) g_bLastClickOurs = true; // Esc closes this window now
        { // the tabs take their clicks over the lists and over a page alike
            int tab = TabAt(rx, ry);
            if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && tab >= 0) {
                m_nPressedTab = tab;
                play_ui_sound(L"BtMouseClick");
                InvalidateRect(nullptr);
                return;
            }
            if (msg == WM_LBUTTONUP && m_nPressedTab >= 0) {
                int pressed = m_nPressedTab;
                m_nPressedTab = -1;
                InvalidateRect(nullptr);
                if (tab == pressed && (pressed != m_nTab || pressed == TAB_PLANS)) ShowTab(pressed);
                return;
            }
        }
        if (g_page.valid) {
            PageMouseButton(msg, rx, ry);
            return;
        }
        if (m_nTab == TAB_PLANS) return; // nothing to click while its page is on the way
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            if (InRetry(rx, ry)) {
                m_bPressedRetry = true;
                play_ui_sound(L"BtMouseClick");
                InvalidateRect(nullptr);
                return;
            }
            int bt = ButtonAt(rx, ry);
            if (bt != BT_NONE) {
                if (Enabled(bt)) {
                    m_nPressedBtn = bt;
                    play_ui_sound(L"BtMouseClick");
                    InvalidateRect(nullptr);
                } else if (g_snap.valid) {
                    Notice(WhyOff(bt), false);
                }
                return;
            }
            RowHit hit = HitRow(rx, ry);
            if (hit.row) {
                DWORD now = GetTickCount();
                if (hit.row->id == m_lastClickId && now - m_dwLastClick < 400) {
                    m_lastClickId.clear();
                    SendAction(VERB_CARD, hit.row, nullptr); // double-click: the card, as in the Crew window
                    Sent();
                    return;
                }
                m_lastClickId = hit.row->id;
                m_dwLastClick = now;
                m_dragId = hit.row->id;
                m_dragFromParty = hit.party;
            }
            return;
        }
        if (msg == WM_LBUTTONUP) {
            if (m_bPressedRetry) {
                m_bPressedRetry = false;
                if (InRetry(rx, ry)) AskAgain();
                InvalidateRect(nullptr);
                return;
            }
            if (!m_dragId.empty()) {
                bool party = m_dragFromParty;
                const Row* r = FindRow(m_dragId);
                bool same = HitRow(rx, ry).row == r; // released on the row it went down on
                m_dragId.clear();
                if (same) Pick(r, party);
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
        {
            int tab = TabAt(rx, ry);
            if (tab != m_nHoverTab) {
                m_nHoverTab = tab;
                InvalidateRect(nullptr);
            }
        }
        if (g_page.valid) {
            POINT pt{rx, ry};
            RECT rcClose = ButtonRect(BT_CLOSE);
            bool waiting = PageWaiting();
            int pb = PtInRect(&rcClose, pt) ? BT_CLOSE : waiting ? BT_NONE : PageButtonAt(rx, ry);
            int option = waiting ? -1 : OptionAt(rx, ry);
            if (pb != m_nHoverBtn || option != m_nHoverOption) {
                if (pb != BT_NONE && pb != m_nHoverBtn) play_ui_sound(L"BtMouseOver");
                m_nHoverBtn = pb;
                m_nHoverOption = option;
                InvalidateRect(nullptr);
            }
            return 0;
        }
        int any = ButtonAt(rx, ry);
        int bt = any;
        if (bt != BT_NONE && !Enabled(bt)) bt = BT_NONE;
        bool retry = InRetry(rx, ry);
        RowHit hit = HitRow(rx, ry);
        std::string hover = hit.row ? hit.row->id : std::string();
        if (bt != m_nHoverBtn || any != m_nHoverAny || retry != m_bHoverRetry || hover != m_hoverRow) {
            if ((bt != BT_NONE && bt != m_nHoverBtn) || (retry && !m_bHoverRetry)) play_ui_sound(L"BtMouseOver");
            m_nHoverBtn = bt;
            m_nHoverAny = any;
            m_bHoverRetry = retry;
            m_hoverRow = hover;
            InvalidateRect(nullptr);
        }
        return 0;
    }

    virtual int OnMouseWheel(int rx, int ry, int nWheel) override {
        if (this != ms_pInstance) return 0;
        if (g_page.valid) {
            m_nPageScroll += nWheel > 0 ? -kLineH * 2 : kLineH * 2;
            ClampPageScroll();
            m_nHoverOption = OptionAt(rx, ry);
            InvalidateRect(nullptr);
            return 1;
        }
        if (m_nTab == TAB_CAMP) {
            m_nBenchScroll += nWheel > 0 ? -1 : 1;
            ClampScroll();
            InvalidateRect(nullptr);
        }
        return 1;
    }

    virtual void OnMouseEnter(int bEnter) override {
        CWnd::OnMouseEnter(bEnter);
        m_bMouseInside = bEnter != 0;
        if (!bEnter && (m_nHoverBtn != BT_NONE || m_nHoverAny != BT_NONE || m_bHoverRetry || !m_hoverRow.empty()
                        || m_nHoverOption >= 0 || m_nHoverTab >= 0)) {
            m_nHoverTab = -1;
            m_nHoverBtn = BT_NONE;
            m_nHoverAny = BT_NONE;
            m_bHoverRetry = false;
            m_hoverRow.clear();
            m_nHoverOption = -1;
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
            if (g_nSavedLeft != INT_MIN && g_nSavedTop != INT_MIN) { // kept for the next start too
                char sx[16], sy[16];
                _snprintf_s(sx, sizeof(sx), _TRUNCATE, "%d", g_nSavedLeft);
                _snprintf_s(sy, sizeof(sy), _TRUNCATE, "%d", g_nSavedTop);
                WritePrivateProfileStringA("crew", "x", sx, kpaths::ConfigPath());
                WritePrivateProfileStringA("crew", "y", sy, kpaths::ConfigPath());
            }
            g_nSavedTab = m_nListTab;
            WritePrivateProfileStringA("crew", "tab", m_nListTab == TAB_CAMP ? "camp" : "crew", kpaths::ConfigPath());
            g_bLastClickOurs = false;
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
    s.party.push_back(SampleRow("halvard", L"Halvard", L"Lv52 Crusader", L"Perion - HP 90%", 90, ROW_BENCH | ROW_SWAP_OUT | ROW_OUT));
    s.party.push_back(SampleRow("casse", L"Casse", L"Lv48 Priest", L"at the store - HP 100%", 100, ROW_BENCH | ROW_SWAP_OUT | ROW_OUT));
    s.party.push_back(SampleRow("kestrel", L"Kestrel", L"Lv50 Ranger", L"on the way (0:40)", 255, 0));
    s.bench.push_back(SampleRow("lioren", L"Lioren", L"Lv47 Wizard", L"train @ Ant Tunnel I - 12% EXP banked", 255, ROW_CALL | ROW_SWAP_IN | ROW_CAN_COME));
    s.bench.push_back(SampleRow("icecookie", L"IceCookie", L"Lv45 Cleric", L"farm @ Sleepywood - 143/200 Stolen Fence - ready to level - 3,120 meso, 4 stacks", 255, ROW_CALL | ROW_SWAP_IN | ROW_CAN_COME));
    s.bench.push_back(SampleRow("pell", L"Pell", L"Lv44 Bandit", L"away: rest @ Perion - resting", 255, ROW_CALL | ROW_SWAP_IN | ROW_CAN_COME));
    s.bench.push_back(SampleRow("rurik", L"Rurik", L"Lv43 Page", L"sell - out of supplies", 255, ROW_CALL | ROW_SWAP_IN | ROW_CAN_COME));
    s.bench.push_back(SampleRow("fenna", L"Fenna", L"Lv41 Assassin", L"gather @ Kerning City", 255, ROW_CALL | ROW_SWAP_IN | ROW_CAN_COME));
    return s;
}

void OpenWindow() {
    if (CCrewWnd::ms_pInstance) return;
    g_page = Page();
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
        // the server answers the lower of its protocol and ours: 1 (More opens the NPC Crew window) or 2 (pages)
        g_nProtocol = protocol >= 1 && protocol <= kProtocol ? protocol : 0;
        g_bEnabled = g_nProtocol != 0 && enabled != 0;
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
        crew_log("CLOSE: the server turned the Crew window off");
        g_bEnabled = false;
        CloseWindow(false);
        break;
    case SV_OPEN:
        OpenWindow();
        break;
    case SV_PAGE: {
        Page page;
        if (g_nProtocol < 2 || !ReadPage(r, page)) {
            crew_log("bad PAGE (%u bytes)", static_cast<unsigned int>(n));
            return;
        }
        if (!CCrewWnd::ms_pInstance) {
            SendPageReply(page.stamp, kReplyClose, -1); // the window closed meanwhile: end the page, as End Chat does
            return;
        }
        CCrewWnd::ms_pInstance->OnPage(std::move(page));
        break;
    }
    case SV_PAGE_CLOSE: {
        int stamp = r.i32();
        if (!r.ok) return;
        if (CCrewWnd::ms_pInstance) {
            CCrewWnd::ms_pInstance->OnPageClose(stamp);
        } else if (g_page.valid && (stamp == 0 || stamp == g_page.stamp)) {
            g_page = Page();
        }
        break;
    }
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

// server_packets hands over 0x17A and each SET_FIELD (serverpackets.cpp owns the ProcessPacket hook).
void OnCrewRecv(const unsigned char* p, size_t n) {
    if (!GuardedCrewPacket(p, n)) crew_log("exception while reading a crew packet");
}

void OnCrewSetField() {
    if (!g_bHelloSent) {
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
    bool bPress = msg == WM_KEYDOWN && !(lParam & 0xC0000000);
    // Esc: only while the mouse is over the window or it was the last window clicked, and never while an edit box has
    // the focus, so the game keeps Esc everywhere else
    if (bPress && wParam == VK_ESCAPE && CCrewWnd::ms_pInstance && NoControlFocused(pThis)
        && (CCrewWnd::ms_pInstance->m_bMouseInside || g_bLastClickOurs)) {
        CloseWindow(true);
        return 0;
    }
    if (bPress && wParam == g_uHotkeyVk && !(g_bEnabled || g_bSelfTest)) {
        // the window opens only after the server's WELCOME said on; the client has no verified way to print a chat
        // line, so the reason goes to the log
        static DWORD s_dwLogged = 0;
        DWORD now = GetTickCount();
        if (s_dwLogged == 0 || now - s_dwLogged > 60000) {
            s_dwLogged = now ? now : 1;
            crew_log("the Crew key %c was pressed, but this server has not turned the Crew window on%s",
                     static_cast<char>(g_uHotkeyVk), g_bHelloSent ? "" : " (not in the game yet)");
        }
    }
    if (bPress && wParam == g_uHotkeyVk && (g_bEnabled || g_bSelfTest)) {
        int scan = (lParam >> 16) & 0xFF;
        static int s_nLogged = 0;
        if (s_nLogged < 5) {
            ++s_nLogged;
            crew_log("hotkey lParam=%08X scan=%d focus-free=%d unbound=%d", static_cast<unsigned int>(lParam), scan,
                     NoControlFocused(pThis) ? 1 : 0, KeyUnbound(scan) ? 1 : 0);
        }
        static bool s_bBoundLogged = false;
        if (NoControlFocused(pThis) && !KeyUnbound(scan) && !s_bBoundLogged) {
            s_bBoundLogged = true;
            crew_log("the Crew key %c is set to something else in the game's key settings, so it does not open "
                          "the Crew window. Clear that key there, or pick another key with [config] crew_hotkey= "
                          "in config.ini.", static_cast<char>(g_uHotkeyVk));
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

// Every frame: the open window's timeouts.
void OnCrewFrame() {
    if (CCrewWnd::ms_pInstance) CCrewWnd::ms_pInstance->Tick();
}

// Each message: a left click forgets that the window was the last one clicked; the window's own OnMouseButton, which
// the same click reaches after this, sets it again when the click was on the window.
bool OnCrewMessage(UINT uMsg, WPARAM, LPARAM, LRESULT*) {
    if (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN) g_bLastClickOurs = false;
    return false;
}

// ---- teardown ---------------------------------------------------------------------------------------------------

void OnCrewLeaveGame() {
    CloseWindow(false);
    g_bEnabled = false;
    g_nProtocol = 0;
    g_snap = Snapshot();
    g_page = Page();
}

void OnCrewClearFieldUI() {
    CloseWindow(true);
}

// config.ini next to MapleStory.exe (the working directory is not always the game folder)
std::string ConfigPath() {
    return kpaths::ConfigPath();
}

// [config] crew_hotkey: exactly one letter or digit (any case). Anything else is logged and U is used.
UINT ReadHotkey(const std::string& config) {
    char buf[32] = {};
    DWORD n = GetPrivateProfileStringA("config", "crew_hotkey", "U", buf, sizeof(buf), config.c_str());
    char ch = n == 1 ? buf[0] : '\0';
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return static_cast<UINT>(ch);
    crew_log("config.ini [config] crew_hotkey=\"%s\" is not one letter or digit; using U instead", buf);
    return 'U';
}

// [crew] x, y: where the window was last closed. A missing or unreadable pair leaves the window centred.
void ReadSavedPosition(const std::string& config) {
    char sx[32] = {}, sy[32] = {};
    DWORD nx = GetPrivateProfileStringA("crew", "x", "", sx, sizeof(sx), config.c_str());
    DWORD ny = GetPrivateProfileStringA("crew", "y", "", sy, sizeof(sy), config.c_str());
    char tab[16] = {};
    GetPrivateProfileStringA("crew", "tab", "", tab, sizeof(tab), config.c_str());
    g_nSavedTab = _stricmp(tab, "camp") == 0 ? TAB_CAMP : TAB_CREW;
    if (nx == 0 && ny == 0) return;
    char* ex = nullptr;
    char* ey = nullptr;
    long x = strtol(sx, &ex, 10);
    long y = strtol(sy, &ey, 10);
    if (nx == 0 || ny == 0 || ex == sx || ey == sy || *ex || *ey || x < -10000 || x > 10000 || y < -10000 || y > 10000) {
        crew_log("config.ini [crew] x=\"%s\" y=\"%s\" is not a position; the window opens centred instead", sx, sy);
        return;
    }
    g_nSavedLeft = static_cast<int>(x); // clamped to the screen when the window opens
    g_nSavedTop = static_cast<int>(y);
}

}  // namespace


// Called by bypass.cpp's CClientSocket::Connect hook: a new connection has not said HELLO yet.
void CrewWindowOnConnect() {
    g_bHelloSent = false;
    g_bEnabled = false;
    g_nProtocol = 0;
    g_page = Page(); // the new connection's server has no page open
    if (CCrewWnd::ms_pInstance) CCrewWnd::ms_pInstance->InvalidateRect(nullptr);
}

// Switched by config.ini [features] crew_window (or the older [config] crew_window), see feature_list.h.
void AttachCrewWindowMod() {
    std::string config = ConfigPath();
    if (!v83::CheckClientVersion()) {
        crew_log("off: MapleStory.exe is not the build these offsets come from");
        return;
    }
    g_uHotkeyVk = ReadHotkey(config);
    ReadSavedPosition(config);
    g_bSelfTest = GetPrivateProfileIntA("config", "crew_selftest", 0, config.c_str()) != 0;
    serverpackets::OnRecv(kRecvOpcode, OnCrewRecv);
    serverpackets::OnSetField(OnCrewSetField);
    ATTACH_HOOK(CWndMan__ProcessKey, CWndMan__ProcessKey_hook);
    clientloop::OnLeaveGame(OnCrewLeaveGame);
    clientloop::OnClearFieldUI(OnCrewClearFieldUI);
    clientloop::OnFrame(OnCrewFrame);
    clientloop::OnMessage(OnCrewMessage);
    g_bHooksOn = true;
    crew_log("on, hotkey %c%s", static_cast<char>(g_uHotkeyVk), g_bSelfTest ? ", self-test" : "");
}
