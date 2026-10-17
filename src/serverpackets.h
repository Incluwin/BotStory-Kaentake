#pragma once
#include <cstddef>

class COutPacket;

// The BotStory server's own packets (the Crew window, the feature handshake) go through one CClientSocket::
// ProcessPacket hook, attached by the core module server_packets (feature_list.h), so no two modules hook it.
namespace serverpackets {

using RecvHandler = void (*)(const unsigned char* pBody, size_t uLength);
using FieldHandler = void (*)();

// Packets with this server opcode go to the handler (the body after the opcode) and never reach the game.
void OnRecv(unsigned short uOpcode, RecvHandler pHandler);

// Called after the game handled each SET_FIELD; a module sends its first packet of a connection from here.
void OnSetField(FieldHandler pHandler);

// To the server on the current connection (nothing when there is none).
void Send(COutPacket& packet);

}  // namespace serverpackets
