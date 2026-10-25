#pragma once
#include "serverroutes.h"
#include <cstddef>

class COutPacket;

// The BotStory server's own packets (the Crew window, the feature handshake) go through one CClientSocket::
// ProcessPacket hook, attached by the core module server_packets (feature_list.h), so no two modules hook it.
// Register from a module's attach function; the opcodes are reserved in the server's docs/CUSTOM_OPCODES.md.
namespace serverpackets {

using RecvHandler = serverroutes::RecvHandler;
using PacketHandler = serverroutes::PacketHandler;
using FieldHandler = void (*)();

// Packets with this server opcode go to the handler (the body after the opcode) and never reach the game.
void OnRecv(unsigned short uOpcode, RecvHandler pHandler);

// Like OnRecv, but the handler gets the client's own CInPacket, already read past the opcode, so it can decode with
// the client's decoders (items, avatars). Never reaches the game.
void OnRecvPacket(unsigned short uOpcode, PacketHandler pHandler);

// Sees the body of a packet the game also handles (after this handler returns). Several modules may watch one opcode.
void OnRecvBefore(unsigned short uOpcode, RecvHandler pHandler);

// Called after the game handled each SET_FIELD; a module sends its first packet of a connection from here.
void OnSetField(FieldHandler pHandler);

// To the server on the current connection (nothing when there is none).
void Send(COutPacket& packet);

}  // namespace serverpackets
