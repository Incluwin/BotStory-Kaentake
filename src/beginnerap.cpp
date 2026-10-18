#include "pch.h"
#include "hook.h"
#include "v83_offsets.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Beginners place AP at any level (beginner_ap). The stat window treats a Beginner job (0, 1000, 2000, 2001) at
// level 10 or below as one whose AP the server places: it deletes the up arrows and AUTO-ASSIGN, draws the "Basic
// Stats" layout and tooltip. Each of those checks is `cmp al, 0x0A; ja <normal>` on the level, so a limit of 0
// turns the lock off (the HeavenMS localhost has the same edit). Ported from LoganML/kaentake (FreedomStory).
//
// The BotStory server spends a Beginner's AP for them at every level-up (USE_AUTOASSIGN_STARTERS_AP); its
// server.mods switch beginner_ap leaves the AP to a player whose client reports this module active.

using namespace v83::statwnd;

static void ba_log(const char* fmt, ...) {
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

void AttachBeginnerApMod() {
    static const unsigned char kCmp[] = {0x3C, 0x0A, 0x59, 0x59}; // cmp al, 0x0A; pop ecx; pop ecx (then ja)
    for (uintptr_t uSite : kBeginnerLevelCmp) {
        if (memcmp(TO_PVOID(uSite), kCmp, sizeof(kCmp)) != 0) {
            ba_log("[beginner_ap] byte mismatch at 0x%08X; not patched\n", static_cast<unsigned>(uSite));
            return;
        }
    }
    for (uintptr_t uSite : kBeginnerLevelCmp) {
        Patch1(uSite + 1, 0x00); // cmp al, 0
    }
}
