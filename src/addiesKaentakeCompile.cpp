#include "pch.h"
#include "hook.h"
#include "constants.h"
#include "v83_offsets.h"
#include <windows.h>

namespace {

// =============================================================
// Big Bang - Remove Charge
// Constants live in v83_offsets.h so they can be checked against the client
// fingerprint alongside the rest of the v83 offset table.
// =============================================================
using v83::skills::kBigBangBishopHook;
using v83::skills::kBigBangBishopReturn;
using v83::skills::kBigBangIceLightningHook;
using v83::skills::kBigBangIceLightningReturn;
using v83::skills::kBigBangFirePoisonHook;
using v83::skills::kBigBangFirePoisonReturn;
using v83::skills::kBigBangTarget;

__declspec(naked) void BB_Bish_Cave() {
    __asm {
        je      DoJump
        jmp     [kBigBangBishopReturn]
    DoJump:
        jmp     [kBigBangTarget]
    }
}

__declspec(naked) void BB_IL_Cave() {
    __asm {
        je      DoJump
        jmp     [kBigBangIceLightningReturn]
    DoJump:
        jmp     [kBigBangTarget]
    }
}

__declspec(naked) void BB_FP_Cave() {
    __asm {
        je      DoJump
        jmp     [kBigBangFirePoisonReturn]
    DoJump:
        jmp     [kBigBangTarget]
    }
}

} // namespace


void AttachFreeAddy() {
	
    // Force Window Mode
	// Credits stelmo
    unsigned char patchWindow[4] = { 0xB8, 0x00, 0x00, 0x00 };
    PatchMemory(TO_PVOID(0x009F7A9B), patchWindow, 4);

    // Remove Logo start-up animation
	// Credits Denki
    PatchNop(0x0062EE54, 0x0062EE54 + 21);

    // Typable PIN
    PatchNop(0x004CA8BA, 0x004CA8BA + 2);

    // Repeat same thing more than 3x (ChatSpam) & Infinite ChatBox
    Patch1(0x004905EB, 0xEB);
    Patch1(0x004CAA09, 0xEB);
    Patch1(0x004CAA84, 0xEB);
    
    //Remove "Repeating the same line over and over\r\ncan negatively affect other users." check allow spam text
    Patch1(0x00490607, 0xEB);
    Patch1(0x00490609, 0x27);
    
    //Remove "Too much chatting can disrupt\r\nother players' ability to play the game." check allow spam text
    Patch1(0x00490651, 0xEB);
    Patch1(0x00490652, 0x1D);
    
    // Remove "You may not use this skill yet" message
	// Remove "This card is already full" message
	// Remove Swear Filter & ?? message
    PatchNop(0x00967707, 0x00967707 + 12);
    PatchNop(0x00A08283, 0x00A08283 + 18);
    PatchNop(0x007A03C8, 0x007A03C8 + 2);

    // Super Tubi
    PatchNop(0x00485C01, 0x00485C01 + 2);
    PatchNop(0x00485C21, 0x00485C21 + 2);
    PatchNop(0x00485C32, 0x00485C32 + 2);

    // Damage Cap
	// Credits Rain
    double dDamageCap = CONSTANTS_DAMAGE_CAP;
    int nDamageCap = static_cast<int>(dDamageCap < 0 ? dDamageCap - 0.5 : dDamageCap + 0.5);
    PatchMemory(TO_PVOID(0x00AFE8A0), &dDamageCap, sizeof(double));
    Patch4(0x008C3304 + 1, nDamageCap);

    // Speed Cap
	// Credits Ronan
    Patch4(0x00780743 + 3, CONSTANTS_SPEED_CAP);
    Patch4(0x008C4286 + 1, CONSTANTS_SPEED_CAP);
    Patch4(0x0094D91E + 1, CONSTANTS_SPEED_CAP);

    //Ladder/Climb Speed
    Patch4(0x009CC6F9 + 2, 0x00C1CF80);
    double dLadderSpeed = 8.0; // Address Speed Control
    PatchMemory(TO_PVOID(0x00C1CF80), &dLadderSpeed, sizeof(double));

    // No Breath
	// Credits Rulax
    Patch1(0x00452316, 0x7C);

    // Jump Tele
	// Credits Rulax
    PatchNop(0x00957C2D, 0x00957C2D + 6);
	
	// Jump Attack Mage / Shoot
	// Credits Teto
    Patch1(0x009539FA, 0xEB);
    Patch1(0x009559E5, 0xEB);

    // Move While Skill
    Patch1(0x0095F97A, 0xEB);
    Patch1(0x0095F97A + 1, 0x59);
    Patch1(0x009CBFB0, 0xEB);
    PatchNop(0x0094C3BB, 0x0094C3BB + 6);

    // Uncap Stats
    // Credits Rulax/Pepa
    // Site table and replacement bytes live in v83::stats; see v83_offsets.h.
    for (size_t i = 0; i < v83::stats::kUncapStatSiteCount; ++i) {
        PatchMemory(
            TO_PVOID(v83::stats::kUncapStatSites[i]),
            const_cast<unsigned char*>(v83::stats::kUncapStatTable),
            sizeof(v83::stats::kUncapStatTable));
    }

    // Increase Stat UI render
    // DISABLED: these widen rendering inside the CharacterStat canvases but do
    // not touch the underlying CWnd containers or the "Detail" sub-panel's
    // parent-relative anchor. Result: dragging the CHARACTER STAT window
    // causes the detail panel to drift out of place. Re-enable only if we
    // later patch the Detail wnd offset to match the new widths.
    //Patch1(0x008C510A + 1, 0xFF); //Increase number rendering of numbers X axis in StatDetail Window
    //Patch4(0x008C6C72 + 1, 0x000000E0);
    //Patch4(0x008C5760 + 1, 0x000000E0);
    //Patch4(0x008C4AB3 + 1, 0x00000105);

    // Genderless equips
	// Credits Eric
    PatchNop(0x00460AED, 0x00460AED + 2);

    // Close Range removed (i.e no whack on bow or claw.)
    Patch1(0x009516C2, 0xE9);
    Patch1(0x009516C2 + 1, 0xc8);
    Patch1(0x009516C2 + 2, 0xfc);
    Patch1(0x009516C2 + 3, 0xff);
    Patch1(0x009516C2 + 4, 0xff);

    // No Whack (secondary site)
    // 009698BC is a 6-byte "jnz rel32" (0F 85 disp32) to the skill shot at 0x00969A39.
    // A lone 0xEB made "jmp short -0x7B" into the no-ammo fallback (CLOSE skill=0 for every ammo skill),
    // so write nop + jmp rel32 and keep disp32: same target, now unconditional.
    Patch1(0x009698BC, 0x90);
    Patch1(0x009698BC + 1, 0xE9);

    // Hurricane / Rapid Fire no whack
    // Each site is a 6-byte "je rel32" (0F 84 disp32). Retarget disp32 from 0x0096AC43 -> 0x0096AEB5.
    // new_disp = newTarget - (site + 6)
    {
        auto patchJeTarget = [](uintptr_t site, uintptr_t newTarget) {
            Patch4(site + 2, (uint32_t)(newTarget - (site + 6)));
        };
        patchJeTarget(0x0096AA4E, 0x0096AEB5);
        patchJeTarget(0x0096AA59, 0x0096AEB5);
        patchJeTarget(0x0096AA64, 0x0096AEB5);
        patchJeTarget(0x0096AA6F, 0x0096AEB5);
    }

    // Lacking Level Check Removal
	// Credits Rulax
    Patch1(0x008AD01A, 0xE9);
    Patch4(0x008AD01A + 1, 0x008AD227 - (0x008AD01A + 5));

    // PetEquip Cap Removal
	// Credits Rulax
    Patch1(0x0046D43B, 0xE9);
    Patch4(0x0046D43B + 1, 0x0046D473 - (0x0046D43B + 5));

    // CItemInfo::EQUIPITEM::IsAbleToStickWithWeapon
	// Bypasses the bitmask check to allow weapon covers on any weapon
    PatchNop(0x0046D3CE, 0x0046D3CE + 2);

    // CUIStatusBar::ChatLogAdd simple max line count
    Patch1(0x008DB387 + 3, 0x7F);

    //Set Aran combo uptime to 7000 milliseconds [Need to adjust server side, AranHandler.java]
	// Credits Crabo
    Patch4(0x0094BDC8 + 1, 0x1B58);
    Patch4(0x00960708 + 1, 0x1B58);
    Patch4(0x0096095B + 1, 0x1B58);

    // BattleShip Rope Climb
	// 009CC11F --> Change Jz to jmp
	// Credits Rulax
    Patch1(0x009CC11F, 0xEB);
    Patch4(0x00936B2D + 6, 28);

    // Chain Lightning range + falloff + bidir bounce now lives in
    // ext_chainlightning.cpp::AttachExtChainLightning(). The new module
    // is a strict superset: same X/Y range edits, table-based no-falloff
    // (also covers Energy Orb 5121002), plus bidirectional bounce via
    // a PatchJmp at 0x006788EE.

    // No charge Piercing Arrow > also remove the loop sound from wz file otherwise your ear dies
	// Credits Rulax
	// Corkscrew Blow No Charge > Also remove the loop sound from wz file
	// Credits Rulax
    Patch1(0x00968048, 0xE9); Patch4(0x00968048 + 1, 0x07);
    Patch1(0x00968278, 0xE9); Patch4(0x00968278 + 1, 0x19);

    // Big Bang (BS/IL/FP) Remove Charge
    PatchJmp(kBigBangBishopHook, BB_Bish_Cave); // Bishop Big Bang
    PatchJmp(kBigBangIceLightningHook, BB_IL_Cave); // IceLightning Big Bang
    PatchJmp(kBigBangFirePoisonHook, BB_FP_Cave); // FirePoison Big Bang

    // Double Flash Jump (Uncomment to do Unlimited FJ)
	// Memory::FillBytes(0x009675A5, 0x90, 5); // nop 5
	// Memory::FillBytes(0x0096BEBC, 0x90, 2); // nop 2
	// Memory::FillBytes(0x0096BEC7, 0x90, 2); // nop 2
    PatchNop(0x0096BED2, 0x0096BED2 + 2);
    PatchNop(0x0096BEE2, 0x0096BEE2 + 2);
    PatchNop(0x0096BF59, 0x0096BF59 + 6);
    Patch1(0x0096BF86, 0xEB);
    PatchNop(0x0096BFAE, 0x0096BFAE + 6);

    // Boomerang Step In Air
    Patch4(0x00950B4D + 2, 0x00950C53 - (0x00950B4D + 6));

	// Assaulter without Target
    // Credit Crabo
    Patch1(0x00951347 + 1, 0x1C);
	
	// Rush without target
    // Credit Crabo
    PatchNop(0x00952E2F, 0x00952E2F + 6);

    // Assasination without Dark Sight
    Patch1(0x0096953E, 0xEB);
    Patch1(0x00952D9B, 0xEB);
    
	// Disables bonus damage to Assassinate from being in darksight
	// Give Assassinate Crit
    unsigned char jumpPatchAssassination[] = { 0xE9, 0xEE, 0x00, 0x00, 0x00, 0x90 };
    PatchMemory(TO_PVOID(0x0078EDCE), jumpPatchAssassination, sizeof(jumpPatchAssassination));
    Patch1(0x00790107, 0xEB);
    Patch1(0x00790296, 0xEB);
	
	// Allow Assassinate while airborne
    PatchNop(0x00950B42, 0x00950B42 + 6);
    Patch4(0x00951A32 + 1, 12); // Remove "Dark Sight" effect (12 = random skill ID)

    // Allow usage of pots while in Dark Sight skill
	// Allow double click pots while in Dark Sight skill
    PatchNop(0x0094F6AB, 0x0094F6AB + 6);
    PatchNop(0x004F0311, 0x004F0311 + 6);

    // Instant Final Attack (No Animation)
    Patch1(0x0095795E, 0x83);
    Patch1(0x0095795E + 1, 0xC0);
    Patch1(0x0095795E + 2, 0x00);
	
	// Maker Skill Instant (No Animation)
    Patch1(0x00826F92 + 2, 0x08);
    Patch1(0x00826F92 + 3, 0x01);
    Patch1(0x00826F92 + 4, 0x00);
    Patch1(0x00826F92 + 5, 0x00);

    // Monster Magnet fix on static enemies
    PatchNop(0x0096C554, 0x0096C554 + 4);

    // Screenshot Functionality
    int m_nGameWidth = get_screen_width();
    int m_nGameHeight = get_screen_height();
    Patch4(0x00744EB4 + 1, m_nGameWidth);
    Patch4(0x00744EB9 + 1, m_nGameHeight);
    Patch4(0x00744E2A + 1, 3 * m_nGameWidth * m_nGameHeight);
    Patch4(0x00744E43 + 1, m_nGameWidth * m_nGameHeight);
    Patch4(0x00744DA6 + 1, 4 * m_nGameWidth * m_nGameHeight);

    // Move Aran Combo and Skills to correct place
	// Credits Crabo
    Patch4(0x00960581 + 3, m_nGameWidth - 42);
    Patch4(0x00960839 + 1, m_nGameWidth - 42);
    Patch4(0x00960C67 + 1, m_nGameWidth - 10);
    Patch4(0x00960DED + 1, m_nGameWidth - 10);
}
