#include "pch.h"
#include <string>
#include "wvs/util.h"
#include "getmobinfobyid.h"
#include <unordered_map>

// ==========================================================
// Get Monster Info by ID
// Reads: String/Mob.img/<mobid>/name
// ==========================================================
std::string GetMobNameById(int id) {
    IWzPropertyPtr pMobImg = get_rm()->GetObjectA(L"String/Mob.img").GetUnknown();
    if (!pMobImg)
        return "Unknown";

    Ztl_bstr_t sId = std::to_wstring(id).c_str();

    try {
        IWzPropertyPtr pMob = pMobImg->item[sId].GetUnknown();
        if (!pMob)
            return "Unknown";

        Ztl_variant_t vName = pMob->item[L"name"];

        if (vName.vt == VT_BSTR)
            return (const char*)_bstr_t(vName);

    } catch (...) {
    }

    return "Unknown";
}

int GetMobLevelById(int id) {
    wchar_t path[64];
    swprintf_s(path, L"Mob/%07d.img", id);

    IWzPropertyPtr pMob = get_rm()->GetObjectA(path).GetUnknown();
    if (!pMob)
        return 0;

    try {
        IWzPropertyPtr pInfo = pMob->item[L"info"].GetUnknown();
        if (!pInfo)
            return 0;

        return get_int32(pInfo->item[L"level"], 0);

    } catch (...) {
    }

    return 0;
}