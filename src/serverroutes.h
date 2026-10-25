#pragma once
// The route table serverpackets.cpp dispatches the BotStory server's packets with, kept free of the game's code so
// test/serverroutes_test.cpp can check it outside the client.
#include <cstddef>

class CInPacket;

namespace serverroutes {

using RecvHandler = void (*)(const unsigned char* pBody, size_t uLength);
using PacketHandler = void (*)(CInPacket* pPacket);

enum class Kind : unsigned char {
    body,   // the bytes after the opcode; the game never sees the packet
    packet, // the client's own CInPacket, read past the opcode, for modules that decode with the client's decoders;
            // the game never sees the packet
    before, // the bytes after the opcode, then the game handles the packet as usual
};

struct Route {
    unsigned short uOpcode;
    Kind kind;
    RecvHandler pBody;
    PacketHandler pPacket;
};

enum class AddResult { added, full, taken };

template <size_t N>
class Table {
public:
    // A before route may share its opcode with others; only one body or packet route may own an opcode, since the
    // packet is read once and kept from the game.
    AddResult Add(const Route& route) {
        if (route.kind != Kind::before && Owner(route.uOpcode)) {
            return AddResult::taken;
        }
        if (m_uCount == N) {
            return AddResult::full;
        }
        m_aRoutes[m_uCount++] = route;
        return AddResult::added;
    }

    // Runs one packet's routes: every before route in the order added, then the route that owns the opcode.
    // pSkipOpcode moves the packet past its opcode before a packet route reads it. True when a route owned the
    // packet, so the game must not see it.
    bool Dispatch(unsigned short uOpcode, const unsigned char* pBody, size_t uLength, CInPacket* pPacket,
                  void (*pSkipOpcode)(CInPacket*)) const {
        for (size_t i = 0; i < m_uCount; ++i) {
            if (m_aRoutes[i].uOpcode == uOpcode && m_aRoutes[i].kind == Kind::before) {
                m_aRoutes[i].pBody(pBody, uLength);
            }
        }
        const Route* pOwner = Owner(uOpcode);
        if (!pOwner) {
            return false;
        }
        if (pOwner->kind == Kind::packet) {
            pSkipOpcode(pPacket);
            pOwner->pPacket(pPacket);
        } else {
            pOwner->pBody(pBody, uLength);
        }
        return true;
    }

    size_t Count() const {
        return m_uCount;
    }

private:
    const Route* Owner(unsigned short uOpcode) const {
        for (size_t i = 0; i < m_uCount; ++i) {
            if (m_aRoutes[i].uOpcode == uOpcode && m_aRoutes[i].kind != Kind::before) {
                return &m_aRoutes[i];
            }
        }
        return nullptr;
    }

    Route m_aRoutes[N] = {};
    size_t m_uCount = 0;
};

}  // namespace serverroutes
