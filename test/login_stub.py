# A login server stub for client_exit_test.ps1: sends the v83 handshake so the client reaches its login screen,
# then holds each connection and never answers. Listens on 127.0.0.1:18484 for 30 minutes.
import socket
import struct
import time

HELLO = struct.pack('<HHH', 0x0E, 83, 1) + b'1' + bytes([0x46, 0x72, 0x7A, 0x52, 0x52, 0x30, 0x78, 0x73, 8])

server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', 18484))
server.listen(8)
server.settimeout(1.0)
held = []
end = time.time() + 1800
while time.time() < end:
    try:
        conn, addr = server.accept()
    except socket.timeout:
        continue
    conn.sendall(HELLO)
    held.append(conn)
    print('handshake sent to', addr, flush=True)
