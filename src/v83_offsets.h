#pragma once
//
// Central table of MapleStory v83 memory offsets referenced by Kaentake's
// patches.
//
// Previously these lived as inline hex literals scattered across the mod
// files. They are stable across v83.x builds of the GMS client, but when
// the client binary changes at all (re-linked, different loader, different
// patch set) every literal silently rots and patches corrupt random memory.
// Collecting them here makes it possible to:
//   (1) diff the set against a new binary,
//   (2) guard the DLL against loading on the wrong client (see
//       v83_version_guard.h), and
//   (3) document what each offset actually patches.
//
// When adding new offsets: put them in the right subsystem namespace, give
// them a descriptive name, and drop a one-line comment explaining what the
// site is (disassembly context, what instruction, what it controls).
//

#include <cstdint>
#include <cstddef>

namespace v83 {

// ---------------------------------------------------------------------------
// Version guard fixtures. Populated by inspecting your v83 client once with
// the DLL attached (look for "[v83] MapleStory.exe SizeOfImage=..." lines in
// DebugView when _DEBUG is enabled). Leaving these at 0 disables the guard --
// the DLL will still load on any client, which is the pre-existing behavior.
//
// Update both when you know the value for your client. A single mismatch is
// fine to leave informational; two mismatches almost certainly means a wrong
// client and the DLL will refuse to attach.
//
// How to find them:
//   1. Build Kaentake in Debug mode and attach.
//   2. Watch DebugView for "[v83] MapleStory.exe SizeOfImage=0x.... TimeDateStamp=0x....".
//   3. Paste those values here, rebuild.
// ---------------------------------------------------------------------------
constexpr uint32_t kExpectedImageSize     = 0x00A92000;  // IMAGE_OPTIONAL_HEADER::SizeOfImage (BotStory LAA exe)
constexpr uint32_t kExpectedTimeDateStamp = 0x4B7C15C9;  // IMAGE_FILE_HEADER::TimeDateStamp (same as the stock exe)

namespace skills {
// Big Bang (Bishop / I-L Arch Mage / F-P Arch Mage) remove-charge caves.
// Each pair is a (hook-site, post-cave return-site). The naked stubs in
// addiesKaentakeCompile.cpp `je` into kBigBangTarget on charge, else fall
// through to the *_Return address to replay the displaced bytes.
constexpr uintptr_t kBigBangBishopHook      = 0x00967ECC;
constexpr uintptr_t kBigBangBishopReturn    = 0x00967ED2;
constexpr uintptr_t kBigBangIceLightningHook   = 0x00967DEE;
constexpr uintptr_t kBigBangIceLightningReturn = 0x00967DF4;
constexpr uintptr_t kBigBangFirePoisonHook     = 0x00967D10;
constexpr uintptr_t kBigBangFirePoisonReturn   = 0x00967D16;
constexpr uintptr_t kBigBangTarget             = 0x0096928B;
}  // namespace skills

namespace stats {
// Sites where the v83 client re-loads the 3-byte bounds table
// { max_low, max_high, field_index } for stat clamping. Patching each site
// with { 0xFF, 0xFE, 12 } widens the clamp so raw stat values larger than the
// vanilla cap still transmit cleanly.
//
// These are loaded as an immediate-operand right after the opcode byte, so
// each patched address is (site + 1) -- the +1 is baked into the table so
// callers don't have to remember it.
constexpr uintptr_t kUncapStatSites[] = {
    0x00780620 + 1,
    0x0077E055 + 1,
    0x0077E12F + 1,
    0x0077E215 + 1,
    0x0078FF5F + 1,
    0x0078E061 + 1,
    0x0078E67D + 1,
    0x0079166C + 1,
    0x00791CD5 + 1,
    0x007918FC + 1,
    0x007806D0 + 1,
    0x00780702 + 1,
};
constexpr size_t kUncapStatSiteCount =
    sizeof(kUncapStatSites) / sizeof(kUncapStatSites[0]);

// Replacement clamp table. 0xFFFE widens low/high bounds; 12 keeps the same
// field index the original code uses.
constexpr unsigned char kUncapStatTable[] = { 0xFF, 0xFE, 12 };
}  // namespace stats

// ---------------------------------------------------------------------------
// Quest window list rows (questcolor.cpp): CUIQuestInfo::Draw picks each unselected quest row's font.
namespace quest {
constexpr uintptr_t kQuestWndDraw      = 0x00883126;  // CUIQuestInfo::Draw (vtable 0xB3A9EC slot 11)
constexpr uintptr_t kQuestIdRead       = 0x00883FC9;  // mov eax,[ebp-0x2C]; mov ax,[eax]: the row's quest id
constexpr uintptr_t kQuestPickFontSite = 0x00884090;  // test esi,esi; ...; lea ecx grey/black font
constexpr uintptr_t kQuestFontCall     = 0x008840AC;  // call kComPtrGet -> font for DrawText (patched)
constexpr uintptr_t kComPtrGet         = 0x004284A5;  // IWzFontPtr getter: mov eax,[ecx]; ret
constexpr uintptr_t kGetBasicFont      = 0x0098A707;  // get_basic_font(IWzFontPtr*, type)
constexpr int kFontBlue = 4;                          // Arial 12, 0xFF2000FF (type 1 is the same in black)
constexpr unsigned int kResidentQuestMin = 30000;     // living-world quest-log ids (server quest-log-ids.yaml)
constexpr unsigned int kResidentQuestMax = 32767;
}  // namespace quest

// ---------------------------------------------------------------------------
// Native Crew window (crewwnd.cpp). From an unpacked dump of the shipped build (2026-10-08 crash dump image).
// Only the Crew window hooks check the fingerprint above; nothing else calls the guard.
namespace crew {
constexpr unsigned short kRecvOpcode     = 0x17A;       // server -> client (server SendOpcode.CREW_WINDOW)
constexpr unsigned short kSendOpcode     = 0x117;       // client -> server (server RecvOpcode.CREW_WINDOW)
constexpr unsigned short kSetFieldOpcode = 0x7D;        // SET_FIELD: HELLO follows the first one of a connection
constexpr int kProtocol = 1;
constexpr int kDllBuild = 20261012;
constexpr uintptr_t kProcessPacket        = 0x004965F1; // CClientSocket::ProcessPacket(CInPacket&), from ManipulatePacket 0x004965C4
constexpr uintptr_t kSendPacket           = 0x0049637B; // CClientSocket::SendPacket(const COutPacket&)
constexpr uintptr_t kClientSocketInstance = 0x00BE7914; // TSingleton<CClientSocket>::ms_pInstance
constexpr uintptr_t kWvsContextInstance   = 0x00BE7918; // TSingleton<CWvsContext>::ms_pInstance
constexpr uintptr_t kContextOnKey         = 0x00A07431; // CWvsContext key handler, as CUIWnd::OnKey (0x0092C6EE) forwards
constexpr uintptr_t kProcessKey           = 0x009E40F1; // CWndMan::ProcessKey(msg, wParam, lParam), from CWvsApp::ISMsgProc
constexpr uintptr_t kWndManFocus          = 0x88;       // CWndMan: the focused IUIMsgHandler (CWndMan+4 = none)
constexpr uintptr_t kFuncKeyMappedMan     = 0x00BED5A0; // TSingleton<CFuncKeyMappedMan>::ms_pInstance (ctor 0x0058DD0D)
constexpr uintptr_t kFuncKeyMappedOffset  = 0x4;        // FUNCKEY_MAPPED[89] { char nType; long nID; }
constexpr int kFuncKeySlots = 89;
constexpr uintptr_t kOnLeaveGame          = 0x00A041FF; // CWvsContext::OnLeaveGame
constexpr uintptr_t kClearFieldUI         = 0x00A04920; // CWvsContext::ClearFieldUI (Cash Shop / MTS stage Init)
constexpr uintptr_t kRttiCWnd             = 0x00BF1608; // CWnd::ms_RTTI (CUIWnd's 0x00BF11DC points here)
constexpr uintptr_t kGetBasicFont         = 0x0098A707; // get_basic_font(IWzFontPtr*, type)
constexpr uintptr_t kPlayUiSound          = 0x00989588; // play_ui_sound(L"leaf"), prefixes Sound.wz/UI.img/
constexpr int kFontBlack = 1;      // 12px 0xFF000000
constexpr int kFontDarkGrey = 2;   // 12px 0xFF404040
constexpr int kFontLightGrey = 17; // 12px 0xFFBBBBBB
constexpr int kFontBlackBold = 29; // 12px 0xFF000000, style SP 0x583
}  // namespace crew

}  // namespace v83
