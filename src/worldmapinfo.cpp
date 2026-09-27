#include "pch.h"
#include "hook.h"
#include "ztl/ztl.h"

#include <windows.h>
#include <climits>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "wvs/tooltip.h"
#include "wvs/util.h"
#include "getmobinfobyid.h"
#include "getnpcbyid.h"

// =====================================================
// ADDRESSES
// =====================================================
static const DWORD ADDR_WM_OnMouseMove = 0x009EE2B3;
static const DWORD ADDR_GetFieldOpt = 0x00437A0C;

// =====================================================
// TYPES
// =====================================================
using t_OnMouseMove = int(__thiscall*)(void*, int, int);
using t_GetField = void*(__cdecl*)();

static t_GetField g_GetField = nullptr;

// =====================================================
// ORIGINAL FUNCTIONS
// =====================================================
static auto CWorldMapDlg__OnMouseMove_Orig =
        reinterpret_cast<t_OnMouseMove>(ADDR_WM_OnMouseMove);

using t_WorldMap_OnDestroy = void(__thiscall*)(void*);
static t_WorldMap_OnDestroy CWorldMapDlg__OnDestroy_Orig =
        reinterpret_cast<t_WorldMap_OnDestroy>(0x009EB94A);

// =====================================================
// TOOLTIP STATE
// =====================================================
alignas(8) unsigned char m_ttBuf[0x600];
bool m_bTtInit = false;

struct TooltipState {
    char text[512];
};

static TooltipState g_tt;

// =====================================================
// WORLD MAP STRUCT OFFSETS
// =====================================================
static const int OFF_SpotsPtr_dw = 366;
static const int SPOT_STRIDE_DWORDS = 17;
static const int SPOT_X_dw = 0;
static const int SPOT_Y_dw = 1;
static const int CANVAS_ORIGIN_X = 13;
static const int CANVAS_ORIGIN_Y = 35;
static const int OFF_Spot_FieldArr_dw = 11;
static const int HIT_RADIUS_PX = 6;

// =====================================================
// UTILS
// =====================================================
static inline int* rd_intptr_dw(const void* base, int dwordIndex) {
    return *reinterpret_cast<int* const*>(
        reinterpret_cast<const char*>(base) + dwordIndex * 4);
}

struct SpotInfo {
    int index;
    int mapId;
};

SpotInfo GetSpotAt(void* base, int rx, int ry) {
    SpotInfo out{};
    out.index = -1;

    int* spots = rd_intptr_dw(base, OFF_SpotsPtr_dw);
    if (!spots)
        return out;

    int cnt = *(spots - 1);
    if (cnt <= 0)
        return out;

    int cx = rx - CANVAS_ORIGIN_X;
    int cy = ry - CANVAS_ORIGIN_Y;
    int R2 = HIT_RADIUS_PX * HIT_RADIUS_PX;

    int best = -1;
    int bestR2 = INT_MAX;

    for (int i = 0; i < cnt; i++) {
        int* s = spots + i * SPOT_STRIDE_DWORDS;

        unsigned* arr = *(unsigned**)(s + OFF_Spot_FieldArr_dw);
        if (!arr)
            continue;

        unsigned n = *(arr - 1);
        if (!n)
            continue;

        int dx = s[SPOT_X_dw] - cx;
        int dy = s[SPOT_Y_dw] - cy;
        int r2 = dx * dx + dy * dy;

        if (r2 <= R2 && r2 < bestR2) {
            bestR2 = r2;
            best = i;
        }
    }

    if (best < 0)
        return out;

    int* s = spots + best * SPOT_STRIDE_DWORDS;
    unsigned* arr = *(unsigned**)(s + OFF_Spot_FieldArr_dw);

    out.index = best;
    out.mapId = (arr && *(arr - 1) > 0) ? arr[0] : 0;

    return out;
}

void* get_field_ctx() {
    if (!g_GetField)
        g_GetField = reinterpret_cast<t_GetField>(ADDR_GetFieldOpt);

    return g_GetField ? g_GetField() : nullptr;
}

// =====================================================
// TOOLTIP CORE
// =====================================================
void EnsureTooltip() {
    if (m_bTtInit)
        return;

    reinterpret_cast<void(__thiscall*)(void*)>(0x008E49B5)(m_ttBuf);
    m_bTtInit = true;
}

void TooltipClear() {
    if (m_bTtInit)
        reinterpret_cast<void(__thiscall*)(void*)>(0x008E6E23)(m_ttBuf);
}

void TooltipShow(int x, int y, const char* text) {
    if (!text || !*text)
        return;

    EnsureTooltip();

    CUIToolTip* tt = reinterpret_cast<CUIToolTip*>(m_ttBuf);
    tt->ClearToolTip();

    ZXString<char> title("Map Information");
    ZXString<char> desc(text);

    tt->SetToolTip_String2(
        x + 64,
        y + 64,
        title,
        desc,
        0, 0, 0,
        250,
        1,
        0);
}

// =====================================================
// CACHE
// =====================================================
std::unordered_map<int, std::string> g_mapCache;

// =====================================================
// DATA
// =====================================================
std::string GetMapMobNpcInfo(int mapId) {
    auto it = g_mapCache.find(mapId);
    if (it != g_mapCache.end())
        return it->second;

    try {
        wchar_t path[128];
        swprintf_s(path, L"Map/Map/Map%d/%08d.img", mapId / 100000000, mapId);

        IWzPropertyPtr pMap = get_rm()->GetObjectA(path).GetUnknown();
        if (!pMap)
            return "No data";

        IWzPropertyPtr pLife = pMap->item[L"life"].GetUnknown();
        if (!pLife)
            return "No spawns";

        std::unordered_map<int, std::string> mobs;
        std::unordered_set<std::string> npcs;

        int total = pLife->Getcount();

        for (int i = 0; i < total; i++) {
            wchar_t idx[16];
            _itow_s(i, idx, 10);

            IWzPropertyPtr pEntry = get_unknown(pLife->item[idx]);
            if (!pEntry)
                continue;

            int id = get_int32(pEntry->item[L"id"], 0);

            Ztl_variant_t vType = pEntry->item[L"type"];
            if (vType.vt != VT_BSTR)
                continue;

            std::wstring type = (const wchar_t*)_bstr_t(vType);

            if (type == L"m")
                mobs[id] = GetMobNameById(id);
            else if (type == L"n")
                npcs.insert(GetNpcById(id));
        }

        std::string result;

        if (!mobs.empty()) {
            result += "Monsters:\n";

            int shown = 0;
            for (auto& [id, name] : mobs) {
                int lvl = GetMobLevelById(id);
                result += "[Lv. " + std::to_string(lvl) + "] - " + name + "\n";

                if (++shown >= 10)
                    break;
            }
        }

        if (!npcs.empty()) {
            if (!result.empty())
                result += "\n";

            result += "NPCs:\n";

            int shown = 0;
            for (auto& npc : npcs) {
                result += "- " + npc + "\n";

                if (++shown >= 10)
                    break;
            }
        }

        if (result.empty())
            result = "Empty Map";

        g_mapCache[mapId] = result;
        return result;

    } catch (...) {
        return "Error";
    }
}

// =====================================================
// HOOKS
// =====================================================
static int __fastcall CWorldMapDlg__OnMouseMove_hook(
        void* _ECX, void*, int x, int y) {

    if (!_ECX || !get_field_ctx()) {
        TooltipClear();
        return 0;
    }

    void* base = (char*)_ECX - 4;
    SpotInfo spot = GetSpotAt(base, x, y);

    if (spot.index >= 0) {
        std::string text = GetMapMobNpcInfo(spot.mapId);
        strncpy_s(g_tt.text, text.c_str(), _TRUNCATE);

        TooltipShow(x, y, g_tt.text);
    } else {
        TooltipClear();
    }

    return CWorldMapDlg__OnMouseMove_Orig(_ECX, x, y);
}

void __fastcall CWorldMapDlg__OnDestroy_hook(void* _ECX, void*) {
    TooltipClear();
    CWorldMapDlg__OnDestroy_Orig(_ECX);
}

// =====================================================
// INSTALL
// =====================================================
void AttachMapInfoToolTip() {
    ATTACH_HOOK(CWorldMapDlg__OnMouseMove_Orig, CWorldMapDlg__OnMouseMove_hook);
    ATTACH_HOOK(CWorldMapDlg__OnDestroy_Orig, CWorldMapDlg__OnDestroy_hook);
}