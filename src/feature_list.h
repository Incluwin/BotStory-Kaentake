#pragma once
// The client's modules, one X(...) line each. Shared by Kaentake.dll (features.cpp attaches them) and the
// launcher (its Features page lists them), so adding a module is one line here plus its .cpp in CMakeLists.txt.
//
// X(id, title, default_on, flags, section, requires, version, attach)
//   id        snake_case: the config.ini [features] key, and the name the server's feature handshake uses
//   title     the launcher's checkbox text
//   default   1 = attached when config.ini has no line for it
//   flags     FEATURE_CORE          always attached, no switch
//             FEATURE_SERVER_GATED  changes gameplay: the server may veto or require it at login
//             FEATURE_LIVE          features::SetActive can make it inert (and back) without a restart
//             FEATURE_EARLY         attached from DllMain, before the client's own hooks go in
//             FEATURE_DEFERRED      attached by another module when it needs it (wz_map_limit from res_man)
//   section   the Features page heading it is listed under (KAENTAKE_SECTIONS); core for the core modules
//   requires  a module that must be attached for this one to work, or none
//   version   1 up: raise it when the module's behaviour changes, so the server's policy can name a minimum
//             (the feature handshake sends it)
//   attach    the function that installs the module's hooks and patches
//
// The list order is the attach order. Every hook and patch an attach function makes is recorded under its module
// (features.cpp), and two modules touching the same bytes are reported at startup.
#define FEATURE_CORE         0x01
#define FEATURE_SERVER_GATED 0x02
#define FEATURE_LIVE         0x04
#define FEATURE_EARLY        0x08
#define FEATURE_DEFERRED     0x10

// The Features page's headings, in page order. core holds the modules that have no switch and is not shown;
// botstory is what only does something on a BotStory server.
#define KAENTAKE_SECTIONS(S) \
    S(core,     "Always on") \
    S(botstory, "BotStory") \
    S(ui,       "Interface") \
    S(combat,   "Combat display") \
    S(login,    "Login") \
    S(avatar,   "Avatars and item effects") \
    S(gameplay, "Gameplay (the server decides)") \
    S(fixes,    "Fixes")

enum class FeatureSection : int {
#define FEATURE_SECTION_ENUM(id, title) id,
    KAENTAKE_SECTIONS(FEATURE_SECTION_ENUM)
#undef FEATURE_SECTION_ENUM
    count
};

#define KAENTAKE_FEATURES(X) \
    X(version_guard,           "v83 client version guard",               1, FEATURE_CORE | FEATURE_EARLY,           core,     none,          1, v83::EnsureClientVersionOrExit) \
    X(client_bypass,           "Client bypass",                          1, FEATURE_CORE,                           core,     none,          1, AttachClientBypass) \
    X(client_inlink,           "WZ inlink/outlink canvases",             1, FEATURE_CORE,                           core,     none,          1, AttachClientInlink) \
    X(string_pool,             "String pool edits",                      1, FEATURE_CORE,                           core,     none,          1, AttachStringPoolMod) \
    X(res_man,                 "Resource manager (Custom.wz)",           1, FEATURE_CORE,                           core,     none,          1, AttachResManMod) \
    X(wz_map_limit,            "Large WZ archives read from file",       1, FEATURE_CORE | FEATURE_DEFERRED,        core,     none,          1, AttachWzMapLimit) \
    X(avatar_data,             "Avatar extras (taming mobs, chairs)",    1, 0,                                      avatar,   none,          1, AttachAvatarDataMod) \
    X(item_effect,             "Item effects (ItemEff.img)",             1, 0,                                      avatar,   avatar_data,   1, AttachItemEffectMod) \
    X(resolution,              "Wide resolutions and config.ini prefs",  1, 0,                                      ui,       none,          1, AttachResolutionMod) \
    X(mob_hp_tag,              "Mob HP bar with numbers",                1, FEATURE_LIVE,                           combat,   none,          1, AttachMobHpTagMod) \
    X(tool_tip,                "Custom item names and equip tooltips",   1, FEATURE_LIVE,                           ui,       none,          1, AttachToolTipMod) \
    X(fusion_anvil_field,      "Fusion Anvil field in item packets",     1, FEATURE_CORE,                           core,     none,          1, AttachFusionAnvilField) \
    X(fusion_anvil,            "Fusion Anvil transmog",                  1, 0,                                      botstory, none,          1, AttachFusionAnvilMod) \
    X(item_icon,               "Item icon drawing fix",                  1, FEATURE_LIVE,                           fixes,    none,          1, AttachIconIconMod) \
    X(temp_stat,               "Buff and skill cooldown display",        1, FEATURE_LIVE,                           combat,   none,          1, AttachTempStatMod) \
    X(quick_login,             "Quick login (skip world select)",        1, FEATURE_LIVE,                           login,    none,          1, AttachQuickLoginMod) \
    X(remember_password,       "Remember login",                         1, FEATURE_LIVE,                           login,    none,          1, AttachRememberPasswordMod) \
    X(boss_hp,                 "Boss HP % under the boss bar",           0, 0,                                      combat,   none,          1, AttachBossHPMod) \
    X(chronicle_extras,        "No exit ad, minimize button",            1, FEATURE_LIVE,                           ui,       none,          1, AttachChronicleExtras) \
    X(enlarge_quickslot,       "26-slot quickslot bar",                  1, 0,                                      ui,       resolution,    1, AttachEnlargeQuickslotMod) \
    X(movement_keys,           "Bindable movement keys (e.g. WASD)",     1, 0,                                      ui,       none,          1, AttachMovementKeysMod) \
    X(shoulder_slots,          "Shoulder equip slot fix",                1, FEATURE_LIVE,                           fixes,    none,          1, AttachShoulderSlotsFix) \
    X(is_correct_body_part,    "Equip body part check fix",              1, FEATURE_LIVE,                           fixes,    none,          1, AttachIsCorrectBodyPartFix) \
    X(get_body_part_from_item, "Equip body part lookup fix",             1, FEATURE_LIVE,                           fixes,    none,          1, AttachGetBodyPartFromItemFix) \
    X(free_addy,               "Gameplay patches (damage/speed caps)",   1, FEATURE_SERVER_GATED | FEATURE_LIVE,    gameplay, none,          1, AttachFreeAddy) \
    X(ext_chain_lightning,     "Chain Lightning range and damage",       1, FEATURE_SERVER_GATED | FEATURE_LIVE,    gameplay, none,          1, AttachExtChainLightning) \
    X(map_info_tool_tip,       "World map mob/NPC tooltips",             1, 0,                                      ui,       none,          1, AttachMapInfoToolTip) \
    X(quest_list_color,        "Resident quests in blue",                1, FEATURE_LIVE,                           botstory, none,          1, AttachQuestListColorMod) \
    X(server_packets,          "BotStory server packets, feature handshake", 1, FEATURE_CORE,                       core,     none,          1, AttachServerPackets) \
    X(crew_window,             "Crew window",                            1, 0,                                      botstory, none,          1, AttachCrewWindowMod) \
    X(set_items,               "Set item tooltips",                      1, FEATURE_LIVE,                           botstory, tool_tip,      1, AttachSetItemsMod)

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
