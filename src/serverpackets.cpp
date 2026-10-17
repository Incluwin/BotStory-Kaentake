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

struct Route {
    unsigned short uOpcode;
    serverpackets::RecvHandler pHandler;
};

Route g_aRoutes[8];
size_t g_uRoutes = 0;
serverpackets::FieldHandler g_apOnSetField[8];
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

auto CClientSocket__ProcessPacket = reinterpret_cast<void(__thiscall*)(void*, void*)>(kProcessPacket);

void __fastcall CClientSocket__ProcessPacket_hook(void* pThis, void* /*edx*/, void* pPacket) {
    unsigned short op = 0;
    const unsigned char* body = nullptr;
    size_t len = 0;
    bool peeked = PeekOpcode(pPacket, &op, &body, &len);
    if (peeked) {
        for (size_t i = 0; i < g_uRoutes; ++i) {
            if (g_aRoutes[i].uOpcode == op) {
                g_aRoutes[i].pHandler(body, len);
                return; // never the game's: an unknown opcode is its stage's to ignore, and ours is fully read here
            }
        }
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

void OnRecv(unsigned short uOpcode, RecvHandler pHandler) {
    if (g_uRoutes == sizeof(g_aRoutes) / sizeof(g_aRoutes[0])) {
        log_line("too many server opcodes; one is not read");
        return;
    }
    g_aRoutes[g_uRoutes++] = { uOpcode, pHandler };
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
