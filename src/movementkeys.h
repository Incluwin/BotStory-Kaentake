#pragma once

struct IWzProperty;

// Four extra actions in the existing KeyConfig palette, stored in the normal
// type-4 keymap and persisted by the server with every other key binding.
void InjectMovementKeyIcons(IWzProperty* root, const wchar_t* absoluteUOL);
void ApplyMovementKeyBindings(unsigned char* scanStates, const unsigned char* bindings);
