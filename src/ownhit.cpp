#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "features.h"
#include "ownhitproto.h"
#include "serverpackets.h"
#include "v83_offsets.h"

// The module own_hit: the damage number (or MISS) and the hit sound over your own character when the server applied a
// hit to you: a PvP duel hit, or a server-run boss's attack (S->C 0x189 PVP_HIT, server.combat.OwnHit). The stock
// client draws nothing for a DAMAGE_PLAYER about yourself, so without this you only see your HP bar drop.
//
// It calls the routine CUserLocal::SetDamaged and CUserRemote::OnHit draw their numbers with, and the mob and skill
// sound players SetDamaged uses (addresses and evidence: v83_offsets.h, namespace ownhit). Display only: no knockback,
// no invincibility, no HP change (the server already sent the stat), no TAKE_DAMAGE. The packet is read on the game
// thread inside CClientSocket::ProcessPacket, where CUserRemote::OnHit draws its numbers too.

namespace {

using namespace v83::ownhit;

using UserShowHP_t = void(__thiscall*)(void* pUser, int n);
using PlayMobSound_t = void(__cdecl*)(int nTemplateID, int nType, int nVolume);
using PlaySkillSound_t = int(__cdecl*)(int nSkillID, int nType);

void* Singleton(uintptr_t uAddress) {
    return *reinterpret_cast<void* const*>(uAddress);
}

// The game's calls, each in its own frame: a fault or a game exception (a missing WZ node) in one is caught here, so
// a bad packet costs one number or sound and never the game.
bool ShowNumber(void* pUser, int n) {
    __try {
        reinterpret_cast<UserShowHP_t>(kUserShowHP)(pUser, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PlayMobSound(int nTemplateID, int nType) {
    __try {
        reinterpret_cast<PlayMobSound_t>(kPlayMobSound)(nTemplateID, nType, kOwnVolume);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PlaySkillSound(int nSkillID) {
    __try {
        reinterpret_cast<PlaySkillSound_t>(kPlaySkillSound)(nSkillID, kSkillSoundHit);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Draw(const ownhitproto::Hit& hit) {
    // 0 between fields (the CUserLocal dtor clears it), and the display singleton lives as long as the game
    void* pUser = Singleton(kUserLocalInstance);
    if (!pUser || !Singleton(kAnimationDisplayer)) {
        return;
    }
    if (!ShowNumber(pUser, ownhitproto::ShownDelta(hit))) {
        LOG_ONCE("[own_hit] drawing a hit number failed (damage %d); later failures are not logged", hit.damage);
        return;
    }
    LOG_ONCE("[own_hit] first hit drawn: damage %d, miss %d, mob %d, skill %d", hit.damage, hit.miss ? 1 : 0,
             hit.mobTemplateId, hit.skillId);
    int nMobSound = ownhitproto::MobSoundType(hit);
    if (nMobSound && !PlayMobSound(hit.mobTemplateId, nMobSound)) {
        LOG_ONCE_PER_ID(hit.mobTemplateId, "[own_hit] mob %d's hit sound failed", hit.mobTemplateId);
    }
    int nSkill = ownhitproto::SkillSoundId(hit);
    if (nSkill && !PlaySkillSound(nSkill)) {
        LOG_ONCE_PER_ID(nSkill, "[own_hit] skill %d's hit sound failed", nSkill);
    }
}

void OnPvpHit(const unsigned char* pBody, size_t uLength) {
    if (!features::IsActive(FeatureId::own_hit)) {
        return;
    }
    ownhitproto::Hit hit;
    if (!ownhitproto::ReadHit(pBody, uLength, hit)) {
        LOG_ONCE("[own_hit] a PVP_HIT of %u bytes is shorter than its flags say; not drawn", static_cast<unsigned>(uLength));
        return;
    }
    Draw(hit);
}

#ifdef KAENTAKE_OWN_HIT_PROBE
// Test builds only, never shipped: a DAMAGE_PLAYER (0xC0) about config.ini [own_hit] probe_cid is drawn as an own hit
// too, so a server without PVP_HIT can check the drawing with !pvp probe self [src] [mobId]. The game still gets the
// packet (and drops it: you are not a remote user).
unsigned int g_uProbeCid = 0;

void OnDamagePlayerProbe(const unsigned char* pBody, size_t uLength) {
    if (!features::IsActive(FeatureId::own_hit)) {
        return;
    }
    size_t off = 0;
    auto read = [&](void* out, size_t size) {
        if (off > uLength || uLength - off < size) {
            return false;
        }
        memcpy(out, pBody + off, size);
        off += size;
        return true;
    };
    unsigned int cid = 0;
    signed char src = 0;
    int damage = 0, unused = 0, mobId = 0;
    unsigned char direction = 0;
    if (!read(&cid, 4) || !read(&src, 1) || (src == -3 && !read(&unused, 4)) || !read(&damage, 4)) {
        return;
    }
    if (src != -4 && (!read(&mobId, 4) || !read(&direction, 1))) {
        return;
    }
    LogMessage("[own_hit] probe: DAMAGE_PLAYER cid %u src %d damage %d mob %d", cid, src, damage, mobId);
    if (cid != g_uProbeCid) {
        return;
    }
    ownhitproto::Hit hit;
    hit.miss = damage <= 0;
    hit.damage = hit.miss ? 0 : damage;
    hit.direction = direction;
    hit.mobTemplateId = src >= 0 ? mobId : 0;
    hit.mobAttackIndex = src;
    Draw(hit);
}
#endif

}  // namespace

void AttachOwnHitMod() {
    serverpackets::OnRecv(kRecvOpcode, &OnPvpHit);
#ifdef KAENTAKE_OWN_HIT_PROBE
    char sPath[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, sPath, MAX_PATH);
    while (n > 0 && sPath[n - 1] != '\\' && sPath[n - 1] != '/') {
        --n;
    }
    strcpy_s(sPath + n, MAX_PATH - n, "config.ini"); // beside MapleStory.exe, as the other modules read it
    g_uProbeCid = GetPrivateProfileIntA("own_hit", "probe_cid", 0, sPath);
    LogMessage("[own_hit] PROBE BUILD: DAMAGE_PLAYER about cid %u is drawn as an own hit", g_uProbeCid);
    serverpackets::OnRecvBefore(0xC0, &OnDamagePlayerProbe);
#endif
}
