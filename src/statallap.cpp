#include "pch.h"
#include "hook.h"
#include "features.h"
#include "v83_offsets.h"
#include "wvs/ctrlwnd.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

// All-AP buttons in the stat window (stat_all_ap), ported from LoganML/kaentake's statmax.cpp (FreedomStory).
//
// Right of the AP up arrow of STR, DEX, INT and LUK a second button (red, double arrow) puts ALL remaining AP into
// that stat after a Yes/No. It sends the client's own auto-assign packet (CWvsContext::SendAPAutoAssign) with that
// stat and a second one at 0 AP; the server checks it like any auto-assign (AssignAPProcessor, the client's split
// while USE_SERVER_AUTOASSIGNER is off).
//
// The window is 14 px wider to fit the buttons: width 176 -> 190, the close, AUTO-ASSIGN and DETAIL buttons and the
// detail panel move right with it. The art is Custom.wz UI/UIWindow.img/StatApMax (tools/make_stat_ap_art.py): the
// background goes in as the UOL CUIStat::OnCreate hands CUIWnd::OnCreate (normally empty, which means the stock
// Stat/backgrnd), so the stock art is never overridden. The layout bytes are patched when the first stat window
// opens, once the art is known to be there; a Custom.wz without it leaves the stock window.

using namespace v83::statwnd;

namespace {

constexpr int kWiden = 14;
constexpr unsigned int kBtId = 2010; // after the window's own 2000-2009
constexpr int kStatFlag[4] = {0x40, 0x80, 0x100, 0x200}; // STR, DEX, INT, LUK
const char* const kStatName[4] = {"STR", "DEX", "INT", "LUK"};
const wchar_t kBackgrndUol[] = L"Custom/UI/UIWindow.img/StatApMax/backgrnd";
const wchar_t kButtonUol[] = L"Custom/UI/UIWindow.img/StatApMax/BtApMax";

enum class Art { unchecked, ready, missing };
Art g_eArt = Art::unchecked;
ZRef<CCtrlButton> g_aBtMax[4];

auto decode_secure_short = reinterpret_cast<short(__cdecl*)(void*, int)>(kDecodeSecureShort);
auto util_dlg_yes_no = reinterpret_cast<int(__cdecl*)(ZXString<char>, int, int)>(kUtilDlgYesNo);
auto send_ap_auto_assign = reinterpret_cast<void(__thiscall*)(void*, void*)>(kSendApAutoAssign);

void sa_log(const char* fmt, ...) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, "kaentake_diag.txt", "a") != 0 || !fp) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fclose(fp);
}

bool has_object(const wchar_t* sPath) {
    try {
        Ztl_variant_t v = get_rm()->GetObjectA(Ztl_bstr_t(sPath));
        return v.GetUnknown() != nullptr;
    } catch (...) {
        return false;
    }
}

// The bytes the wider layout changes: {instruction, its opcode, immediate size, old immediate, new immediate}.
struct LayoutSite {
    uintptr_t uAddress;
    unsigned char bOp;
    size_t uImm;
    unsigned int uOld;
    unsigned int uNew;
};

std::vector<LayoutSite> layout_sites() {
    std::vector<LayoutSite> v = {
        {kWidthPush, 0x68, 4, 0xB0, 0xB0 + kWiden},
        {kClosePush, 0x68, 4, 0x9C, 0x9C + kWiden},
        {kBackgrndPush, 0x68, 4, kEmptyWide, 0}, // uNew: kBackgrndUol, set below
        {kDetailAnd, 0x25, 4, static_cast<unsigned int>(-0xAA), static_cast<unsigned int>(-(0xAA + kWiden))},
    };
    v[2].uNew = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(kBackgrndUol));
    for (uintptr_t u : kDetailAdd) {
        v.push_back({u, 0x05, 4, 0xAA, 0xAA + kWiden});
    }
    for (uintptr_t u : kAutoBtPush) {
        v.push_back({u, 0x6A, 1, 0x61, 0x61 + kWiden});
    }
    return v;
}

// The DETAIL button's `push 0x144; push 0x7C`: a push imm8 is sign-extended, so x 0x7C + 14 = 0x8A would be -118
// (off the window). The two pushes become a call here; eax and edx are live, ecx is free.
const unsigned char kDetailBtPushes[] = {0x68, 0x44, 0x01, 0x00, 0x00, 0x6A, 0x7C};

__declspec(naked) void detail_button_pos() {
    __asm {
        pop     ecx              // return address
        push    0x144            // y, as before
        push    0x8A             // x: 0x7C + kWiden (a C++ name here would be read as a memory operand)
        jmp     ecx
    }
}
static_assert(0x7C + kWiden == 0x8A, "detail_button_pos pushes the DETAIL button's x");
static_assert(0x61 + kWiden < 0x80, "the AUTO-ASSIGN x stays a push imm8");

bool layout_bytes_match() {
    for (const auto& s : layout_sites()) {
        const auto* p = reinterpret_cast<const unsigned char*>(s.uAddress);
        unsigned int uImm = 0;
        memcpy(&uImm, p + 1, s.uImm);
        if (p[0] != s.bOp || uImm != s.uOld) {
            sa_log("[stat_all_ap] byte mismatch at 0x%08X; not attached\n", static_cast<unsigned>(s.uAddress));
            return false;
        }
    }
    if (memcmp(TO_PVOID(kDetailBtPos), kDetailBtPushes, sizeof(kDetailBtPushes)) != 0) {
        sa_log("[stat_all_ap] byte mismatch at 0x%08X; not attached\n", static_cast<unsigned>(kDetailBtPos));
        return false;
    }
    return true;
}

// First stat window: the wider layout if Custom.wz has the art, recorded under this module.
void prepare_layout() {
    if (g_eArt != Art::unchecked) {
        return;
    }
    if (!has_object(kBackgrndUol) || !has_object(L"Custom/UI/UIWindow.img/StatApMax/BtApMax/normal/0")) {
        g_eArt = Art::missing;
        sa_log("[stat_all_ap] Custom.wz has no UI/UIWindow.img/StatApMax; the stat window stays stock\n");
        return;
    }
    features::AttachScope scope(FeatureId::stat_all_ap);
    for (const auto& s : layout_sites()) {
        if (s.uImm == 1) {
            Patch1(s.uAddress + 1, static_cast<unsigned char>(s.uNew));
        } else {
            Patch4(s.uAddress + 1, s.uNew);
        }
    }
    PatchCall(kDetailBtPos, &detail_button_pos, sizeof(kDetailBtPushes));
    g_eArt = Art::ready;
}

int get_ap() {
    auto pContext = *reinterpret_cast<uintptr_t*>(kWvsContextInstance);
    if (!pContext) {
        return 0;
    }
    auto pCd = *reinterpret_cast<uintptr_t*>(pContext + kCharacterData);
    if (!pCd) {
        return 0;
    }
    return decode_secure_short(reinterpret_cast<void*>(pCd + kCharacterDataAp),
                               *reinterpret_cast<int*>(pCd + kCharacterDataAp + 4));
}

// IUIMsgHandler part (+4), vtable +0x1C = Enable(int): what EnableButtons does for the up arrows
void set_enable(CCtrlButton* pBt, bool bEnable) {
    uintptr_t pHandler = reinterpret_cast<uintptr_t>(pBt) + 4;
    void** vt = *reinterpret_cast<void***>(pHandler);
    reinterpret_cast<void(__thiscall*)(uintptr_t, int)>(vt[0x1C / 4])(pHandler, bEnable ? 1 : 0);
}

void enable_buttons() {
    bool bEnable = get_ap() > 0;
    for (auto& bt : g_aBtMax) {
        if (bt) {
            set_enable(bt, bEnable);
        }
    }
}

void release_buttons() {
    for (auto& bt : g_aBtMax) {
        bt = nullptr;
    }
}

void create_buttons(void* pStat) {
    for (int i = 0; i < 4; i++) {
        if (g_aBtMax[i]) {
            continue;
        }
        CCtrlButton::CREATEPARAM param;
        param.sUOL = kButtonUol;
        g_aBtMax[i] = new CCtrlButton();
        CCtrlButton* pBt = g_aBtMax[i];
        // CreateCtrl at vtable +0x20, as ResetButtons creates the up arrows (0x008C7B2A)
        void** vt = *reinterpret_cast<void***>(pBt);
        reinterpret_cast<void(__thiscall*)(CCtrlButton*, void*, unsigned int, int, int, int, void*)>(vt[0x20 / 4])(
                pBt, pStat, kBtId + i, kBtApUpX + kWiden, kBtApUpY[i], 1, &param);
    }
    enable_buttons();
}

void on_all_ap_clicked(int nStat) {
    int nAp = get_ap();
    if (nAp <= 0) {
        return;
    }
    char sMsg[160];
    sprintf_s(sMsg, "Put all %d AP into %s?", nAp, kStatName[nStat]);
    if (util_dlg_yes_no(ZXString<char>(sMsg), 0, 1) != 6) {
        return;
    }
    nAp = get_ap(); // may have changed while the dialog was open
    void* pContext = *reinterpret_cast<void**>(kWvsContextInstance);
    if (nAp <= 0 || !pContext) {
        return;
    }
    // ZArray<pair<long, long>>: the data pointer with the count just before it. The server reads two pairs, so the
    // second is another stat with 0 AP.
    int nOther = nStat == 0 ? 1 : 0;
    int aBuff[5] = {2, kStatFlag[nStat], nAp, kStatFlag[nOther], 0};
    void* pArray = &aBuff[1];
    send_ap_auto_assign(pContext, &pArray);
}

}  // namespace


class CUIStat {
public:
    MEMBER_HOOK(void*, kCtor, Constructor, int nArg)
    MEMBER_HOOK(void, kDtor, Destructor)
    MEMBER_HOOK(void, kResetButtons, ResetButtons)
    MEMBER_HOOK(void, kEnableButtons, EnableButtons)
    MEMBER_HOOK(void, kOnButtonClicked, OnButtonClicked, unsigned int nId)
};

void* CUIStat::Constructor_hook(int nArg) {
    prepare_layout();
    return CUIStat::Constructor(this, nArg);
}

void CUIStat::Destructor_hook() {
    CUIStat::Destructor(this);
    release_buttons();
}

void CUIStat::ResetButtons_hook() {
    CUIStat::ResetButtons(this);
    if (g_eArt != Art::ready) {
        return;
    }
    // the STR up arrow is there unless the window is in its Beginner layout
    bool bUpArrows = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(this) + kBtApUp + 2 * 8 + 4) != 0;
    if (bUpArrows) {
        create_buttons(this);
    } else {
        release_buttons();
    }
}

void CUIStat::EnableButtons_hook() {
    CUIStat::EnableButtons(this);
    enable_buttons();
}

void CUIStat::OnButtonClicked_hook(unsigned int nId) {
    if (nId >= kBtId && nId < kBtId + 4) {
        on_all_ap_clicked(static_cast<int>(nId - kBtId));
        return;
    }
    CUIStat::OnButtonClicked(this, nId);
}


void AttachStatAllApMod() {
    if (!layout_bytes_match()) {
        return;
    }
    ATTACH_HOOK(CUIStat::Constructor, CUIStat::Constructor_hook);
    ATTACH_HOOK(CUIStat::Destructor, CUIStat::Destructor_hook);
    ATTACH_HOOK(CUIStat::ResetButtons, CUIStat::ResetButtons_hook);
    ATTACH_HOOK(CUIStat::EnableButtons, CUIStat::EnableButtons_hook);
    ATTACH_HOOK(CUIStat::OnButtonClicked, CUIStat::OnButtonClicked_hook);
}
