#include "pch.h"
#include "hook.h"
#include "serverpackets.h"
#include "v83_offsets.h"
#include "v83_version_guard.h"
#include "wvs/packet.h"
#include <cstdio>

// The core module server_packets: one CClientSocket::ProcessPacket hook that hands the BotStory server's own opcodes
// to the module that reads them (crewwnd.cpp, featurehandshake.cpp) and tells them about each SET_FIELD. Every other
// packet goes to the game unchanged.

using namespace v83::crew; // the packet offsets were found for the Crew window

namespace {

serverroutes::Table<32> g_routes;
serverpackets::FieldHandler g_apOnSetField[16];
size_t g_uOnSetField = 0;

void log_line(const char* s) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, "kaentake_diag.txt", "a") == 0 && fp) {
        fprintf(fp, "[server_packets] %s\n", s);
        fclose(fp);
    }
}

// CInPacket as the client lays it out (CInPacket::Decode2 at 0x0042470C reads these fields).
struct InPacketView {
    int bLoopback;
    int nState;
    unsigned char* aRecvBuff;
    unsigned short uLength;
    unsigned short uRawSeq;
    unsigned short uDataLen;
    unsigned short uPad;
    unsigned int uOffset;
};
static_assert(offsetof(InPacketView, aRecvBuff) == 0x8);
static_assert(offsetof(InPacketView, uLength) == 0xC);
static_assert(offsetof(InPacketView, uOffset) == 0x14);

bool PeekOpcode(void* pPacket, unsigned short* pOp, const unsigned char** ppBody, size_t* pBodyLen) {
    __try {
        auto* v = static_cast<InPacketView*>(pPacket);
        if (!v || !v->aRecvBuff || v->uOffset + 2 > v->uLength) return false;
        const unsigned char* data = v->aRecvBuff + v->uOffset;
        *pOp = static_cast<unsigned short>(data[0] | (data[1] << 8));
        *ppBody = data + 2;
        *pBodyLen = static_cast<size_t>(v->uLength - v->uOffset - 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What CInPacket::Decode2 does to the read offset; PeekOpcode has checked the two bytes are there.
void SkipOpcode(CInPacket* pPacket) {
    reinterpret_cast<InPacketView*>(pPacket)->uOffset += 2;
}

auto CClientSocket__ProcessPacket = reinterpret_cast<void(__thiscall*)(void*, void*)>(kProcessPacket);

void __fastcall CClientSocket__ProcessPacket_hook(void* pThis, void* /*edx*/, void* pPacket) {
    unsigned short op = 0;
    const unsigned char* body = nullptr;
    size_t len = 0;
    bool peeked = PeekOpcode(pPacket, &op, &body, &len);
    if (peeked && g_routes.Dispatch(op, body, len, static_cast<CInPacket*>(pPacket), SkipOpcode)) {
        return; // never the game's: an unknown opcode is its stage's to ignore, and ours is fully read here
    }
    CClientSocket__ProcessPacket(pThis, pPacket);
    if (peeked && op == kSetFieldOpcode) {
        for (size_t i = 0; i < g_uOnSetField; ++i) {
            g_apOnSetField[i]();
        }
    }
}

}  // namespace


namespace serverpackets {

static void AddRoute(const serverroutes::Route& route) {
    char line[96] = {};
    switch (g_routes.Add(route)) {
    case serverroutes::AddResult::added:
        return;
    case serverroutes::AddResult::full:
        sprintf_s(line, "too many server opcode routes; 0x%X is not read", route.uOpcode);
        break;
    case serverroutes::AddResult::taken:
        sprintf_s(line, "two modules read opcode 0x%X; the second one is not called", route.uOpcode);
        break;
    }
    log_line(line);
}

void OnRecv(unsigned short uOpcode, RecvHandler pHandler) {
    AddRoute({ uOpcode, serverroutes::Kind::body, pHandler, nullptr });
}

void OnRecvPacket(unsigned short uOpcode, PacketHandler pHandler) {
    AddRoute({ uOpcode, serverroutes::Kind::packet, nullptr, pHandler });
}

void OnRecvBefore(unsigned short uOpcode, RecvHandler pHandler) {
    AddRoute({ uOpcode, serverroutes::Kind::before, pHandler, nullptr });
}

void OnSetField(FieldHandler pHandler) {
    if (g_uOnSetField == sizeof(g_apOnSetField) / sizeof(g_apOnSetField[0])) {
        log_line("too many SET_FIELD listeners; one is not called");
        return;
    }
    g_apOnSetField[g_uOnSetField++] = pHandler;
}

void Send(COutPacket& packet) {
    void* pSocket = *reinterpret_cast<void**>(kClientSocketInstance);
    if (pSocket) {
        reinterpret_cast<void(__thiscall*)(void*, COutPacket*)>(kSendPacket)(pSocket, &packet);
    }
}

}  // namespace serverpackets


void AttachServerPackets() {
    if (!v83::CheckClientVersion()) {
        log_line("off: MapleStory.exe is not the build these offsets come from");
        return;
    }
    ATTACH_HOOK(CClientSocket__ProcessPacket, CClientSocket__ProcessPacket_hook);
    AttachFeatureHandshake();
}
