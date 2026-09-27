#include "pch.h"
#include "hook.h"
#include "debug.h"

// Bidirectional bounce stubs — overwrite both X edges to ±5000 from
// source position so the rect spans symmetrically regardless of the
// caller-provided direction flag at [ebp+0x14] (which v62/v83 use to
// pick forward vs backward bounce direction).
//
// The rect-builder's two branches both converge at the Y-bottom lea,
// each with one fixed edge (=esi) and one offset edge. We hook there
// to override BOTH edges with ±5000 spans, then re-emit the Y-bottom
// lea (overwritten by the 5-byte jmp) and continue.

#if defined(KAENTAKE_TARGET_v62)
static const uintptr_t kCL_BidirReturn = 0x005D570B;  // after Y-bottom lea
void __declspec(naked) ChainLightning_Bidir_hook() {
    __asm {
        lea ecx, [esi - 9C4h]      // left = esi - 2500
        mov [ebp-4], ecx
        lea eax, [esi + 9C4h]      // right = esi + 2500
        lea ecx, [edi + 0FAh]       // re-emit Y-bottom = edi + 250
        jmp [kCL_BidirReturn]
    }
}
#elif defined(KAENTAKE_TARGET_v83)
static const uintptr_t kCL_BidirReturn = 0x006788F4;  // after Y-bottom lea
void __declspec(naked) ChainLightning_Bidir_hook() {
    __asm {
        lea ecx, [esi - 9C4h]
        mov [ebp-4], ecx
        lea eax, [esi + 9C4h]
        lea ecx, [edi + 0FAh]
        jmp [kCL_BidirReturn]
    }
}
#endif

// Chain Lightning damage falloff + bounce range buff.
//
// v83 dropoff table at 0x00BDB470 (geometric r=0.7):
//   1.0, 0.7, 0.49, 0.343, 0.2401, 0.16807
// v62 dropoff table at 0x00968858 (geometric r=0.5):
//   1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125
// Both consumed by `fld qword ptr [eax*8 + table]` in the chain-skill
// dispatcher (v83 0x75BFF1, v62 0x64CFEA). Skills using this table
// include 0x21E3CE (Chain Lightning, ID 2221006 — Ice/Lightning Mage
// 3rd job) and 0x4E23EA (5121002 / 5120042).
//
// NOTE: User-provided ID 2121006 (=0x205D2E) is incorrect — that's a
// different skill. Real Chain Lightning is 2221006 = 0x21E3CE per
// disasm of both binaries.
//
// Range patches alter the bounce-mob search rectangle:
//   v83 0x67886D rect-builder: X-range from caller arg (push 0x96 at
//     0x955EF0); Y-range inlined as ±0x96 at 0x6788EE/0x6788F6.
//   v62 0x5D5682 rect-builder: ALL 4 edges inlined as ±0x96 (no
//     caller-supplied arg). Patches go directly into the lea/add
//     imm32 displacements within the rect-builder body.
//
// Patching the dropoff table affects ALL chain-bounce skills that
// share it (Chain Lightning + 5121002 + a couple others). Behavior
// difference is negligible — they're already chain-bounce skills.

static void WriteDouble(uintptr_t addr, double v) {
    auto bytes = reinterpret_cast<const uint8_t*>(&v);
    for (int i = 0; i < 8; ++i) {
        Patch1(addr + i, bytes[i]);
    }
}

static void WriteInt32(uintptr_t addr, int32_t v) {
    auto bytes = reinterpret_cast<const uint8_t*>(&v);
    for (int i = 0; i < 4; ++i) {
        Patch1(addr + i, bytes[i]);
    }
}

void AttachExtChainLightning() {
#if defined(KAENTAKE_TARGET_v62)
    DEBUG_MESSAGE("AttachExtChainLightning: v62 — flat damage + 2500h/250v range");
    // 1. No-dropoff: 6 doubles -> 1.0 each
    for (int i = 0; i < 6; ++i) {
        WriteDouble(0x00968858 + i * 8, 1.0);
    }
    // 2. Range patches in 0x5D5682 bounce-rect builder
    WriteInt32(0x005D56C9 + 2, -2500);  // X-left  (lea ecx, [esi-0x96])
    WriteInt32(0x005D56F9 + 2,  2500);  // X-right (lea ecx, [esi+0x96])
    // 0x005D5705 (Y-bottom) is OVERWRITTEN by the bidir PatchJmp below;
    // the stub re-emits Y-bottom with the +250 value before jumping back.
    // So skip the WriteInt32 at +2 — would be clobbered anyway.
    WriteInt32(0x005D570D + 2,  -250);  // Y-top   (add edi, -0x96)
    // Bidirectional bounce: PatchJmp at the rect-builder convergence
    // (Y-bottom lea). Stub forces both X edges to esi ±2500 regardless
    // of caller's direction flag, re-emits Y-bottom, then continues.
    // PatchJmp covers 5 bytes of the 6-byte `lea ecx, [edi+0x96]`; the
    // 6th byte (offset 0x005D570A) is unreachable since we redirect.
    PatchJmp(0x005D5705, &ChainLightning_Bidir_hook);
#elif defined(KAENTAKE_TARGET_v83)
    DEBUG_MESSAGE("AttachExtChainLightning: v83 — flat damage + 2500h/250v range");
    for (int i = 0; i < 6; ++i) {
        WriteDouble(0x00BDB470 + i * 8, 1.0);
    }
    // NOTE: this push imm32 is the X-range arg the rect-builder reads, but
    // the bidir stub below overrides it unconditionally. Kept in sync with
    // the stub's literal so a future "drop bidir" revert still has the
    // right caller-pushed value.
    WriteInt32(0x00955EF0 + 1, 2500);   // push imm32 horizontal range
    // 0x006788EE Y-bottom imm32 OVERWRITTEN by bidir PatchJmp below;
    // stub re-emits Y-bottom = 250 before jumping back.
    WriteInt32(0x006788F6 + 2, -250);   // Y-top   add
    // Bidirectional bounce: PatchJmp at rect-builder convergence
    // (Y-bottom lea at 0x006788EE). Stub forces both X edges to
    // esi ±2500 regardless of caller's direction flag.
    PatchJmp(0x006788EE, &ChainLightning_Bidir_hook);
#else
    DEBUG_MESSAGE("AttachExtChainLightning: no target — skipping");
#endif
}
