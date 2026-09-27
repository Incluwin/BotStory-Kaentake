#include "pch.h"
#include "hook.h"
#include <initializer_list>

// -----------------------------------------------------------------------------
// get_bodypart_from_item rewrite (detour)
// -----------------------------------------------------------------------------
// Client function:
//   int __cdecl get_bodypart_from_item(int nItemID, int nGender, int *aBodyParts, int bWriteAll) @ 0x004606A0
static auto get_bodypart_from_item = reinterpret_cast<int(__cdecl*)(int, int, int*, int)>(0x004606A0);

static bool is_weapon_category(int itemCategory) {
    const int categoryTens = itemCategory / 10;
    return categoryTens == 13 || categoryTens == 14 || categoryTens == 16 || categoryTens == 17;
}

static int write_bodyparts(int* outBodyParts, bool writeAll, std::initializer_list<int> parts) {
    if (parts.size() == 0)
        return 0;

    outBodyParts[0] = *parts.begin();
    if (!writeAll)
        return 1;

    int i = 0;
    for (int part : parts)
        outBodyParts[i++] = part;
    return i;
}

static int write_bodyparts_category_182(int* outBodyParts, bool writeAll) {
    return write_bodyparts(outBodyParts, writeAll, {21, 31, 39});
}

static int __cdecl get_bodypart_from_item_hook(int nItemID, int nGender, int* aBodyParts, int bWriteAll) {
    const int itemCategory = nItemID / 10000;
    const bool writeAll = bWriteAll != 0;

    switch (itemCategory) {
        case 119:
        case 109:
            aBodyParts[0] = 10;
            return 1;
        case 100:
            aBodyParts[0] = 1;
            return 1;
        case 101:
            aBodyParts[0] = 2;
            return 1;
        case 102:
            aBodyParts[0] = 3;
            return 1;
        case 103:
            aBodyParts[0] = 4;
            return 1;
        case 104:
        case 105:
            aBodyParts[0] = 5;
            return 1;
        case 106:
            aBodyParts[0] = 6;
            return 1;
        case 107:
            aBodyParts[0] = 7;
            return 1;
        case 108:
            aBodyParts[0] = 8;
            return 1;
        case 110:
            aBodyParts[0] = 9;
            return 1;
        case 111:
            return write_bodyparts(aBodyParts, writeAll, {12, 13, 15, 16});
        case 112:
            return write_bodyparts(aBodyParts, writeAll, {17, 51});
        case 113:
            aBodyParts[0] = 50;
            return 1;
        case 114:
            aBodyParts[0] = 49;
            return 1;
        case 115:
            aBodyParts[0] = 20;
            return 1;
        case 180:
            // Special-case: 1802100 follows the same rules as category 182.
            if (nItemID == 1802100)
                return write_bodyparts_category_182(aBodyParts, writeAll);
            return write_bodyparts(aBodyParts, writeAll, {14, 30, 38});
        case 181:
            switch (nItemID) {
                case 1812000:
                    return write_bodyparts(aBodyParts, writeAll, {23, 34, 42});
                case 1812001:
                    return write_bodyparts(aBodyParts, writeAll, {22, 33, 41});
                case 1812002:
                    aBodyParts[0] = 24;
                    return 1;
                case 1812003:
                    aBodyParts[0] = 25;
                    return 1;
                case 1812004:
                    return write_bodyparts(aBodyParts, writeAll, {26, 35, 43});
                case 1812005:
                    return write_bodyparts(aBodyParts, writeAll, {27, 36, 44});
                case 1812006:
                    return write_bodyparts(aBodyParts, writeAll, {28, 37, 45});
                case 1812007:
                    // Note: original client writes the 3rd body-part at index 3 (skips index 2).
                    aBodyParts[0] = 46;
                    if (!writeAll)
                        return 1;
                    aBodyParts[1] = 47;
                    aBodyParts[3] = 48;
                    return 3;
                default:
                    return write_bodyparts_category_182(aBodyParts, writeAll);
            }
        case 182:
            return write_bodyparts_category_182(aBodyParts, writeAll);
        case 183:
            return write_bodyparts(aBodyParts, writeAll, {29, 32, 40});
        case 190:
            aBodyParts[0] = 18;
            return 1;
        case 191:
            aBodyParts[0] = 19;
            return 1;
        // case 192:
        //     aBodyParts[0] = 20;
        //     return 1;
        case 194:
            aBodyParts[0] = 1000;
            return 1;
        case 195:
            aBodyParts[0] = 1001;
            return 1;
        case 196:
            aBodyParts[0] = 1002;
            return 1;
        case 197:
            aBodyParts[0] = 1003;
            return 1;
        default:
            if (!is_weapon_category(itemCategory))
                return 0;
            aBodyParts[0] = 11;
            return 1;
    }
}

void AttachGetBodyPartFromItemFix() {
    ATTACH_HOOK(get_bodypart_from_item, get_bodypart_from_item_hook);
}
