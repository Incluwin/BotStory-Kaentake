// Ported from https://gist.github.com/Andre-3001/d7b0d5c17cf36856f820c6fae8cf1022
//
// Enlarge Quickslot Bar: 8 slots -> 26 slots (2 rows x 13 columns).
// ---------------------------------------------------------------------------
// Memory::* primitives (Ezorsia framework) mapped to Kaentake equivalents:
//     Memory::WriteByte(a, v)   -> Patch1(a, v)
//     Memory::WriteInt(a, v)    -> Patch4(a, v)
//     Memory::FillBytes(a,v,n)  -> FillBytes(a, v, n)  [helper below]
//     Memory::CodeCave(fn,a,n)  -> CodeCave(a, fn, n)  [helper below]
// ---------------------------------------------------------------------------
// Kaentake's widescreen patches already touch a few of these addresses. The
// resolution.cpp patch at 0x008CFD50+1 writes 1920 (same as this mod wants),
// so we skip the duplicate. GetShortCutIndexByPos is ATTACH_HOOKed by
// resolution.cpp; Detours only patches the prologue so byte patches inside
// the function body (0x008DE8F4+1, 0x008DE926+1) still apply and take effect
// when the Kaentake hook calls through to the original. tempstat.cpp hooks
// DrawSkillCooltime at 0x008E085B and returns to 0x008E0990 -- our patches
// at 0x008E069D, 0x008E06A3 (before the hook) and 0x008E099F+3 (after the
// return point) are outside the hijacked span.
//
// KNOWN GAP: the original gist references a 5th code cave
// `Restore_Array_Expanded` at 0x008CFDFD without defining it. Without the
// body we can't install it; since cooldown_Array is initialized to 0xFF, the
// common "skill cooldown resets properly" path still works. If you notice
// stale cooldown overlays on the new slots after a skill lapses, this is the
// missing piece -- would need a disassembly of the original 6 bytes at
// 0x008CFDFD to reconstruct.
// ---------------------------------------------------------------------------
#include "pch.h"
#include "hook.h"
#include <windows.h>
#include <cstring>


// ============================================================================
// Helpers: Memory::FillBytes and Memory::CodeCave equivalents.
// ============================================================================
static void FillBytes(uintptr_t addr, unsigned char value, size_t count) {
    void* buf = malloc(count);
    memset(buf, value, count);
    PatchMemory(reinterpret_cast<void*>(addr), buf, count);
    free(buf);
}

template <typename Fn>
static void CodeCave(uintptr_t addr, Fn fn, size_t totalSize) {
    PatchJmp(addr, fn);              // 5-byte E9 <rel32>
    if (totalSize > 5) {
        FillBytes(addr + 5, 0x90, totalSize - 5);
    }
}


// ============================================================================
// Arrays. These MUST live at file scope so their addresses are stable.
// ============================================================================

// s_aDefaultQKM (27 entries x 4 bytes) -- default quickslot key map
static unsigned char Array_aDefaultQKM[] = {
    42, 0, 0, 0,
    82, 0, 0, 0,
    71, 0, 0, 0,
    73, 0, 0, 0,
    2,  0, 0, 0,
    3,  0, 0, 0,
    4,  0, 0, 0,
    5,  0, 0, 0,
    6,  0, 0, 0,
    30, 0, 0, 0,
    31, 0, 0, 0,
    32, 0, 0, 0,
    33, 0, 0, 0,
    29, 0, 0, 0,
    83, 0, 0, 0,
    79, 0, 0, 0,
    81, 0, 0, 0,
    16, 0, 0, 0,
    17, 0, 0, 0,
    18, 0, 0, 0,
    19, 0, 0, 0,
    20, 0, 0, 0,
    44, 0, 0, 0,
    45, 0, 0, 0,
    46, 0, 0, 0,
    47, 0, 0, 0,
    52, 0, 0, 0
};

// s_ptShortKeyPos -- 26 (x,y) pairs for the slot hitboxes/draw positions.
// 13 columns (x: 7, 42, 77, 112, 147, 182, 217, 252, 287, 322, 357, 392, 427)
// x 2 rows (y: 8 for top row, 41 for bottom row). Stored as 32-bit LE.
// Cols 9..13 (>= 256) use explicit two-byte form so we don't rely on modular
// wrap of an `unsigned char` literal -- silences MSVC C4309/C4838 warnings.
static unsigned char Array_ptShortKeyPos[] = {
    // top row, y = 8
    7,    0,    0, 0,   8,  0, 0, 0,
    42,   0,    0, 0,   8,  0, 0, 0,
    77,   0,    0, 0,   8,  0, 0, 0,
    112,  0,    0, 0,   8,  0, 0, 0,
    147,  0,    0, 0,   8,  0, 0, 0,
    182,  0,    0, 0,   8,  0, 0, 0,
    217,  0,    0, 0,   8,  0, 0, 0,
    252,  0,    0, 0,   8,  0, 0, 0,
    0x1F, 0x01, 0, 0,   8,  0, 0, 0,   // 287
    0x42, 0x01, 0, 0,   8,  0, 0, 0,   // 322
    0x65, 0x01, 0, 0,   8,  0, 0, 0,   // 357
    0x88, 0x01, 0, 0,   8,  0, 0, 0,   // 392
    0xAB, 0x01, 0, 0,   8,  0, 0, 0,   // 427
    // bottom row, y = 41
    7,    0,    0, 0,   41, 0, 0, 0,
    42,   0,    0, 0,   41, 0, 0, 0,
    77,   0,    0, 0,   41, 0, 0, 0,
    112,  0,    0, 0,   41, 0, 0, 0,
    147,  0,    0, 0,   41, 0, 0, 0,
    182,  0,    0, 0,   41, 0, 0, 0,
    217,  0,    0, 0,   41, 0, 0, 0,
    252,  0,    0, 0,   41, 0, 0, 0,
    0x1F, 0x01, 0, 0,   41, 0, 0, 0,   // 287
    0x42, 0x01, 0, 0,   41, 0, 0, 0,   // 322
    0x65, 0x01, 0, 0,   41, 0, 0, 0,   // 357
    0x88, 0x01, 0, 0,   41, 0, 0, 0,   // 392
    0xAB, 0x01, 0, 0,   41, 0, 0, 0    // 427
};

// s_aDefaultQKM_0 (alternate default key map used by a second init path)
static unsigned char Array_aDefaultQKM_0[] = {
    42, 0, 0, 0, 82, 0, 0, 0, 71, 0, 0, 0, 73, 0, 0, 0,   // 0..3
    29, 0, 0, 0, 83, 0, 0, 0, 79, 0, 0, 0, 81, 0, 0, 0,   // 4..7
    42, 0, 0, 0, 82, 0, 0, 0, 71, 0, 0, 0, 73, 0, 0, 0,   // 8..11
    29, 0, 0, 0, 83, 0, 0, 0, 79, 0, 0, 0, 81, 0, 0, 0,   // 12..15
    84, 0, 0, 0, 85, 0, 0, 0, 86, 0, 0, 0, 87, 0, 0, 0,   // 16..19
    88, 0, 0, 0, 89, 0, 0, 0, 29, 0, 0, 0, 29, 0, 0, 0,   // 20..23
    29, 0, 0, 0, 29, 0, 0, 0, 29, 0, 0, 0                  // 24..26
};

// Expanded keymap blob (312 bytes: 26 entries x 12-byte record)
static unsigned char Array_Expanded[312] = {
    4, 4,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 1,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 2,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 3,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 5,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 6,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 7,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 8,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 10, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 11, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 12, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 13, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 14, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 15, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 16, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 17, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 23, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 24, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 25, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 26, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    4, 27, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    5, 50, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    5, 51, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    5, 52, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    5, 53, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
};

// Skill-cooldown shadow array: 0xFF = no cooldown. 26 entries x 4 bytes + pad.
static unsigned char cooldown_Array[124] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF
};


// ============================================================================
// Code caves (naked trampolines). Addresses & assembly taken verbatim from
// the gist -- only `_declspec` normalized to `__declspec` and `jmp <var>`
// replaced with the explicit `push imm; ret` form that the gist author left
// as the documented alternative. x86 only; Kaentake is 32-bit so this is fine.
// ============================================================================

static unsigned int Array_aDefaultQKM_Address       = reinterpret_cast<unsigned int>(&Array_aDefaultQKM);
// (The gist also exports Array_mystery_Address[+1] and cooldown_Array_Address
//  but only Array_aDefaultQKM_Address is referenced from inside the caves.)

__declspec(naked) static void CompareValidateFuncKeyMappedInfo_cave() {
    __asm {
        push 0x138
        push 0x0
        push eax
        pushad
        popad
        push 0x8DD8BD
        ret
    }
}

__declspec(naked) static void sub_9FA0CB_cave() {
    __asm {
        test eax, eax
        jne  label
        push 0xD4
        pushad
        popad
        // -> ZAllocEx<ZAllocAnonSelector>::Alloc(_s_alloc, 0x44u);
        push 0x9FA0E1
        ret
    label:
        push 0x138
        push 0x0
        push eax
        pushad
        popad
        // -> memset(this + 0xD20, 0, 0x60u);
        push 0x8DD8BD
        ret
    }
}

__declspec(naked) static void sDefaultQuickslotKeyMap_cave() {
    __asm {
        push ebx
        push esi
        push edi
        xor  edx, edx
        mov  ebx, ecx
        call ret_label
        nop
        lea  edi, dword ptr ds:[ebx + 0x4]
        mov  ecx, 0x1A
        mov  esi, Array_aDefaultQKM_Address
        rep  movsd
        lea  edi, dword ptr ds:[ebx + 0x6C]
        mov  ecx, 0x1A
        mov  esi, Array_aDefaultQKM_Address
        rep  movsd
        pop  edi
        pop  esi
        pop  ebx
        ret
    ret_label:
        push esi
        mov  esi, ecx
        lea  eax, dword ptr ds:[esi + 0x4]
        // -> _DWORD* __fastcall sub_72B7BC(_DWORD* a1)
        push 0x72B7C2
        ret
    }
}

__declspec(naked) static void DefaultQuickslotKeyMap_cave() {
    __asm {
        push esi
        push edi
        lea  eax, dword ptr ds:[ecx + 0x4]
        mov  esi, Array_aDefaultQKM_Address
        mov  ecx, 0x1A
        mov  edi, eax
        rep  movsd
        pop  edi
        pop  esi
        ret
    }
}


// ============================================================================
// Entry point -- called from hook.h::AttachClientHooks()
// ============================================================================
void AttachEnlargeQuickslotMod() {

    // -------- Kaentake-specific quickbar hit-region patches ---------------
    // From the user's note: "Without these, hovering/clicking on quickslots
    // does nothing". These extend the mouse-event bounds to cover the full
    // 1920x1058 client area so clicks on the new rightmost slots register.
    //
    // NB: the gist ALSO prescribes six BAR POSITION patches
    //     (0x008D1793, 0x008D179A, 0x008DF782, 0x008DF7F8, 0x008DE8EE,
    //      0x008DE8E5). Those are omitted intentionally -- they were
    //     designed for Ezorsia, which positions the quickslot widget via
    //     hardcoded offsets in code. Kaentake instead installs a custom 2D
    //     origin (CWndMan::ms_pOrgQuickSlot, see resolution.cpp line 236)
    //     that repositions the bar dynamically. Applying both on top of
    //     each other double-shifts the anchor and pushes the whole bar --
    //     including the original 8 slots -- off-screen at any resolution.
    // ----------------------------------------------------------------------
    // 0x008CFD50+1 is already patched to 1920 by resolution.cpp -- skipped.
    Patch4(0x008CFD4B + 1, 1080 - 22);
    Patch4(0x008D1D50 + 1, 1080 - 22);
    Patch4(0x008D1D55 + 1, 1920);
    Patch4(0x008D1FF4 + 1, 1080 - 22);
    Patch4(0x008D1FF9 + 1, 1920);
    Patch4(0x008D82F5 + 1, 1080 - 22);
    Patch4(0x008D82FA + 1, 1920);

    // -------- CUIStatusBar::OnCreate (draw extra slots/hotkeys/cooldowns) --
    Patch1(0x008D155C + 1, 0xF0); // Draw rest of quickslot bar (1008 = 0x3F0)
    Patch1(0x008D155C + 2, 0x03);
    Patch1(0x008D182E + 1, 0xF0); // Draw rest of hotkeys
    Patch1(0x008D182E + 2, 0x03);
    Patch1(0x008D1AC0 + 1, 0xF0); // Draw rest of cooldowns
    Patch1(0x008D1AC0 + 2, 0x03);

    // -------- CQuickslotKeyMappedMan::CQuickslotKeyMappedMan ---------------
    Patch4(0x0072B7CE + 1, reinterpret_cast<uintptr_t>(&Array_aDefaultQKM_0));
    Patch4(0x0072B8EB + 1, reinterpret_cast<uintptr_t>(&Array_aDefaultQKM_0));

    // -------- CUIStatusBar::CQuickSlot::CompareValidateFuncKeyMappedInfo ---
    Patch1(0x008DD916, 0x1A);                                     // cmp 8 -> cmp 26
    Patch1(0x008DD8AD, 0x1A);                                     // cmp 8 -> cmp 26
    Patch1(0x008DD8FD, 0xBB);                                     // mov ebx, imm32
    Patch4(0x008DD8FD + 1, reinterpret_cast<uintptr_t>(&Array_Expanded));
    Patch1(0x008DD8FD + 5, 0x90);                                 // errant byte -> nop
    Patch1(0x008DD898, 0xB8);                                     // mov eax, imm32
    Patch4(0x008DD898 + 1, reinterpret_cast<uintptr_t>(&Array_Expanded));
    Patch1(0x008DD898 + 5, 0x90);                                 // errant byte -> nop

    // -------- CUIStatusBar::CQuickSlot::Draw -------------------------------
    Patch1(0x008DE75E + 3, 0x6C);                                 // struct offset bump
    Patch1(0x008DDF99, 0xB8);                                     // mov eax, imm32
    Patch4(0x008DDF99 + 1, reinterpret_cast<uintptr_t>(&Array_Expanded));
    FillBytes(0x008DDF99 + 5, 0x90, 3);                           // nop the stale operands

    // -------- CUIStatusBar::OnMouseMove ------------------------------------
    Patch1(0x008D7F1E + 1, 0x34);
    Patch1(0x008D7F1E + 2, 0x85);
    Patch4(0x008D7F1E + 3, reinterpret_cast<uintptr_t>(&Array_Expanded));

    // -------- CUIStatusBar::CQuickSlot::GetPosByIndex ----------------------
    Patch4(0x008DE94D + 2, reinterpret_cast<uintptr_t>(&Array_ptShortKeyPos));
    Patch4(0x008DE955 + 2, reinterpret_cast<uintptr_t>(&Array_ptShortKeyPos) + 4);
    Patch1(0x008DE941 + 2, 0x1A);                                 // cmp 8 -> cmp 26

    // -------- CUIStatusBar::GetShortCutIndexByPos --------------------------
    // (Kaentake's ATTACH_HOOK at 0x008DE8D5 only overwrites the prologue.
    //  These body-level patches still apply.)
    Patch4(0x008DE8F4 + 1, reinterpret_cast<uintptr_t>(&Array_ptShortKeyPos) + 4);
    Patch1(0x008DE926 + 1, 0x3E);

    // -------- CUIStatusBar::CQuickSlot::DrawSkillCooltime ------------------
    Patch1(0x008E099F + 3, 0x1A);                                 // cmp 8 -> cmp 26
    Patch1(0x008E069D, 0xBE);                                     // mov esi, imm32
    Patch4(0x008E069D + 1, reinterpret_cast<uintptr_t>(&cooldown_Array));
    Patch1(0x008E069D + 5, 0x90);                                 // errant byte
    Patch1(0x008E06A3, 0xBF);                                     // mov edi, imm32
    Patch4(0x008E06A3 + 1, reinterpret_cast<uintptr_t>(&Array_Expanded) + 1);
    Patch1(0x008E06A3 + 5, 0x90);                                 // errant byte

    // -------- CDraggableMenu --------------------------------------------
    Patch1(0x004F928A + 2, 0x1A); // OnDropped        cmp 8 -> cmp 26
    Patch1(0x004F93F9 + 2, 0x1A); // MapFuncKey       cmp 8 -> cmp 26

    // -------- CUIKeyConfig::OnDestroy --------------------------------------
    Patch1(0x00833797 + 2, 0x6C); // struct offset 0x24 -> 0x6C (triple)
    Patch1(0x00833841 + 2, 0x6C);
    Patch1(0x00833791 + 1, 0x68); // push 0x68 (triple)
    Patch1(0x0083383B + 1, 0x68);

    // -------- CUIKeyConfig::~CUIKeyConfig ----------------------------------
    Patch1(0x0083287F + 2, 0x6C);
    Patch1(0x00832882 + 1, 0x68);

    // -------- CQuickslotKeyMappedMan::SaveQuickslotKeyMap ------------------
    Patch1(0x0072B8C0 + 2, 0x6C);
    Patch1(0x0072B8A0 + 1, 0x68);
    Patch1(0x0072B8BD + 1, 0x68);

    // -------- CQuickslotKeyMappedMan::OnInit -------------------------------
    Patch1(0x0072B861 + 1, 0x68);
    Patch1(0x0072B867 + 2, 0x6C);

    // -------- CUIKeyConfig::CNoticeDlg::OnChildNotify ----------------------
    Patch1(0x00836A1E + 1, 0x68);
    Patch1(0x00836A21 + 2, 0x6C);

    // -------- Code caves ---------------------------------------------------
    CodeCave(0x008DD8B8, &CompareValidateFuncKeyMappedInfo_cave, 5);
    CodeCave(0x009FA0DB, &sub_9FA0CB_cave,                        5);
    CodeCave(0x0072B7BC, &sDefaultQuickslotKeyMap_cave,           5);
    CodeCave(0x0072B8E6, &DefaultQuickslotKeyMap_cave,            5);
    // NOTE: 5th cave Restore_Array_Expanded @ 0x008CFDFD is intentionally
    //       omitted -- the gist declares it but never defines the body.
    //       See the file header for behavioural impact (minor).
}
