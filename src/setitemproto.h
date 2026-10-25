#pragma once
#include "crewproto.h"
#include <string>
#include <vector>

// SET_ITEM_SYNC, the server's set item packet (BotStory server server.set.SetItemSync, opcode 0x186 in
// docs/CUSTOM_OPCODES.md), without any game code, so test/setitemproto_test.cpp can check it against the bytes the
// server's SetItemSyncTest decodes.
//
// byte protocol (1), short sets, then per set: string name, short total (its highest tier: the slots that can count),
// short pieces (worn), short members each int itemId + byte worn, short tiers each short pieces + short stats each
// byte stat + int value. Always the whole list, never a change. Bytes after a well-formed body are ignored.

namespace setitemproto {

constexpr int kProtocol = 1;

// The stats the server applies from a set (Character.recalcEquipStats); a code the client does not know is kept and
// not drawn, so the server can add one without breaking older clients.
enum Stat : int { STAT_ALL_STATS = 1, STAT_WATK = 2, STAT_MATK = 3, STAT_HP = 4, STAT_MP = 5 };

struct Member {
    int itemId = 0;
    bool worn = false;
};

struct StatValue {
    int stat = 0;
    int value = 0;
};

struct Tier {
    int pieces = 0;
    std::vector<StatValue> stats;
};

struct Set {
    std::string name;
    int total = 0;
    int pieces = 0;
    std::vector<Member> members;
    std::vector<Tier> tiers;

    bool Has(int itemId) const {
        for (const Member& m : members) {
            if (m.itemId == itemId) return true;
        }
        return false;
    }
};

// Caps against a malformed packet; the server's largest set has 14 members and 7 tiers.
constexpr int kMaxSets = 256;
constexpr int kMaxMembers = 64;
constexpr int kMaxTiers = 16;
constexpr int kMaxStats = 16;

inline const char* StatLabel(int stat) {
    switch (stat) {
    case STAT_ALL_STATS: return "All Stats";
    case STAT_WATK: return "Weapon Attack";
    case STAT_MATK: return "Magic Attack";
    case STAT_HP: return "Max HP";
    case STAT_MP: return "Max MP";
    default: return nullptr;
    }
}

// False (and out untouched) for a packet of another protocol or one that does not decode.
inline bool ReadSync(const unsigned char* p, size_t n, std::vector<Set>& out) {
    crewproto::Reader r{p, n};
    if (r.u8() != kProtocol) {
        return false;
    }
    int count = r.u16();
    if (!r.ok || count > kMaxSets) {
        return false;
    }
    std::vector<Set> sets;
    sets.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count && r.ok; ++i) {
        Set s;
        s.name = r.str();
        s.total = r.u16();
        s.pieces = r.u16();
        int members = r.u16();
        if (members > kMaxMembers) return false;
        for (int m = 0; m < members && r.ok; ++m) {
            Member member;
            member.itemId = r.i32();
            member.worn = r.u8() != 0;
            s.members.push_back(member);
        }
        int tiers = r.u16();
        if (tiers > kMaxTiers) return false;
        for (int t = 0; t < tiers && r.ok; ++t) {
            Tier tier;
            tier.pieces = r.u16();
            int stats = r.u16();
            if (stats > kMaxStats) return false;
            for (int k = 0; k < stats && r.ok; ++k) {
                StatValue sv;
                sv.stat = r.u8();
                sv.value = r.i32();
                tier.stats.push_back(sv);
            }
            s.tiers.push_back(std::move(tier));
        }
        sets.push_back(std::move(s));
    }
    if (!r.ok) {
        return false;
    }
    out = std::move(sets);
    return true;
}

// The first set the item is a member of, or null.
inline const Set* FindSet(const std::vector<Set>& sets, int itemId) {
    for (const Set& s : sets) {
        if (s.Has(itemId)) return &s;
    }
    return nullptr;
}

}  // namespace setitemproto
