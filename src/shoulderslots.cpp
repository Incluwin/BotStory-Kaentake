#include "pch.h"
#include "hook.h"

// -----------------------------------------------------------------------------
// Shoulder Slots / Equip data path fix
// -----------------------------------------------------------------------------
// Client function: `get_equip_data_path` @ 0x005C94A1
// Patch: change `cmp eax, 72h` (114) -> `cmp eax, 73h` (115)
// Instruction: 83 F8 72 at 0x005C973A, immediate byte at +2.
static constexpr DWORD kGetEquipDataPath_cmp_v2_114_imm = 0x005C973A + 2;

void AttachShoulderSlotsFix() {
    // Only changes the immediate; the conditional jump remains correct.
    Patch1(kGetEquipDataPath_cmp_v2_114_imm, 0x73);
}
