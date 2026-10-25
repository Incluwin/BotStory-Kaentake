#include "pch.h"
#include "hook.h"
#include "movementkeys.h"
#include "ztl/ztl.h"

#include <cstring>

// The module movement_keys: Up, Down, Left and Right become bindable actions (type 4, actions 28-31) in the key
// config window, so a player can move with WASD or any other key. The server keeps them in the keymap like any other
// binding. The four arrow icons are drawn from icon 10 when UI/UIWindow.img loads (resman.cpp calls
// InjectMovementKeyIcons), so no WZ file changes.
//
// Ported 2026-10-25 from the community release "kaentake-direction-keys" (made against iw2d/kaentake 2a43a01; no
// licence or author line). Changes here: the two build checks log to kaentake_diag.txt and skip the module instead of
// opening a message box.

namespace {

constexpr int kFirstAction = 28;
constexpr int kFirstPaletteSlot = 40;
constexpr int kDirectionCount = 4;
constexpr int kLastBindableScanCode = 88;
constexpr unsigned char kArrowScanCodes[kDirectionCount] = {0xC8, 0xD0, 0xCB, 0xCD}; // up, down, left, right
constexpr uintptr_t kKeyMapManager = 0x00BED5A0;
constexpr uintptr_t kKeyConfigOpen = 0x00BED650;
constexpr uintptr_t kWndManager = 0x00BEC20C;
constexpr uintptr_t kKeyboardSnapshotPatch = 0x0059B00A;
constexpr uintptr_t kKeyboardSnapshotReturn = 0x0059B011;
constexpr uintptr_t kPaletteDrawEndAddress = 0x00833FB0;

using PaletteInit_t = void(__thiscall*)(void*);
using ActionForSlot_t = int(__thiscall*)(void*, int);
using SlotForAction_t = int(__thiscall*)(void*, int, int);
using IsKeyPressed_t = int(__thiscall*)(void*, int);

PaletteInit_t originalPaletteInit = reinterpret_cast<PaletteInit_t>(0x00836619);
ActionForSlot_t originalActionForSlot = reinterpret_cast<ActionForSlot_t>(0x00836C82);
SlotForAction_t originalSlotForAction = reinterpret_cast<SlotForAction_t>(0x00836C5F);
IsKeyPressed_t originalIsKeyPressed = reinterpret_cast<IsKeyPressed_t>(0x0059A25A);

// CUIKeyConfig has 54 palette records of 12 bytes at +0x9DC. The stock client
// populates only records 0..39; records 40..43 are visible empty cells.
void __fastcall PaletteInit_hook(void* self, void*) {
    originalPaletteInit(self);
    auto* records = static_cast<unsigned char*>(self) + 0x9DC;
    for (int i = 0; i < kDirectionCount; ++i) {
        auto* record = records + (kFirstPaletteSlot + i) * 12;
        record[0] = 4;
        *reinterpret_cast<int*>(record + 1) = kFirstAction + i;
        std::memset(record + 5, 0, 7);
    }
}

int __fastcall ActionForSlot_hook(void* self, void*, int slot) {
    if (slot >= kFirstPaletteSlot && slot < kFirstPaletteSlot + kDirectionCount)
        return kFirstAction + slot - kFirstPaletteSlot;
    return originalActionForSlot(self, slot);
}

int __fastcall SlotForAction_hook(void* self, void*, int type, int action) {
    if (type == 4 && action >= kFirstAction && action < kFirstAction + kDirectionCount)
        return kFirstPaletteSlot + action - kFirstAction;
    return originalSlotForAction(self, type, action);
}

bool TextEditFocused() {
    auto* manager = *reinterpret_cast<unsigned char**>(kWndManager);
    if (!manager) return false;
    auto* focused = *reinterpret_cast<void**>(manager + 0x88);
    if (!focused) return false;
    auto* vtable = *reinterpret_cast<void***>(focused);
    if (!vtable) return false;
    const uintptr_t onKey = reinterpret_cast<uintptr_t>(vtable[0]);
    // CCtrlEdit, CCtrlEdit with alternate style, and CCtrlMLEdit.
    return onKey == 0x004C9DA8 || onKey == 0x004CF399 || onKey == 0x004D3159;
}

int __fastcall IsKeyPressed_hook(void* self, void*, int virtualKey) {
    const int original = originalIsKeyPressed(self, virtualKey);
    if (original || virtualKey < VK_LEFT || virtualKey > VK_DOWN ||
        *reinterpret_cast<void**>(kKeyConfigOpen) || TextEditFocused())
        return original;

    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    if (foregroundProcess != GetCurrentProcessId()) return original;

    const auto* manager = *reinterpret_cast<const unsigned char**>(kKeyMapManager);
    if (!manager) return original;
    int direction;
    switch (virtualKey) {
    case VK_UP: direction = 0; break;
    case VK_DOWN: direction = 1; break;
    case VK_LEFT: direction = 2; break;
    case VK_RIGHT: direction = 3; break;
    default: return original;
    }
    for (int scanCode = 1; scanCode <= kLastBindableScanCode; ++scanCode) {
        const auto* binding = manager + 4 + scanCode * 5;
        int action;
        std::memcpy(&action, binding + 1, sizeof(action));
        const UINT virtualBoundKey = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
        if (binding[0] == 4 && action == kFirstAction + direction &&
            virtualBoundKey && (GetAsyncKeyState(virtualBoundKey) & 0x8000))
            return 1;
    }
    return original;
}

void __cdecl RemapKeyboardSnapshot(unsigned char* keys) {
    if (!keys || *reinterpret_cast<void**>(kKeyConfigOpen) || TextEditFocused()) return;
    const auto* manager = *reinterpret_cast<const unsigned char**>(kKeyMapManager);
    if (!manager) return;

    ApplyMovementKeyBindings(keys, manager + 4);
}

__declspec(naked) void KeyboardSnapshot_hook() {
    __asm {
        pushfd
        pushad
        lea eax, [ebp - 0x110]
        push eax
        call RemapKeyboardSnapshot
        add esp, 4
        popad
        popfd
        test byte ptr [ebp - 0xD6], 0x80
        jmp kKeyboardSnapshotReturn
    }
}

void DrawArrow(IWzCanvas* canvas, int direction) {
    constexpr unsigned ink = 0xFFFDF6F9;
    // One integer-grid mask, rotated for the other directions. DrawLine's
    // width and antialiasing made the up and left diagonals look uneven.
    for (int y = 8; y <= 24; ++y) {
        for (int x = 7; x <= 24; ++x) {
            const bool head = y <= 15 && x >= 15 - (y - 8) && x <= 16 + (y - 8);
            const bool stem = y >= 16 && x >= 14 && x <= 17;
            if (!head && !stem) continue;
            int px = x, py = y;
            switch (direction) {
            case 1: px = 31 - x; py = 31 - y; break; // down
            case 2: px = y; py = 31 - x; break;     // left
            case 3: px = 31 - y; py = x; break;     // right
            }
            canvas->DrawRectangle(px, py, 1, 1, ink);
        }
    }
}

} // namespace

void ApplyMovementKeyBindings(unsigned char* keys, const unsigned char* bindings) {
    bool directions[kDirectionCount]{};
    // The saved key numbers are DirectInput scan codes (W=17, A=30, S=31,
    // D=32), matching the raw snapshot at this point in UpdateKeyboard.
    for (int scanCode = 1; scanCode <= kLastBindableScanCode; ++scanCode) {
        if (!(keys[scanCode] & 0x80)) continue;
        const auto* binding = bindings + scanCode * 5;
        if (binding[0] != 4) continue;
        int action;
        std::memcpy(&action, binding + 1, sizeof(action));
        if (action < kFirstAction || action >= kFirstAction + kDirectionCount) continue;
        directions[action - kFirstAction] = true;
        keys[scanCode] = 0;
    }
    for (int i = 0; i < kDirectionCount; ++i)
        if (directions[i]) keys[kArrowScanCodes[i]] |= 0x80;
}

void InjectMovementKeyIcons(IWzProperty* root, const wchar_t* absoluteUOL) {
    if (!root || !absoluteUOL || _wcsicmp(absoluteUOL, L"UI/UIWindow.img") != 0) return;
    IWzPropertyPtr icons = root->item[L"KeyConfig/icon"].GetUnknown();
    if (!icons) return;
    // Icon 10 has the matching frame and anchor. Its "TO ALL" caption is
    // entirely within the flat pink interior, which we clear below.
    IWzCanvasPtr source = icons->item[L"10"].GetUnknown();
    if (!source || source->width != 32 || source->height != 32) return;
    for (int i = 0; i < kDirectionCount; ++i) {
        IWzCanvasPtr canvas;
        PcCreateObject<IWzCanvasPtr>(L"Canvas", canvas, nullptr);
        canvas->Create(32, 32, 0, CANVAS_PIXFORMAT::CP_A8R8G8B8);
        canvas->Copy(0, 0, source);
        canvas->DrawRectangle(3, 2, 26, 28, 0xFFBB8899);
        canvas->cx = source->cx;
        canvas->cy = source->cy;
        DrawArrow(canvas, i);
        wchar_t name[8];
        swprintf_s(name, L"%d", kFirstAction + i);
        icons->Add(name, Ztl_variant_t(static_cast<IUnknown*>(canvas), true), false);
    }
}

void AttachMovementKeysMod() {
    static const unsigned char expected[] = {0xF6, 0x85, 0x2A, 0xFF, 0xFF, 0xFF, 0x80};
    if (std::memcmp(reinterpret_cast<const void*>(kKeyboardSnapshotPatch), expected, sizeof(expected)) != 0) {
        LogMessage("[movement_keys] off: CInputSystem::UpdateKeyboard is not the build this was made for");
        return;
    }
    if (*reinterpret_cast<const unsigned*>(kPaletteDrawEndAddress) != 0x00BE2BF8) {
        LogMessage("[movement_keys] off: the CUIKeyConfig draw loop is not the build this was made for");
        return;
    }
    if (!ATTACH_HOOK(originalPaletteInit, PaletteInit_hook) ||
        !ATTACH_HOOK(originalActionForSlot, ActionForSlot_hook) ||
        !ATTACH_HOOK(originalSlotForAction, SlotForAction_hook) ||
        !ATTACH_HOOK(originalIsKeyPressed, IsKeyPressed_hook)) {
        LogMessage("[movement_keys] a KeyConfig hook failed to attach");
        return;
    }
    PatchJmp(kKeyboardSnapshotPatch, &KeyboardSnapshot_hook);
    PatchNop(kKeyboardSnapshotPatch + 5, kKeyboardSnapshotPatch + sizeof(expected));
    // Draw all 44 populated palette records. The adjacent position array has
    // room for 54 records, so this only changes the loop's end pointer.
    Patch4(kPaletteDrawEndAddress, 0x00BE2C18);
}
