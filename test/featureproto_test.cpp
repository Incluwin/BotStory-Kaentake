// Checks src/featureproto.h, the login feature handshake's packets, outside the game.
// Build and run (x86 Developer prompt):  cl /nologo /EHsc /std:c++17 /I..\src featureproto_test.cpp && featureproto_test.exe
// The bytes are the ones the server's FeaturePacketsTest (BotStory server) decodes and encodes.
#include "featureproto.h"
#include <cstdio>
#include <cstring>

using namespace featureproto;

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

// FeaturePacketsTest.sampleReport(0) on the server, written by hand.
static Bytes ServerSampleReport() {
    Bytes p;
    p.u8(0).u16(1).i32(20261017).u16(3)
        .str("mob_hp_tag").str("Mob HP bar with numbers").u16(1).u8(0x04).u8(0x03)
        .str("crew_window").str("Crew window").u16(1).u8(0x00).u8(0x03)
        .str("free_addy").str("Gameplay patches (damage/speed caps)").u16(2).u8(0x06).u8(0x01);
    return p;
}

static void EncodesTheServersSampleReport() {
    std::vector<Feature> v = {
        {"mob_hp_tag", "Mob HP bar with numbers", 1, 0x04, STATE_ATTACHED | STATE_ACTIVE},
        {"crew_window", "Crew window", 1, 0x00, STATE_ATTACHED | STATE_ACTIVE},
        {"free_addy", "Gameplay patches (damage/speed caps)", 2, 0x06 | 0x08 | 0x10, STATE_ATTACHED},
    };
    // EARLY/DEFERRED flags are not sent: the server reads core, server_gated and live only
    CHECK(EncodeReport(OP_REPORT, 1, 20261017, v) == ServerSampleReport().b);
    std::vector<unsigned char> applied = EncodeReport(OP_APPLIED, 1, 20261017, v);
    CHECK(applied.size() == ServerSampleReport().b.size() && applied[0] == 1);
    CHECK(EncodeReport(OP_REPORT, 1, 0, {}) == (Bytes().u8(0).u16(1).i32(0).u16(0).b));
}

// FeaturePacketsTest.aPolicyIsItsHeaderAndOneDecisionPerFeature, after the opcode 0x17B.
static Bytes ServerPolicy() {
    Bytes p;
    p.u8(0).u16(1).u16(3).str("mob_hp_tag").u8(1).str("crew_window").u8(0).str("free_addy").u8(2);
    return p;
}

static void ReadsTheServersPolicy() {
    Bytes p = ServerPolicy();
    // the server test spells out these first bytes after the opcode
    const unsigned char head[] = {0x00, 0x01, 0x00, 0x03, 0x00, 0x0A, 0x00, 'm', 'o', 'b'};
    CHECK(std::memcmp(p.b.data(), head, sizeof(head)) == 0);
    Policy pol;
    CHECK(ReadPolicy(p.b.data(), p.b.size(), pol));
    CHECK(pol.valid && pol.protocol == 1 && pol.answers.size() == 3);
    CHECK(pol.answers[0].id == "mob_hp_tag" && pol.answers[0].decision == DECISION_DENY);
    CHECK(pol.answers[1].id == "crew_window" && pol.answers[1].decision == DECISION_ALLOW);
    CHECK(pol.answers[2].id == "free_addy" && pol.answers[2].decision == DECISION_REQUIRE);

    Bytes empty;
    empty.u8(0).u16(1).u16(0);
    Policy none;
    CHECK(ReadPolicy(empty.b.data(), empty.b.size(), none) && none.answers.empty());

    Bytes later = ServerPolicy();
    later.u8(0xEE).i32(-1); // a later protocol's extra bytes
    Policy longer;
    CHECK(ReadPolicy(later.b.data(), later.b.size(), longer) && longer.answers.size() == 3);
}

static void RejectsEveryTruncationAndBadValue() {
    Bytes p = ServerPolicy();
    for (size_t cut = 0; cut < p.b.size(); ++cut) {
        Policy pol;
        CHECK(!ReadPolicy(p.b.data(), cut, pol));
    }
    Policy pol;
    Bytes otherType;
    otherType.u8(1).u16(1).u16(0);
    CHECK(!ReadPolicy(otherType.b.data(), otherType.b.size(), pol));
    Bytes protocol0;
    protocol0.u8(0).u16(0).u16(0);
    CHECK(!ReadPolicy(protocol0.b.data(), protocol0.b.size(), pol));
    Bytes tooMany;
    tooMany.u8(0).u16(1).u16(256);
    CHECK(!ReadPolicy(tooMany.b.data(), tooMany.b.size(), pol));
    Bytes badDecision;
    badDecision.u8(0).u16(1).u16(1).str("mob_hp_tag").u8(3);
    CHECK(!ReadPolicy(badDecision.b.data(), badDecision.b.size(), pol));
    Bytes emptyId;
    emptyId.u8(0).u16(1).u16(1).str("").u8(0);
    CHECK(!ReadPolicy(emptyId.b.data(), emptyId.b.size(), pol));
    Bytes pastTheEnd;
    pastTheEnd.u8(0).u16(1).u16(1).u16(500);
    CHECK(!ReadPolicy(pastTheEnd.b.data(), pastTheEnd.b.size(), pol));
}

int main() {
    EncodesTheServersSampleReport();
    ReadsTheServersPolicy();
    RejectsEveryTruncationAndBadValue();
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
