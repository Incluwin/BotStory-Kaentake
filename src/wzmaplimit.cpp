#include "pch.h"
#include "hook.h"
#include <intrin.h>

// The stock client maps every WZ archive whole into its 32-bit address space (NameSpace.dll, in the archive open
// below). This client's Character.wz is 1.9 GB and Npc.wz 554 MB: mapped, they leave too little room, and the client
// ran out of its 4 GB with 12 characters on Horntail (4,081 MB, 2026-10-14). An archive the map passes over reads from
// its file instead, the way the stock client already reads Map.wz, Mob.wz, Item.wz, Effect.wz and Sound.wz once no
// room is left for them: NameSpace's stream read seeks and reads the file when it has no view.

namespace {
    // Archives larger than this are read from their file instead of mapped whole.
    constexpr LONGLONG kMapLimitBytes = 128LL * 1024 * 1024;

    // NameSpace.dll's file stream: maps a view of its file (thiscall, 7 stack arguments, ret 0x1C); the file handle
    // is at +0x10. Hooked as __fastcall, whose ecx and callee-cleaned stack match thiscall's; edx is unused.
    typedef void*(__fastcall* MapView_t)(void* pStream, void* edx, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5,
                                         DWORD a6, DWORD a7);
    MapView_t g_pMapView = nullptr;
    // Where the archive open's whole-file map call returns; the stream's other callers (a checksum) are left alone.
    uintptr_t g_uArchiveOpenReturn = 0;

    void* __fastcall MapView_hook(void* pStream, void* edx, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5,
                                  DWORD a6, DWORD a7) {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_uArchiveOpenReturn) {
            HANDLE hFile = *reinterpret_cast<HANDLE*>(reinterpret_cast<uintptr_t>(pStream) + 0x10);
            LARGE_INTEGER liSize;
            if (hFile && hFile != INVALID_HANDLE_VALUE && GetFileSizeEx(hFile, &liSize)
                    && liSize.QuadPart > kMapLimitBytes) {
                DEBUG_MESSAGE("WZ map limit: reading a %lld MB archive from its file", liSize.QuadPart >> 20);
                return nullptr; // as a map that found no room: no view, the archive reads from its file
            }
        }
        return g_pMapView(pStream, edx, a1, a2, a3, a4, a5, a6, a7);
    }
}

// Called before the original CWvsApp::InitializeResMan opens the archives.
void AttachWzMapLimit() {
    void* pMap = GetAddressByPattern("NAMESPACE.DLL", "55 8B EC 83 EC 28 53 56 57 33 DB 8B F1 53 E8");
    auto pCall = static_cast<unsigned char*>(
            GetAddressByPattern("NAMESPACE.DLL", "E8 ?? ?? ?? ?? 89 46 18 8B 07 8B CF FF 50 2C"));
    if (!pMap || !pCall || pCall + 5 + *reinterpret_cast<int32_t*>(pCall + 1) != pMap) {
        DEBUG_MESSAGE("WZ map limit: NameSpace.dll does not match, archives stay mapped");
        return;
    }
    g_uArchiveOpenReturn = reinterpret_cast<uintptr_t>(pCall + 5);
    g_pMapView = reinterpret_cast<MapView_t>(pMap);
    ATTACH_HOOK(g_pMapView, MapView_hook);
}
