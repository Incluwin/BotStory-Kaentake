#pragma once
#include <cstddef>
#include <cstring>

// PVP_HIT, the server's "a hit landed on you" packet (BotStory server server.combat.OwnHit, opcode 0x189 in
// docs/CUSTOM_OPCODES.md), without any game code, so test/ownhitproto_test.cpp can check it against the bytes the
// server's OwnHitTest writes.
//
// int damage (the HP the hit took), byte flags, byte direction (0: the attacker stands right of you, 1: left, as
// DAMAGE_PLAYER's; read and not used, since the hit has no knockback), then MOB_SOUND: int mobTemplateId +
// byte attackIndex (0 = attack1), then SKILL_SOUND: int skillId. A flag this client does not know is ignored, and
// bytes after a well-formed body are ignored, so the server can append.

namespace ownhitproto {

enum Flag : unsigned char {
    MISS = 0x01,        // draw MISS (also drawn for damage <= 0)
    MOB_SOUND = 0x02,   // the mob's CharDam sound: Sound/Mob.img/<id>/CharDam1, CharDam2 ... by attack index
    SKILL_SOUND = 0x04, // the attacker's skill hit sound: Sound/Skill.img/<id>/Hit
};

struct Hit {
    int damage = 0;
    bool miss = false;
    int direction = 0;
    int mobTemplateId = 0; // 0: no mob sound
    int mobAttackIndex = -1;
    int skillId = 0;       // 0: no skill sound
};

namespace detail {
inline bool Read(const unsigned char* p, size_t n, size_t& off, void* out, size_t size) {
    if (off > n || n - off < size) {
        return false;
    }
    std::memcpy(out, p + off, size);
    off += size;
    return true;
}
}  // namespace detail

// False when the body is shorter than its flags say; hit is then not to be drawn.
inline bool ReadHit(const unsigned char* p, size_t n, Hit& hit) {
    hit = Hit{};
    if (!p) {
        return false;
    }
    size_t off = 0;
    int damage = 0;
    unsigned char flags = 0, direction = 0;
    if (!detail::Read(p, n, off, &damage, 4) || !detail::Read(p, n, off, &flags, 1)
        || !detail::Read(p, n, off, &direction, 1)) {
        return false;
    }
    hit.miss = (flags & MISS) != 0 || damage <= 0;
    hit.damage = hit.miss ? 0 : damage;
    hit.direction = direction;
    if (flags & MOB_SOUND) {
        int id = 0;
        signed char index = 0;
        if (!detail::Read(p, n, off, &id, 4) || !detail::Read(p, n, off, &index, 1)) {
            return false;
        }
        hit.mobTemplateId = id;
        hit.mobAttackIndex = index;
    }
    if (flags & SKILL_SOUND) {
        int id = 0;
        if (!detail::Read(p, n, off, &id, 4)) {
            return false;
        }
        hit.skillId = id;
    }
    return true;
}

// What CUserLocal::SetDamaged hands play_mob_sound for this hit (attack index + 11: 11 CharDam1, 12 CharDam2 ...
// 19 CharDamF), or 0 for no mob sound: a miss, no mob, or an index past the CharDam names.
inline int MobSoundType(const Hit& hit) {
    if (hit.miss || hit.mobTemplateId <= 0 || hit.mobAttackIndex < 0 || hit.mobAttackIndex > 8) {
        return 0;
    }
    return 11 + hit.mobAttackIndex;
}

// The skill whose Hit sound to play, or 0 for none (a miss makes no hit sound).
inline int SkillSoundId(const Hit& hit) {
    return hit.miss || hit.skillId <= 0 ? 0 : hit.skillId;
}

// The argument for CUser's HP-number routine: minus the damage draws violet hit digits, 0 draws MISS.
inline int ShownDelta(const Hit& hit) {
    return hit.miss ? 0 : -hit.damage;
}

}  // namespace ownhitproto
