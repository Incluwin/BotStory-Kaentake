#pragma once
//
// Runtime check that the loaded MapleStory.exe matches the v83 build our
// hardcoded offsets were written for. Call once from DllMain before any
// patches go in.
//
// Behaviour depends on the constants in v83_offsets.h:
//   * If kExpectedImageSize == 0 AND kExpectedTimeDateStamp == 0, the guard
//     logs the actual values via DebugMessage and returns -- useful for
//     recording what your client reports so you can populate the constants.
//   * If either is non-zero, the actual value is compared. A full mismatch
//     (both non-zero and both differ) shows a MessageBox and calls
//     ExitProcess to avoid corrupting memory with wrong-offset patches.
//
// This is best-effort, not tamper resistant: anyone who can inject a DLL
// can bypass it. The goal is preventing accidental runs on the wrong
// client, not stopping a determined modder.
//

namespace v83 {

// Returns true if the client matches (or the guard is disabled/ambiguous).
bool CheckClientVersion();

// Hard-fail entry point. Calls CheckClientVersion; on failure, shows a
// MessageBox describing the mismatch and calls ExitProcess(1).
void EnsureClientVersionOrExit();

}  // namespace v83
