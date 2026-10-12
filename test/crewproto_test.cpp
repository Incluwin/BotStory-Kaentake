// Checks src/crewproto.h, the Crew window packet reader, outside the game.
// Build and run (x86 Developer prompt):  cl /nologo /EHsc /std:c++17 /I..\src crewproto_test.cpp && crewproto_test.exe
// The golden SNAPSHOT bytes come from the server's encoder (NativeCrewPacketsTest in the BotStory server).
#include "crewproto.h"
#include <cstdio>
#include <cstdlib>

using namespace crewproto;

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
    void u8(int v) { b.push_back(static_cast<unsigned char>(v)); }
    void u16(int v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
    void i32(int v) { for (int i = 0; i < 4; ++i) u8((static_cast<unsigned int>(v) >> (8 * i)) & 0xFF); }
    void str(const char* s) { size_t n = std::strlen(s); u16(static_cast<int>(n)); b.insert(b.end(), s, s + n); }
    void row(const char* id, int version, const char* name, const char* levelJob, const char* detail, int hp, int flags) {
        str(id); i32(version); str(name); str(levelJob); str(detail); u8(hp); u8(flags);
    }
};

static Bytes SampleSnapshot() {
    Bytes p;
    p.i32(77);         // ownerVersion
    p.u8(4);           // seats
    p.u8(6);           // partyCap
    p.u8(6);           // benchSeats
    p.str("(1 seat kept)");
    p.u8(SNAP_USUAL | SNAP_SOMEONE_OUT);
    p.u8(1);
    p.row("halvard", 12, "Halvard", "Lv52 Crusader", "Perion - HP 90%", 90, ROW_BENCH | ROW_SWAP_OUT);
    p.u8(2);
    p.row("lioren", 3, "Lioren", "Lv47 Wizard", "train @ Ant Tunnel I - 12% EXP banked", 255, ROW_CALL | ROW_SWAP_IN);
    p.row("hire:1234", -5, "Anvith", "Lv30 Hunter", "rest @ Henesys", 255, ROW_CALL);
    return p;
}

static void ParsesASnapshot() {
    Bytes p = SampleSnapshot();
    Reader r{p.b.data(), p.b.size()};
    Snapshot s;
    CHECK(ReadSnapshot(r, s));
    CHECK(s.valid);
    CHECK(s.ownerVersion == 77);
    CHECK(s.seats == 4 && s.partyCap == 6 && s.benchSeats == 6);
    CHECK(s.seatsNote == L"(1 seat kept)");
    CHECK(s.flags == (SNAP_USUAL | SNAP_SOMEONE_OUT));
    CHECK(s.party.size() == 1 && s.bench.size() == 2);
    CHECK(s.party[0].id == "halvard" && s.party[0].version == 12 && s.party[0].hp == 90);
    CHECK(s.party[0].name == L"Halvard" && s.party[0].levelJob == L"Lv52 Crusader");
    CHECK(s.party[0].detail == L"Perion - HP 90%");
    CHECK(s.party[0].flags == (ROW_BENCH | ROW_SWAP_OUT));
    CHECK(s.bench[1].id == "hire:1234" && s.bench[1].version == -5 && s.bench[1].hp == 255);
    CHECK(r.off == p.b.size());
}

static void RejectsEveryTruncation() {
    Bytes p = SampleSnapshot();
    for (size_t cut = 0; cut < p.b.size(); ++cut) {
        Reader r{p.b.data(), cut};
        Snapshot s;
        bool ok = ReadSnapshot(r, s);
        if (ok) {
            std::printf("FAIL truncated at %u parsed\n", static_cast<unsigned int>(cut));
            ++g_failures;
        }
    }
}

static void RejectsAbsurdCounts() {
    Bytes p;
    p.i32(1); p.u8(1); p.u8(6); p.u8(5); p.str(""); p.u8(0);
    p.u8(200); // party rows: more than any party
    Reader r{p.b.data(), p.b.size()};
    Snapshot s;
    CHECK(!ReadSnapshot(r, s));
}

static void StringLengthPastTheEnd() {
    Bytes p;
    p.u16(0x7FFF); // a string that claims more than the packet holds
    p.u8('a');
    Reader r{p.b.data(), p.b.size()};
    std::string s = r.str();
    CHECK(!r.ok);
    CHECK(s.empty());
}

static Bytes SamplePage() {
    Bytes p;
    p.i32(4207);       // stamp
    p.u8(PAGE_MENU);
    p.u8(0);           // flags
    p.str("#eThe crew#n\r\n");
    p.u16(3);
    p.u8(1); p.i32(4207000); p.str("Formation...");
    p.u8(0); p.i32(0); p.str("\r\n(after the run)");
    p.u8(1); p.i32(4207998); p.str("Back");
    return p;
}

static void ParsesAPage() {
    Bytes p = SamplePage();
    Reader r{p.b.data(), p.b.size()};
    Page pg;
    CHECK(ReadPage(r, pg));
    CHECK(pg.valid && pg.stamp == 4207 && pg.kind == PAGE_MENU && pg.flags == 0);
    CHECK(pg.text == L"#eThe crew#n\r\n");
    CHECK(pg.entries.size() == 3);
    CHECK(pg.entries[0].option && pg.entries[0].selection == 4207000 && pg.entries[0].label == L"Formation...");
    CHECK(!pg.entries[1].option && pg.entries[1].label == L"\r\n(after the run)");
    CHECK(pg.entries[2].option && pg.entries[2].selection == 4207998);
    CHECK(r.off == p.b.size());
    for (size_t cut = 0; cut < p.b.size(); ++cut) {
        Reader t{p.b.data(), cut};
        Page q;
        if (ReadPage(t, q)) {
            std::printf("FAIL page truncated at %u parsed\n", static_cast<unsigned int>(cut));
            ++g_failures;
        }
    }
}

static void RejectsBadPages() {
    Bytes kind; // an unknown kind
    kind.i32(1); kind.u8(9); kind.u8(0); kind.str("x"); kind.u16(0);
    Reader r1{kind.b.data(), kind.b.size()};
    Page p1;
    CHECK(!ReadPage(r1, p1));
    Bytes many; // more entries than any page
    many.i32(1); many.u8(PAGE_MENU); many.u8(0); many.str(""); many.u16(500);
    Reader r2{many.b.data(), many.b.size()};
    Page p2;
    CHECK(!ReadPage(r2, p2));
    Bytes type; // an entry type that is neither heading nor option
    type.i32(1); type.u8(PAGE_MENU); type.u8(0); type.str(""); type.u16(1); type.u8(7); type.i32(0); type.str("x");
    Reader r3{type.b.data(), type.b.size()};
    Page p3;
    CHECK(!ReadPage(r3, p3));
}

static void ReadsMarkup() {
    std::vector<Span> s = ParseMarkup(L"The lineups I keep:\r\n#eHunting#n: Halvard, #bCasse#k and #r50%#k");
    CHECK(s.size() == 7);
    CHECK(s[0].text == L"The lineups I keep:" && !s[0].bold && s[0].ink == INK_BLACK);
    CHECK(s[1].lineBreak);
    CHECK(s[2].text == L"Hunting" && s[2].bold);
    CHECK(s[3].text == L": Halvard, " && !s[3].bold);
    CHECK(s[4].text == L"Casse" && s[4].ink == INK_BLUE);
    CHECK(s[5].text == L" and " && s[5].ink == INK_BLACK);
    CHECK(s[6].text == L"50%" && s[6].ink == INK_RED);
    // codes with arguments vanish with them; an unknown code and a lone '#' stay as text
    std::vector<Span> t = ParseMarkup(L"Take #v2000000# and #L3#this#l, #h #! 100# #q");
    std::wstring all;
    for (auto& sp : t) all += sp.text;
    CHECK(all == L"Take  and this, ! 100# ");
    std::vector<Span> u = ParseMarkup(L"a\r\n\r\nb");
    CHECK(u.size() == 4 && u[1].lineBreak && u[2].lineBreak && u[3].text == L"b");
}

static int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Optional: a file of hex bytes (the body after the 2-byte opcode, starting at the type byte) from the server.
static void ParsesServerGolden(const char* path) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) {
        std::printf("skip golden: %s not found\n", path);
        return;
    }
    std::vector<unsigned char> b;
    int hi = -1;
    for (int c; (c = std::fgetc(fp)) != EOF;) {
        int h = Hex(static_cast<char>(c));
        if (h < 0) continue;
        if (hi < 0) { hi = h; } else { b.push_back(static_cast<unsigned char>(hi * 16 + h)); hi = -1; }
    }
    std::fclose(fp);
    Reader r{b.data(), b.size()};
    CHECK(r.u8() == SV_SNAPSHOT);
    Snapshot s;
    CHECK(ReadSnapshot(r, s));
    CHECK(r.off == b.size());
    std::printf("golden: %u bytes, %u party rows, %u bench rows\n", static_cast<unsigned int>(b.size()),
                static_cast<unsigned int>(s.party.size()), static_cast<unsigned int>(s.bench.size()));
    for (auto& row : s.party) std::printf("  party %s v%d hp%d flags%d '%ls' '%ls'\n", row.id.c_str(), row.version, row.hp, row.flags, row.levelJob.c_str(), row.detail.c_str());
    for (auto& row : s.bench) std::printf("  bench %s v%d hp%d flags%d '%ls' '%ls'\n", row.id.c_str(), row.version, row.hp, row.flags, row.levelJob.c_str(), row.detail.c_str());
}

int main(int argc, char** argv) {
    ParsesASnapshot();
    RejectsEveryTruncation();
    RejectsAbsurdCounts();
    StringLengthPastTheEnd();
    ParsesAPage();
    RejectsBadPages();
    ReadsMarkup();
    if (argc > 1) ParsesServerGolden(argv[1]);
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
