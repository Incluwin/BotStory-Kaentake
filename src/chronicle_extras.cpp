// Ported from https://github.com/Chronicle20/gms-83-dll (AGPL 3.0)
// Credits: Chronicle20
//
// Two mini-edits bundled here:
//   1. no-ad-balloon  -- kills the Nexon ad popup on client exit
//   2. enable-minimize -- restores the minimize button in windowed mode
#include "pch.h"
#include "hook.h"
#include <windows.h>

// ============================================================================
// [1] no-ad-balloon
// ----------------------------------------------------------------------------
// WIN_MAIN = 0x009F19F2, WIN_MAIN_AD_BALLOON_CONDITIONAL offset = 0xA3D
// -> VA 0x009FA42F, flip "je" (0x74) to "jmp short" (0xEB)
// ============================================================================
static void AttachNoAdBalloon() {
    Patch1(0x009FA42F, 0xEB);
}

// ============================================================================
// [2] enable-minimize
// ----------------------------------------------------------------------------
// IAT-hook USER32!CreateWindowExA; OR in WS_MINIMIZEBOX on every window create.
// Chronicle20's version also zeroes dwExStyle which can break tooltips/popups;
// we intentionally leave dwExStyle alone -- only adding the minimize button.
// ============================================================================
typedef HWND(WINAPI* CreateWindowExA_t)(
    DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName,
    DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
    HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);

static CreateWindowExA_t CreateWindowExA_Original = nullptr;

static HWND WINAPI CreateWindowExA_Hook(
    DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName,
    DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
    HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam
) {
    dwStyle |= WS_MINIMIZEBOX;
    return CreateWindowExA_Original(
        dwExStyle, lpClassName, lpWindowName, dwStyle,
        X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
}

static void AttachEnableMinimize() {
    CreateWindowExA_Original = reinterpret_cast<CreateWindowExA_t>(
        GetAddress("USER32.DLL", "CreateWindowExA"));
    if (CreateWindowExA_Original) {
        AttachHook(
            reinterpret_cast<void**>(&CreateWindowExA_Original),
            CastHook(&CreateWindowExA_Hook));
    }
}

// ============================================================================
// Entry point -- called from hook.h::AttachClientHooks()
// ============================================================================
void AttachChronicleExtras() {
    AttachNoAdBalloon();
    AttachEnableMinimize();
}
