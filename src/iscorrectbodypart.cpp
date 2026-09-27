#include "pch.h"
#include "hook.h"

// -----------------------------------------------------------------------------
// is_correct_bodypart rewrite (detour)
// -----------------------------------------------------------------------------
// Client function:
//   int __cdecl is_correct_bodypart(int nItemID, int nBodyPart, int nGender) @ 0x00460358
// Goal:
//   Add category 115 -> treat as nBodyPart == 20.
static auto is_correct_bodypart = reinterpret_cast<int(__cdecl*)(int, int, int)>(0x00460358);

static bool is_weapon_category(int v4) {
    const int itemCategory = v4;
    const int categoryTens = itemCategory / 10;
    return categoryTens == 13 || categoryTens == 14 || categoryTens == 16 || categoryTens == 17;
}

static int is_correct_bodypart_category_182(int nBodyPart) {
    if (nBodyPart == 21 || nBodyPart == 31)
        return 1;
    return nBodyPart == 39;
}

static int __cdecl is_correct_bodypart_hook(int nItemID, int nBodyPart, int nGender) {
    const int itemCategory = nItemID / 10000;

    switch (itemCategory) {
        case 119:
            return nBodyPart == 10;
        case 100:
            return nBodyPart == 1;
        case 101:
            return nBodyPart == 2;
        case 102:
            return nBodyPart == 3;
        case 103:
            return nBodyPart == 4;
        case 104:
        case 105:
            return nBodyPart == 5;
        case 106:
            return nBodyPart == 6;
        case 107:
            return nBodyPart == 7;
        case 108:
            return nBodyPart == 8;
        case 109:
            return nBodyPart == 10;
        case 110:
            return nBodyPart == 9;
        case 111:
            if (nBodyPart == 12 || nBodyPart == 13 || nBodyPart == 15)
                return 1;
            return nBodyPart == 16;
        case 112:
            if (nBodyPart == 17)
                return 1;
            return nBodyPart == 51;
        case 113:
            return nBodyPart == 50;
        case 114:
            return nBodyPart == 49;
        case 115:
            return nBodyPart == 20;
        case 180:
            // Special-case: 1802100 follows the same rules as category 182.
            if (nItemID == 1802100)
                return is_correct_bodypart_category_182(nBodyPart);

            if (nBodyPart == 14 || nBodyPart == 30)
                return 1;
            return nBodyPart == 38;

        case 181:
            switch (nItemID) {
                case 1812000:
                    if (nBodyPart == 23 || nBodyPart == 34)
                        return 1;
                    return nBodyPart == 42;
                case 1812001:
                    if (nBodyPart == 22 || nBodyPart == 33)
                        return 1;
                    return nBodyPart == 41;
                case 1812002:
                    return nBodyPart == 24;
                case 1812003:
                    return nBodyPart == 25;
                case 1812004:
                    if (nBodyPart == 26 || nBodyPart == 35)
                        return 1;
                    return nBodyPart == 43;
                case 1812005:
                    if (nBodyPart == 27 || nBodyPart == 36)
                        return 1;
                    return nBodyPart == 44;
                case 1812006:
                    if (nBodyPart == 28 || nBodyPart == 37)
                        return 1;
                    return nBodyPart == 45;
                case 1812007:
                    if (nBodyPart == 46 || nBodyPart == 47)
                        return 1;
                    return nBodyPart == 48;
                default:
                    return 0;
            }

        case 182:
            return is_correct_bodypart_category_182(nBodyPart);
        case 183:
            if (nBodyPart == 29 || nBodyPart == 32)
                return 1;
            return nBodyPart == 40;
        case 190:
            return nBodyPart == 18;
        case 191:
            return nBodyPart == 19;
        // case 192:
        //     return nBodyPart == 20;
        case 194:
            return nBodyPart == 1000;
        case 195:
            return nBodyPart == 1001;
        case 196:
            return nBodyPart == 1002;
        case 197:
            return nBodyPart == 1003;
        default:
            if (!is_weapon_category(itemCategory))
                return 0;
            return nBodyPart == 11;
    }
}

void AttachIsCorrectBodyPartFix() {
    ATTACH_HOOK(is_correct_bodypart, is_correct_bodypart_hook);
}
