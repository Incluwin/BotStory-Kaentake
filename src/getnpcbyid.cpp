#include "pch.h"
#include <string>
#include "wvs/util.h"
#include "getnpcbyid.h"

// ==========================================================
// Get NPC Name by ID
// Reads: String/Npc.img/<npcid>/name
// ==========================================================
std::string GetNpcById(int id) {
    IWzPropertyPtr pNpcImg = get_rm()->GetObjectA(L"String/Npc.img").GetUnknown();
    if (!pNpcImg)
        return "Unknown";

    Ztl_bstr_t sId = std::to_wstring(id).c_str();

    try {
        IWzPropertyPtr pNpc = pNpcImg->item[sId].GetUnknown();
        if (!pNpc)
            return "Unknown";

        Ztl_variant_t vName = pNpc->item[L"name"];

        if (vName.vt == VT_BSTR)
            return (const char*)_bstr_t(vName);

    } catch (...) {
    }

    return "Unknown";
}