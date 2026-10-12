#pragma once
#include <windows.h>
#include <string>
#include <vector>

// The native Crew window protocol (BotStory server docs/BOT_MANAGEMENT_P10_PLAN.md), without any game code, so
// tools/crewproto_test.cpp can check it against the server's own encoder output.

namespace crewproto {

enum ClientOp : unsigned char { OP_HELLO = 0, OP_OPEN = 1, OP_CLOSE = 2, OP_ACTION = 3 };
enum ServerType : unsigned char { SV_WELCOME = 0, SV_SNAPSHOT = 1, SV_CLOSE = 2, SV_OPEN = 3 };
enum Verb : unsigned char {
    VERB_CARD = 1, VERB_CALL = 2, VERB_BENCH = 3, VERB_SWAP = 4, VERB_USUAL = 5, VERB_MORE = 6,
    VERB_FOLLOW = 7, VERB_WAIT = 8, VERB_GATHER = 9, VERB_REBUFF = 10,
};
enum RowFlag : int {
    ROW_CALL = 1, ROW_BENCH = 2, ROW_SWAP_OUT = 4, ROW_SWAP_IN = 8, ROW_GUEST = 16, ROW_COMMITTED = 32,
    ROW_OUT = 64,       // out with the owner: one a SWAP_IN row may take over from
    ROW_CAN_COME = 128, // at camp and can come at once: one a SWAP_OUT row may make room for
};
enum SnapFlag : int { SNAP_USUAL = 1, SNAP_SOMEONE_OUT = 2 };

struct Row {
    std::string id;        // the server's persona id, sent back as is
    int version = 0;
    std::wstring name;
    std::wstring levelJob;
    std::wstring detail;
    int hp = 255;          // 0-100, 255 = not known
    int flags = 0;
};

struct Snapshot {
    bool valid = false;
    int ownerVersion = 0;
    int seats = 0;
    int partyCap = 6;
    int benchSeats = 0;
    std::wstring seatsNote;
    int flags = 0;
    std::vector<Row> party;
    std::vector<Row> bench;
};

// A bounded little-endian reader over a received packet's body. Any read past the end sets ok=false and returns zero.
struct Reader {
    const unsigned char* p;
    size_t n;
    size_t off = 0;
    bool ok = true;

    bool need(size_t k) {
        if (!ok || n - off < k || off > n) {
            ok = false;
        }
        return ok;
    }
    int u8() {
        if (!need(1)) return 0;
        return p[off++];
    }
    int u16() {
        if (!need(2)) return 0;
        int v = p[off] | (p[off + 1] << 8);
        off += 2;
        return v;
    }
    int i32() {
        if (!need(4)) return 0;
        unsigned int v = p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | (static_cast<unsigned int>(p[off + 3]) << 24);
        off += 4;
        return static_cast<int>(v);
    }
    std::string str() {
        int len = u16();
        if (!need(static_cast<size_t>(len))) return {};
        std::string s(reinterpret_cast<const char*>(p + off), static_cast<size_t>(len));
        off += static_cast<size_t>(len);
        return s;
    }
};

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline bool ReadRow(Reader& r, Row& row) {
    row.id = r.str();
    row.version = r.i32();
    row.name = widen(r.str());
    row.levelJob = widen(r.str());
    row.detail = widen(r.str());
    row.hp = r.u8();
    row.flags = r.u8();
    return r.ok;
}

inline bool ReadSnapshot(Reader& r, Snapshot& s) {
    s.ownerVersion = r.i32();
    s.seats = r.u8();
    s.partyCap = r.u8();
    s.benchSeats = r.u8();
    s.seatsNote = widen(r.str());
    s.flags = r.u8();
    int nParty = r.u8();
    if (!r.ok || nParty > 16) return false;
    s.party.resize(static_cast<size_t>(nParty));
    for (auto& row : s.party) {
        if (!ReadRow(r, row)) return false;
    }
    int nBench = r.u8();
    if (!r.ok || nBench > 64) return false;
    s.bench.resize(static_cast<size_t>(nBench));
    for (auto& row : s.bench) {
        if (!ReadRow(r, row)) return false;
    }
    s.valid = true;
    return r.ok;
}

}  // namespace crewproto
