// Checks src/setitemproto.h, the set item tooltips' packet, outside the game.
// Build and run (x86 Developer prompt):  build_setitemproto.cmd
// The bytes are the ones the server's SetItemSyncTest (BotStory server) encodes and decodes.
#include "setitemproto.h"
#include <cstdio>
#include <cstring>

using namespace setitemproto;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

struct Bytes {
    std::vector<unsigned char> b;
    Bytes& u8(int v) { b.push_back(static_cast<unsigned char>(v)); return *this; }
    Bytes& u16(int v) { u8(v & 0xFF); return u8((v >> 8) & 0xFF); }
    Bytes& i32(int v) { for (int i = 0; i < 4; ++i) u8((static_cast<unsigned int>(v) >> (8 * i)) & 0xFF); return *this; }
    Bytes& str(const char* s) { size_t n = std::strlen(s); u16(static_cast<int>(n)); b.insert(b.end(), s, s + n); return *this; }
};

// SetItemSyncTest.packetListsMembersWornFlagsAndOnlyTheAppliedTierStats, after the opcode 0x186: Timeless Warrior,
// hat + overall worn, gloves not; 2 pieces: all stats 15, WATK 5, MATK 5; 3 pieces: all stats 10, HP 100.
static Bytes ServerTimeless() {
    Bytes p;
    p.u8(1).u16(1)
        .str("Timeless Warrior").u16(3).u16(2)
        .u16(3).i32(1002776).u8(1).i32(1052155).u8(1).i32(1082234).u8(0)
        .u16(2)
        .u16(2).u16(3).u8(1).i32(15).u8(2).i32(5).u8(3).i32(5)
        .u16(3).u16(2).u8(1).i32(10).u8(4).i32(100);
    return p;
}

static void ReadsTheServersSet() {
    Bytes p = ServerTimeless();
    std::vector<Set> sets;
    CHECK(ReadSync(p.b.data(), p.b.size(), sets));
    CHECK(sets.size() == 1);
    if (sets.size() != 1) return;
    const Set& s = sets[0];
    CHECK(s.name == "Timeless Warrior");
    CHECK(s.total == 3 && s.pieces == 2);
    CHECK(s.members.size() == 3);
    CHECK(s.members[0].itemId == 1002776 && s.members[0].worn);
    CHECK(s.members[2].itemId == 1082234 && !s.members[2].worn);
    CHECK(s.tiers.size() == 2);
    CHECK(s.tiers[0].pieces == 2 && s.tiers[0].stats.size() == 3);
    CHECK(s.tiers[0].stats[1].stat == STAT_WATK && s.tiers[0].stats[1].value == 5);
    CHECK(s.tiers[1].pieces == 3 && s.tiers[1].stats[1].stat == STAT_HP && s.tiers[1].stats[1].value == 100);
    CHECK(FindSet(sets, 1052155) == &sets[0]);
    CHECK(FindSet(sets, 1302000) == nullptr);
}

// SetItemSyncTest.anEmptySetListIsJustTheHeader
static void AnEmptyListClearsTheSets() {
    Bytes full = ServerTimeless();
    std::vector<Set> sets;
    CHECK(ReadSync(full.b.data(), full.b.size(), sets) && sets.size() == 1);
    Bytes empty;
    empty.u8(1).u16(0);
    CHECK(ReadSync(empty.b.data(), empty.b.size(), sets));
    CHECK(sets.empty());
}

static void ABadPacketKeepsTheOldSets() {
    Bytes full = ServerTimeless();
    std::vector<Set> sets;
    CHECK(ReadSync(full.b.data(), full.b.size(), sets));
    for (size_t cut = 0; cut < full.b.size(); ++cut) {
        CHECK(!ReadSync(full.b.data(), cut, sets));
    }
    Bytes other;
    other.u8(2).u16(0);
    CHECK(!ReadSync(other.b.data(), other.b.size(), sets));
    Bytes huge;
    huge.u8(1).u16(1).str("x").u16(1).u16(0).u16(kMaxMembers + 1);
    CHECK(!ReadSync(huge.b.data(), huge.b.size(), sets));
    CHECK(sets.size() == 1 && sets[0].name == "Timeless Warrior");
}

static void TrailingBytesAreIgnored() {
    Bytes p = ServerTimeless();
    p.u8(0x7F).u16(9);
    std::vector<Set> sets;
    CHECK(ReadSync(p.b.data(), p.b.size(), sets) && sets.size() == 1);
}

static void KnowsTheAppliedStatsOnly() {
    CHECK(std::strcmp(StatLabel(STAT_ALL_STATS), "All Stats") == 0);
    CHECK(std::strcmp(StatLabel(STAT_MP), "Max MP") == 0);
    CHECK(StatLabel(0) == nullptr && StatLabel(99) == nullptr);
}

int main() {
    ReadsTheServersSet();
    AnEmptyListClearsTheSets();
    ABadPacketKeepsTheOldSets();
    TrailingBytesAreIgnored();
    KnowsTheAppliedStatsOnly();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("setitemproto: all checks passed\n");
    return 0;
}
