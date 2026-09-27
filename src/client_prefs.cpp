// Client preferences bridge: reads optional [config] keys from config.ini
// (propagated from launcher.cpp into KAENTAKE_* env vars), overrides the
// matching GLOBAL_OPT keys so the game picks them up on LoadGlobal, and
// mirrors the current in-game values back into config.ini on SaveGlobal so
// config.ini stays the source of truth.
//
// Hooked into resolution.cpp's CConfig::LoadGlobal_hook / SaveGlobal_hook
// via the two exported functions at the bottom.
//
// The GLOBAL_OPT key names below are the best-known v83 conventions. If a
// key is wrong, the only observable effect is "config.ini line is ignored"
// — no crash, no partial state. Fix the string and rebuild.
#include "pch.h"
#include "hook.h"
#include "constants.h"
#include "wvs/config.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace {

struct PrefEntry {
    const char* ini_key;
    const char* env_name;
    const char* opt_key;
    int lo;
    int hi;
};

// Keep indices stable so the launcher's propagate list and the apply/mirror
// loops all walk the same table. Adjust opt_key strings here if we learn
// the game uses different internal names.
const PrefEntry kPrefs[] = {
    { "bgm_volume",  "KAENTAKE_BGM_VOLUME",  "soBGMVolume",    0, 100 },
    { "bgm_mute",    "KAENTAKE_BGM_MUTE",    "soBGMMute",      0,   1 },
    { "sfx_volume",  "KAENTAKE_SFX_VOLUME",  "soSoundVolume",  0, 100 },
    { "sfx_mute",    "KAENTAKE_SFX_MUTE",    "soSoundMute",    0,   1 },
    { "fullscreen",  "KAENTAKE_FULLSCREEN",  "soFullScreen",   0,   1 },
    { "hp_alert",    "KAENTAKE_HP_ALERT",    "soHPAlert",      0, 100 },
    { "mp_alert",    "KAENTAKE_MP_ALERT",    "soMPAlert",      0, 100 },
};

// Parse a decimal int from an env var; return false on absence or bad format.
bool ReadEnvInt(const char* sName, int* pOut, int nLow, int nHigh) {
    char buf[16];
    DWORD n = GetEnvironmentVariableA(sName, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    char* endp = nullptr;
    long val = strtol(buf, &endp, 10);
    if (endp == buf) return false;
    if (val < nLow) val = nLow;
    if (val > nHigh) val = nHigh;
    *pOut = static_cast<int>(val);
    return true;
}

}  // namespace


// Called from CConfig::LoadGlobal_hook AFTER the original LoadGlobal has
// populated CConfig's internal map from the registry. Overrides each key
// that the user opted into via config.ini so that subsequent GetOpt_Int
// callers (sound mixer init, UI dialog population, etc.) see our value.
void ApplyPrefsAfterLoadGlobal() {
    auto* cfg = CConfig::GetInstance();
    if (!cfg) return;
    for (const auto& p : kPrefs) {
        int val;
        if (ReadEnvInt(p.env_name, &val, p.lo, p.hi)) {
            cfg->SetOpt_Int(CConfig::GLOBAL_OPT, p.opt_key, val);
        }
    }
}


// Called from CConfig::SaveGlobal_hook AFTER the original SaveGlobal wrote
// the current in-game state to the registry. For every key the user has
// already opted into (i.e. already present in config.ini), mirror the
// current value back so config.ini stays in sync with in-game edits.
//
// Keys absent from config.ini are left alone — signal that the user wants
// the in-game System Option dialog to own that particular setting.
void MirrorPrefsToConfigAfterSaveGlobal() {
    auto* cfg = CConfig::GetInstance();
    if (!cfg) return;
    for (const auto& p : kPrefs) {
        char existing[32];
        DWORD n = GetPrivateProfileStringA(
            "config", p.ini_key, "",
            existing, sizeof(existing), ".\\" CONSTANTS_CONFIG_NAME);
        if (n == 0) continue;  // user didn't opt in → leave alone
        int val = cfg->GetOpt_Int(CConfig::GLOBAL_OPT, p.opt_key, 0, p.lo, p.hi);
        char sVal[16];
        _snprintf_s(sVal, sizeof(sVal), _TRUNCATE, "%d", val);
        WritePrivateProfileStringA("config", p.ini_key, sVal,
                                   ".\\" CONSTANTS_CONFIG_NAME);
    }
}
