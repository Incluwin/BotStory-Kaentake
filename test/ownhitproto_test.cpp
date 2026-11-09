// Checks src/ownhitproto.h, the own hit packet, outside the game.
// Build and run (x86 Developer prompt):  build_ownhitproto.cmd
// The bytes are the ones the server's OwnHitTest (BotStory server) writes after the opcode 0x189.
#include "ownhitproto.h"
#include <cstdio>
#include <vector>

using namespace ownhitproto;

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
    Bytes& i32(int v) { for (int i = 0; i < 4; ++i) u8((static_cast<unsigned int>(v) >> (8 * i)) & 0xFF); return *this; }
};

// OwnHitTest.pvpHitWithASkillSound: 1234 from a Slash Blast (1001005), the attacker on your left
static void ReadsAPvpHitWithItsSkillSound() {
    Bytes p;
    p.i32(1234).u8(SKILL_SOUND).u8(1).i32(1001005);
    Hit hit;
    CHECK(ReadHit(p.b.data(), p.b.size(), hit));
    CHECK(hit.damage == 1234 && !hit.miss && hit.direction == 1);
    CHECK(hit.skillId == 1001005 && hit.mobTemplateId == 0);
    CHECK(ShownDelta(hit) == -1234);
    CHECK(SkillSoundId(hit) == 1001005);
    CHECK(MobSoundType(hit) == 0);
}

// OwnHitTest.bossHitWithItsCharDamSound: Zakum's arm (8800003) attack 2 (index 1), 5000
static void ReadsABossHitWithItsCharDamSound() {
    Bytes p;
    p.i32(5000).u8(MOB_SOUND).u8(0).i32(8800003).u8(1);
    Hit hit;
    CHECK(ReadHit(p.b.data(), p.b.size(), hit));
    CHECK(hit.damage == 5000 && hit.mobTemplateId == 8800003 && hit.mobAttackIndex == 1);
    CHECK(MobSoundType(hit) == 12); // CharDam2
    CHECK(SkillSoundId(hit) == 0);
    hit.mobAttackIndex = 0;
    CHECK(MobSoundType(hit) == 11); // CharDam1
    hit.mobAttackIndex = 8;
    CHECK(MobSoundType(hit) == 19); // CharDamF
    hit.mobAttackIndex = 9;
    CHECK(MobSoundType(hit) == 0);
    hit.mobAttackIndex = -1; // a body touch has no CharDam
    CHECK(MobSoundType(hit) == 0);
}

// OwnHitTest.missIsDamageZeroWithTheMissFlag
static void AMissDrawsMissAndPlaysNoSound() {
    Bytes p;
    p.i32(0).u8(MISS | SKILL_SOUND).u8(0).i32(2001004);
    Hit hit;
    CHECK(ReadHit(p.b.data(), p.b.size(), hit));
    CHECK(hit.miss && hit.damage == 0 && ShownDelta(hit) == 0);
    CHECK(SkillSoundId(hit) == 0);

    Bytes q; // damage <= 0 without the flag is a MISS too, as DAMAGE_PLAYER's 0
    q.i32(-5).u8(0).u8(0);
    CHECK(ReadHit(q.b.data(), q.b.size(), hit));
    CHECK(hit.miss && ShownDelta(hit) == 0);
}

static void ShortBodiesAreRefusedAndExtraBytesIgnored() {
    Hit hit;
    Bytes header;
    header.i32(77).u8(0);
    CHECK(!ReadHit(header.b.data(), header.b.size(), hit)); // no direction byte
    CHECK(!ReadHit(nullptr, 0, hit));
    Bytes mob;
    mob.i32(77).u8(MOB_SOUND).u8(0).i32(100100); // no attack index
    CHECK(!ReadHit(mob.b.data(), mob.b.size(), hit));
    Bytes skill;
    skill.i32(77).u8(SKILL_SOUND).u8(0).u8(1).u8(2);
    CHECK(!ReadHit(skill.b.data(), skill.b.size(), hit));
    Bytes later; // a flag and bytes a later server appends
    later.i32(77).u8(0x80).u8(0).i32(42).u8(9);
    CHECK(ReadHit(later.b.data(), later.b.size(), hit));
    CHECK(hit.damage == 77 && !hit.miss && hit.skillId == 0 && hit.mobTemplateId == 0);
}

static void BothSoundsReadInOrder() {
    Bytes p;
    p.i32(300).u8(MOB_SOUND | SKILL_SOUND).u8(1).i32(9400121).u8(0).i32(4101004);
    Hit hit;
    CHECK(ReadHit(p.b.data(), p.b.size(), hit));
    CHECK(hit.mobTemplateId == 9400121 && hit.mobAttackIndex == 0 && hit.skillId == 4101004);
}

int main() {
    ReadsAPvpHitWithItsSkillSound();
    ReadsABossHitWithItsCharDamSound();
    AMissDrawsMissAndPlaysNoSound();
    ShortBodiesAreRefusedAndExtraBytesIgnored();
    BothSoundsReadInOrder();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ownhitproto: all checks pass\n");
    return 0;
}
