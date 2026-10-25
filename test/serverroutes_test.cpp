// Checks src/serverroutes.h, the route table the server_packets module dispatches with, outside the game.
// Build and run: test\build_serverroutes.cmd (or, in an x86 Developer prompt,
// cl /nologo /EHsc /std:c++17 /I..\src serverroutes_test.cpp && serverroutes_test.exe).
#include "serverroutes.h"
#include <cstdint>
#include <cstdio>
#include <string>

using namespace serverroutes;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static std::string g_calls; // the handlers that ran, in order
static size_t g_lastLength = 0;
static CInPacket* g_lastPacket = nullptr;

static void BodyA(const unsigned char*, size_t uLength) { g_calls += "A"; g_lastLength = uLength; }
static void BodyB(const unsigned char*, size_t) { g_calls += "B"; }
static void BeforeX(const unsigned char*, size_t) { g_calls += "x"; }
static void BeforeY(const unsigned char*, size_t) { g_calls += "y"; }
static void PacketP(CInPacket* pPacket) { g_calls += "P"; g_lastPacket = pPacket; }
static void Skip(CInPacket*) { g_calls += "s"; }

static Route Body(unsigned short op, RecvHandler h) { return { op, Kind::body, h, nullptr }; }
static Route Before(unsigned short op, RecvHandler h) { return { op, Kind::before, h, nullptr }; }
static Route Packet(unsigned short op, PacketHandler h) { return { op, Kind::packet, nullptr, h }; }

static bool Run(const Table<8>& t, unsigned short op, CInPacket* pPacket = nullptr) {
    g_calls.clear();
    static const unsigned char body[3] = { 1, 2, 3 };
    return t.Dispatch(op, body, sizeof(body), pPacket, Skip);
}

int main() {
    // A body route owns its opcode; other opcodes go to the game.
    {
        Table<8> t;
        CHECK(t.Add(Body(0x17A, BodyA)) == AddResult::added);
        CHECK(Run(t, 0x17A));
        CHECK(g_calls == "A");
        CHECK(g_lastLength == 3);
        CHECK(!Run(t, 0x17B));
        CHECK(g_calls.empty());
    }
    // A packet route moves the packet past its opcode first and gets the same packet.
    {
        Table<8> t;
        CInPacket* fake = reinterpret_cast<CInPacket*>(static_cast<uintptr_t>(0x1234));
        CHECK(t.Add(Packet(0x17E, PacketP)) == AddResult::added);
        CHECK(Run(t, 0x17E, fake));
        CHECK(g_calls == "sP");
        CHECK(g_lastPacket == fake);
    }
    // Before routes run in the order added, then the game (false) when nothing owns the opcode.
    {
        Table<8> t;
        CHECK(t.Add(Before(0x131, BeforeX)) == AddResult::added);
        CHECK(t.Add(Before(0x131, BeforeY)) == AddResult::added);
        CHECK(!Run(t, 0x131));
        CHECK(g_calls == "xy");
    }
    // Before routes still run first when a body route owns the same opcode.
    {
        Table<8> t;
        CHECK(t.Add(Body(0x180, BodyA)) == AddResult::added);
        CHECK(t.Add(Before(0x180, BeforeX)) == AddResult::added);
        CHECK(Run(t, 0x180));
        CHECK(g_calls == "xA");
    }
    // Only one module may own an opcode; the first keeps it.
    {
        Table<8> t;
        CHECK(t.Add(Body(0x17C, BodyA)) == AddResult::added);
        CHECK(t.Add(Body(0x17C, BodyB)) == AddResult::taken);
        CHECK(t.Add(Packet(0x17C, PacketP)) == AddResult::taken);
        CHECK(t.Add(Before(0x17C, BeforeX)) == AddResult::added);
        CHECK(t.Count() == 2);
        CHECK(Run(t, 0x17C));
        CHECK(g_calls == "xA");
    }
    // A full table refuses the next route and keeps the ones it has.
    {
        Table<8> t;
        for (unsigned short op = 0x200; op < 0x208; ++op) {
            CHECK(t.Add(Body(op, BodyA)) == AddResult::added);
        }
        CHECK(t.Add(Body(0x300, BodyB)) == AddResult::full);
        CHECK(t.Count() == 8);
        CHECK(Run(t, 0x207));
        CHECK(g_calls == "A");
        CHECK(!Run(t, 0x300));
    }

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("serverroutes: all checks passed\n");
    return 0;
}
