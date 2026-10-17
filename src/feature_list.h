#pragma once
// The client's modules, one X(...) line each. Shared by Kaentake.dll (features.cpp attaches them) and the
// launcher (its Features page lists them), so adding a module is one line here plus its .cpp in CMakeLists.txt.
//
// X(id, title, default_on, flags, requires, attach)
//   id        snake_case: the config.ini [features] key, and the name the server's feature handshake uses
//   title     the launcher's checkbox text
//   default   1 = attached when config.ini has no line for it
//   flags     FEATURE_CORE          always attached, no switch
//             FEATURE_SERVER_GATED  changes gameplay: the server may veto or require it at login
//             FEATURE_LIVE          features::SetActive can make it inert (and back) without a restart
//             FEATURE_EARLY         attached from DllMain, before the client's own hooks go in
//             FEATURE_DEFERRED      attached by another module when it needs it (wz_map_limit from res_man)
//   requires  a module that must be attached for this one to work, or none
//   attach    the function that installs the module's hooks and patches
//
// The list order is the attach order. Every hook and patch an attach function makes is recorded under its module
// (features.cpp), and two modules touching the same bytes are reported at startup.
#define FEATURE_CORE         0x01
#define FEATURE_SERVER_GATED 0x02
#define FEATURE_LIVE         0x04
#define FEATURE_EARLY        0x08
#define FEATURE_DEFERRED     0x10

#define KAENTAKE_FEATURES(X) \
    X(version_guard,           "v83 client version guard",               1, FEATURE_CORE | FEATURE_EARLY,    none,          v83::EnsureClientVersionOrExit) \
    X(client_bypass,           "Client bypass",                          1, FEATURE_CORE,                    none,          AttachClientBypass) \
    X(client_inlink,           "WZ inlink/outlink canvases",             1, FEATURE_CORE,                    none,          AttachClientInlink) \
    X(string_pool,             "String pool edits",                      1, FEATURE_CORE,                    none,          AttachStringPoolMod) \
    X(res_man,                 "Resource manager (Custom.wz)",           1, FEATURE_CORE,                    none,          AttachResManMod) \
    X(wz_map_limit,            "Large WZ archives read from file",       1, FEATURE_CORE | FEATURE_DEFERRED, none,          AttachWzMapLimit) \
    X(avatar_data,             "Avatar extras (taming mobs, chairs)",    1, 0,                               none,          AttachAvatarDataMod) \
    X(item_effect,             "Item effects (ItemEff.img)",             1, 0,                               avatar_data,   AttachItemEffectMod) \
    X(resolution,              "Wide resolutions and config.ini prefs",  1, 0,                               none,          AttachResolutionMod) \
    X(mob_hp_tag,              "Mob HP bar with numbers",                1, FEATURE_LIVE,                    none,          AttachMobHpTagMod) \
    X(tool_tip,                "Custom item names and equip tooltips",   1, FEATURE_LIVE,                    none,          AttachToolTipMod) \
    X(fusion_anvil_field,      "Fusion Anvil field in item packets",     1, FEATURE_CORE,                    none,          AttachFusionAnvilField) \
    X(fusion_anvil,            "Fusion Anvil transmog",                  1, 0,                               none,          AttachFusionAnvilMod) \
    X(item_icon,               "Item icon drawing fix",                  1, FEATURE_LIVE,                    none,          AttachIconIconMod) \
    X(temp_stat,               "Buff and skill cooldown display",        1, FEATURE_LIVE,                    none,          AttachTempStatMod) \
    X(quick_login,             "Quick login (skip world select)",        1, FEATURE_LIVE,                    none,          AttachQuickLoginMod) \
    X(remember_password,       "Remember login",                         1, FEATURE_LIVE,                    none,          AttachRememberPasswordMod) \
    X(boss_hp,                 "Boss HP % under the boss bar",           0, 0,                               none,          AttachBossHPMod) \
    X(chronicle_extras,        "No exit ad, minimize button",            1, FEATURE_LIVE,                    none,          AttachChronicleExtras) \
    X(enlarge_quickslot,       "26-slot quickslot bar",                  1, 0,                               resolution,    AttachEnlargeQuickslotMod) \
    X(shoulder_slots,          "Shoulder equip slot fix",                1, FEATURE_LIVE,                    none,          AttachShoulderSlotsFix) \
    X(is_correct_body_part,    "Equip body part check fix",              1, FEATURE_LIVE,                    none,          AttachIsCorrectBodyPartFix) \
    X(get_body_part_from_item, "Equip body part lookup fix",             1, FEATURE_LIVE,                    none,          AttachGetBodyPartFromItemFix) \
    X(free_addy,               "Gameplay patches (damage/speed caps)",   1, FEATURE_SERVER_GATED | FEATURE_LIVE, none,      AttachFreeAddy) \
    X(ext_chain_lightning,     "Chain Lightning range and damage",       1, FEATURE_SERVER_GATED | FEATURE_LIVE, none,      AttachExtChainLightning) \
    X(map_info_tool_tip,       "World map mob/NPC tooltips",             1, 0,                               none,          AttachMapInfoToolTip) \
    X(quest_list_color,        "Resident quests in blue",                1, FEATURE_LIVE,                    none,          AttachQuestListColorMod) \
    X(crew_window,             "Crew window",                            1, 0,                               none,          AttachCrewWindowMod)

// Keys that predate [features] and still switch a module when [features] has no line for it.
struct FeatureAlias {
    const char* id;
    const char* section;
    const char* key;
};
constexpr FeatureAlias kFeatureAliases[] = {
    { "crew_window", "config", "crew_window" },
};


#include <windows.h>
#include <cstdlib>
#include <cstring>

// config.ini [features] <id>=1|0 (also on/off, true/false, yes/no). A missing or unreadable line falls back to an
// alias key, then to the module's default. Shared with the launcher so both read a switch the same way.
inline bool ReadFeatureSwitch(const char* sConfigPath, const char* sId, bool bDefault) {
    char buf[16];
    if (GetPrivateProfileStringA("features", sId, "", buf, sizeof(buf), sConfigPath) > 0) {
        if (buf[0] >= '0' && buf[0] <= '9') {
            return atoi(buf) != 0;
        }
        if (!_stricmp(buf, "on") || !_stricmp(buf, "true") || !_stricmp(buf, "yes")) {
            return true;
        }
        if (!_stricmp(buf, "off") || !_stricmp(buf, "false") || !_stricmp(buf, "no")) {
            return false;
        }
    }
    for (const auto& a : kFeatureAliases) {
        if (strcmp(a.id, sId) == 0 && GetPrivateProfileStringA(a.section, a.key, "", buf, sizeof(buf), sConfigPath) > 0) {
            return atoi(buf) != 0; // what GetPrivateProfileIntA made of the old key
        }
    }
    return bDefault;
}
