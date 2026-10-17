#pragma once
#include "crewproto.h"
#include <string>
#include <vector>

// The login feature handshake's wire format (BotStory server docs/CLIENT_FEATURE_HANDSHAKE.md, "Protocol"), without
// any game code, so test/featureproto_test.cpp can check it against the server's own encoder output.
//
// client -> server 0x118: byte op (REPORT 0 | APPLIED 1), short protocol, int dllBuild, short count, then per module
//                         string id, string title, short version, byte flags (feature_list.h), byte state
// server -> client 0x17B: byte type (POLICY 0), short protocol, short count, then per module string id, byte decision
// Each packet leads with its protocol; bytes after a well-formed body are ignored (a later protocol may add some).

namespace featureproto {

enum ClientOp : unsigned char { OP_REPORT = 0, OP_APPLIED = 1 };
enum ServerType : unsigned char { SV_POLICY = 0 };
enum Decision : int { DECISION_ALLOW = 0, DECISION_DENY = 1, DECISION_REQUIRE = 2 };
enum State : int {
    STATE_ATTACHED = 1,
    STATE_ACTIVE = 2, // attached and not inert
    STATE_VETOED = 4, // config.ini inert= names it
};
constexpr int kFlagsSent = 0x07; // FEATURE_CORE | FEATURE_SERVER_GATED | FEATURE_LIVE: what the server reads

struct Feature {
    std::string id;
    std::string title;
    int version = 1;
    int flags = 0;
    int state = 0;
};

struct Answer {
    std::string id;
    int decision = DECISION_ALLOW;
};

struct Policy {
    bool valid = false;
    int protocol = 0;
    std::vector<Answer> answers;
};

// The body after the opcode (COutPacket adds the opcode itself).
inline std::vector<unsigned char> EncodeReport(unsigned char op, int protocol, int dllBuild,
                                               const std::vector<Feature>& features) {
    std::vector<unsigned char> b;
    auto u8 = [&](int v) { b.push_back(static_cast<unsigned char>(v)); };
    auto u16 = [&](int v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); };
    auto i32 = [&](int v) { for (int i = 0; i < 4; ++i) u8((static_cast<unsigned int>(v) >> (8 * i)) & 0xFF); };
    auto str = [&](const std::string& s) {
        size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        u16(static_cast<int>(n));
        b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    };
    u8(op);
    u16(protocol);
    i32(dllBuild);
    u16(static_cast<int>(features.size()));
    for (const auto& f : features) {
        str(f.id);
        str(f.title);
        u16(f.version);
        u8(f.flags & kFlagsSent);
        u8(f.state);
    }
    return b;
}

// A POLICY body (after the opcode). False when it is short, another type, or a list longer than any server sends.
inline bool ReadPolicy(const unsigned char* p, size_t n, Policy& out) {
    crewproto::Reader r{p, n};
    int type = r.u8();
    out.protocol = r.u16();
    int count = r.u16();
    if (!r.ok || type != SV_POLICY || out.protocol < 1 || count > 255) return false;
    out.answers.resize(static_cast<size_t>(count));
    for (auto& a : out.answers) {
        a.id = r.str();
        a.decision = r.u8();
        if (!r.ok || a.id.empty() || a.decision > DECISION_REQUIRE) return false;
    }
    out.valid = true;
    return true;
}

}  // namespace featureproto
