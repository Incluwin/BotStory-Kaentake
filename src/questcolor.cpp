#include "pch.h"
#include "hook.h"
#include "v83_offsets.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include "ztl/zcom.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Draws the quest window's list rows of living-world Resident quests (ids 30000-32767, the server's
// quest-log mirror) in blue. The v83 list prints colour codes in QuestInfo names literally, so the
// colour has to come from the font CUIQuestInfo::Draw picks for the row.
//
// Inside CUIQuestInfo::Draw (0x00883126), for an unselected quest row (headers and the selected row
// take other paths), the client picks black or grey ("out-levelled", Available tab only):
//   0x00884090  test esi, esi                 ; esi = grey
//               lea  ecx, [ebp-0x4C]          ; grey font
//               jnz  +3
//               lea  ecx, [ebp-0x48]          ; black font
//               ...
//   0x008840AC  call 0x004284A5               ; IWzFontPtr getter: mov eax,[ecx]; ret
//   0x008840B1  push eax                      ; the font DrawText (0x004277AD) uses
// The row item is [ebp-0x2C]; its first word is the quest id (the client reads it so at 0x00883FC9).
// The call is replaced with QuestListFont_stub, which returns the client's font unless the row is a
// Resident quest that is not grey. Offsets come from an unpacked dump of the same build; the bytes
// are checked before patching and the patch is skipped (logged to kaentake_diag.txt) on any mismatch.

using namespace v83::quest;

static auto get_basic_font = reinterpret_cast<IWzFontPtr*(__cdecl*)(IWzFontPtr*, int)>(kGetBasicFont);

// One reference held for the life of the process: never released, so no COM call at static teardown.
static IWzFont* g_pResidentFont = nullptr;
static bool g_bResidentFontFailed = false;

static void qc_log(const char* fmt, ...) {
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

static IWzFont* __stdcall PickQuestRowFont(IWzFont** ppChosen, int bGrey, const unsigned short* pItem) {
    IWzFont* pDefault = *ppChosen; // exactly what the replaced getter returns
    if (bGrey || !pItem) {
        return pDefault;
    }
    unsigned int nQuestId = *pItem;
    if (nQuestId < kResidentQuestMin || nQuestId > kResidentQuestMax) {
        return pDefault;
    }
    if (!g_pResidentFont && !g_bResidentFontFailed) {
        try {
            IWzFontPtr pFont;
            get_basic_font(std::addressof(pFont), kFontBlue);
            g_pResidentFont = pFont.Detach();
        } catch (...) {
        }
        if (!g_pResidentFont) {
            g_bResidentFontFailed = true; // never retried per frame
            qc_log("[questcolor] blue font unavailable; Resident rows stay black\n");
        }
    }
    return g_pResidentFont ? g_pResidentFont : pDefault;
}

// Replaces `call 0x004284A5` at 0x008840AC: ecx = the chosen IWzFontPtr*, esi = grey, ebp = Draw's frame.
// eax/ecx/edx are free; esi/ebp are preserved by the __stdcall helper.
__declspec(naked) static void QuestListFont_stub() {
    __asm {
        push    dword ptr [ebp - 0x2C]
        push    esi
        push    ecx
        call    PickQuestRowFont
        ret
    }
}

void AttachQuestListColorMod() {
    // 0x00884090..0x008840B0: the font pick and the getter call being replaced
    static const unsigned char kPick[] = {
        0x85, 0xF6, 0xC6, 0x45, 0xFC, 0x39, 0x8D, 0x4D, 0xB4, 0x75, 0x03, 0x8D, 0x4D, 0xB8,
        0x8D, 0x85, 0x40, 0xFF, 0xFF, 0xFF, 0x50, 0x8D, 0x85, 0x50, 0xFF, 0xFF, 0xFF, 0x50,
        0xE8, 0xF4, 0x43, 0xBA, 0xFF,
    };
    static const unsigned char kIdRead[] = {0x8B, 0x45, 0xD4, 0x66, 0x8B, 0x00}; // mov eax,[ebp-0x2C]; mov ax,[eax]
    static const unsigned char kGetter[] = {0x8B, 0x01, 0xC3};                  // mov eax,[ecx]; ret
    if (memcmp(TO_PVOID(kQuestPickFontSite), kPick, sizeof(kPick)) != 0
            || memcmp(TO_PVOID(kQuestIdRead), kIdRead, sizeof(kIdRead)) != 0
            || memcmp(TO_PVOID(kComPtrGet), kGetter, sizeof(kGetter)) != 0) {
        qc_log("[questcolor] byte mismatch at the quest list font pick; not patched\n");
        return;
    }
    PatchCall(kQuestFontCall, &QuestListFont_stub);
    qc_log("[questcolor] patched 0x%08X\n", kQuestFontCall);
}
