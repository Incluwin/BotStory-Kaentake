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
constexpr uint32_t kExpectedImageSize     = 0;  // IMAGE_OPTIONAL_HEADER::SizeOfImage
constexpr uint32_t kExpectedTimeDateStamp = 0;  // IMAGE_FILE_HEADER::TimeDateStamp

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

}  // namespace v83
