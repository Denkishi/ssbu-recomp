# SPDX-License-Identifier: GPL-3.0-or-later
"""Room server for local wireless over the internet (runtime/ldn_online.inc is the client). Self-hosted: no default server
exists; players type the host of the one they use.

    python3 ldn_room_server.py [--port 39010] [--bind 0.0.0.0]

Players who give the same room password are put in the same room. Each one gets a virtual address 169.254.1.N; the
server relays two things between the members of a room and never looks inside them:
  - LDN messages (scan, scan reply, connect, disconnect, network sync, destroy) over TCP, by destination address or to
    every other member (broadcast);
  - the game's UDP datagrams addressed to virtual addresses, over UDP.
Both sides must run the same game: the first member's build fingerprint is the room's, and a member with another one is
refused. The log holds peers' addresses (for the operator) but never a room password (only the first bytes of its hash).

TCP frame: u32 length (of what follows, little endian), u8 type, payload. Frames over 64 KB close the connection.
  client -> server  HELLO  1: u8 version (2), u8 len + room password (utf-8, 1..127), u8 len + nickname (utf-8, 1..32),
                              8 bytes build fingerprint
  server -> client  WELCOME 2: u32 vip, u32 token, u32 mask, u32 member count      (vip / mask as host-order integers)
  server -> client  REFUSED 4: u8 len + reason
  both              LDN   10: u32 vip (client->server: destination, 0xffffffff = every other member;
                               server->client: source), u8 ldn message type, message bytes (at most 0x500)
  both              PING  3 / PONG 5: no payload
UDP datagram:
  client -> server  REG  1: u32 token                                           (also the keep-alive, every 2 s)
  client -> server  DATA 2: u32 token, u32 dst vip, u16 dst port, u16 src port, payload
  client -> server  PING 4: u32 token, 8 bytes (the client's clock)            (answered only to the member's own address)
  server -> client  DATA 2: u32 src vip, u16 src port, u32 dst vip, u16 dst port, payload
  server -> client  REGOK 3: no payload
  server -> client  PONG 6: the 8 bytes of the PING
A destination of x.x.x.255 or 255.255.255.255 goes to every other member of the room.
"""
import argparse
import asyncio
import hashlib
import logging
import os
import struct
import time

VERSION = 2
NET = (169 << 24) | (254 << 16) | (1 << 8)          # 169.254.1.0/24
MASK = 0xFFFFFF00
BROADCAST = NET | 0xFF
MAX_MEMBERS = 8
MAX_FRAME = 64 * 1024
MAX_LDN = 0x500                                     # the largest LDN message is a NetworkInfo (0x480)
MAX_DATAGRAM = 0xFFFF - 13

log = logging.getLogger('ldn')


def ip_str(v):
    return '%d.%d.%d.%d' % (v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255)


class Member:
    def __init__(self, room, vip, token, name, writer, peer_ip):
        self.room, self.vip, self.token, self.name, self.writer = room, vip, token, name, writer
        self.peer_ip = peer_ip                          # datagrams count only from the address of the member's TCP connection
        self.udp_addr = None
        self.last_udp = 0.0

    def send(self, ftype, payload=b''):
        if self.writer.is_closing():
            return
        self.writer.write(struct.pack('<IB', len(payload) + 1, ftype) + payload)


def parse_hello(payload):
    """-> (room password, nickname, fingerprint) or a refusal text. Every length is checked against the payload."""
    if len(payload) < 1:
        return 'empty hello'
    if payload[0] != VERSION:
        return 'server protocol %d, client protocol %d: update the game or the server' % (VERSION, payload[0])
    p = 1
    if p >= len(payload):
        return 'malformed hello'
    n = payload[p]; p += 1
    if not 1 <= n <= 127 or p + n > len(payload):
        return 'malformed hello (room password)'
    room = payload[p:p + n]; p += n
    if p >= len(payload):
        return 'malformed hello'
    m = payload[p]; p += 1
    if not 1 <= m <= 32 or p + m + 8 != len(payload):
        return 'malformed hello (nickname 1 to 32 bytes, fingerprint 8 bytes)'
    nick = payload[p:p + m]; p += m
    try:
        room_s, nick_s = room.decode('utf-8'), nick.decode('utf-8')
    except UnicodeDecodeError:
        return 'room password and nickname must be UTF-8'
    if any(ord(c) < 32 or ord(c) == 127 for c in nick_s):
        return 'nickname with control characters'
    return room_s, nick_s, bytes(payload[p:p + 8])


class Server:
    def __init__(self):
        self.rooms = {}            # room key -> {vip: Member}
        self.prints = {}           # room key -> build fingerprint of the room
        self.tokens = {}           # token -> Member
        self.udp = None
        self.stats = {'udp_in': 0, 'udp_out': 0, 'ldn': 0, 'dropped': 0}

    # ---------------------------------------------------------------- TCP: members and LDN messages
    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername')
        member = None
        try:
            while True:
                head = await asyncio.wait_for(reader.readexactly(5), timeout=None if member else 10)
                length, ftype = struct.unpack('<IB', head)
                if length < 1 or length > MAX_FRAME:
                    break
                payload = await reader.readexactly(length - 1)
                if member is None:
                    if ftype != 1:
                        break
                    member = self.join(payload, writer, peer)
                    if member is None:
                        await writer.drain()
                        break
                    continue
                if ftype == 10:
                    self.relay_ldn(member, payload)
                elif ftype == 3:
                    member.send(5)
                await writer.drain()
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, OSError):
            pass
        finally:
            if member:
                self.leave(member)
            writer.close()

    def refuse(self, writer, reason, peer):
        r = reason.encode()[:200]
        writer.write(struct.pack('<IB', len(r) + 2, 4) + bytes([len(r)]) + r)
        log.info('refused %s: %s', peer, reason)

    def join(self, payload, writer, peer):
        hello = parse_hello(payload)
        if isinstance(hello, str):
            self.refuse(writer, hello, peer)
            return None
        room_pw, name, fingerprint = hello
        key = hashlib.sha256(room_pw.encode()).hexdigest()
        room = self.rooms.setdefault(key, {})
        if room and self.prints.get(key) != fingerprint:
            self.refuse(writer, 'your game build differs from the room\'s (game version, game DLL, mods, fighter source or '
                                'new fighters): both sides must run the same game', peer)
            return None
        vip = next((NET | i for i in range(1, 255) if (NET | i) not in room), None)
        if vip is None or len(room) >= MAX_MEMBERS:
            self.refuse(writer, 'room full', peer)
            return None
        if not room:
            self.prints[key] = fingerprint
        token = struct.unpack('<I', os.urandom(4))[0] or 1
        member = Member(key, vip, token, name, writer, peer[0] if peer else None)
        room[vip] = member
        self.tokens[token] = member
        member.send(2, struct.pack('<IIII', vip, token, MASK, len(room)))
        log.info('join  %s as %s (%s) room %s.. now %d member(s)', peer, ip_str(vip), name, key[:8], len(room))
        return member

    def leave(self, member):
        room = self.rooms.get(member.room, {})
        room.pop(member.vip, None)
        self.tokens.pop(member.token, None)
        if not room:
            self.rooms.pop(member.room, None)
            self.prints.pop(member.room, None)
        log.info('leave %s (%s) room %s.. now %d member(s)', ip_str(member.vip), member.name, member.room[:8], len(room))
        # tell the others, as a host that vanished would: an LDN message of type 0xff carries only the address
        for other in room.values():
            other.send(10, struct.pack('<IB', member.vip, 0xFF))

    def relay_ldn(self, member, payload):
        if len(payload) < 5 or len(payload) > 5 + MAX_LDN or payload[4] == 0xFF:
            self.stats['dropped'] += 1
            return
        dst = struct.unpack_from('<I', payload)[0]
        out = struct.pack('<I', member.vip) + payload[4:]
        room = self.rooms.get(member.room, {})
        self.stats['ldn'] += 1
        if dst == 0xFFFFFFFF or dst == BROADCAST:
            for other in room.values():
                if other is not member:
                    other.send(10, out)
        elif dst in room and dst != member.vip:
            room[dst].send(10, out)

    # ---------------------------------------------------------------- UDP: the game's datagrams
    def datagram(self, data, addr):
        if len(data) < 5:
            return
        kind = data[0]
        token = struct.unpack_from('<I', data, 1)[0]
        member = self.tokens.get(token)
        if member is None or addr[0] != member.peer_ip:
            return
        member.udp_addr = addr
        member.last_udp = time.monotonic()
        if kind == 1:
            self.udp.sendto(b'\x03', addr)
            return
        if kind == 4:                                   # ping: echo the client's 8-byte timestamp
            if len(data) == 13:
                self.udp.sendto(b'\x06' + data[5:13], addr)
            return
        if kind != 2 or len(data) < 13 or len(data) - 13 > MAX_DATAGRAM:
            self.stats['dropped'] += 1
            return
        self.stats['udp_in'] += 1
        dst, dport, sport = struct.unpack_from('<IHH', data, 5)
        out = b'\x02' + struct.pack('<IHIH', member.vip, sport, dst, dport) + data[13:]
        room = self.rooms.get(member.room, {})
        if (dst & 0xFF) == 0xFF:
            targets = [m for m in room.values() if m is not member]
        else:
            targets = [room[dst]] if dst in room and dst != member.vip else []
        for t in targets:
            if t.udp_addr:
                self.udp.sendto(out, t.udp_addr)
                self.stats['udp_out'] += 1

    async def report(self):
        while True:
            await asyncio.sleep(60)
            members = sum(len(r) for r in self.rooms.values())
            if members:
                log.info('rooms %d, members %d, udp in %d out %d, ldn %d, dropped %d', len(self.rooms), members,
                         self.stats['udp_in'], self.stats['udp_out'], self.stats['ldn'], self.stats['dropped'])


class Udp(asyncio.DatagramProtocol):
    def __init__(self, server):
        self.server = server

    def connection_made(self, transport):
        self.server.udp = transport

    def datagram_received(self, data, addr):
        try:
            self.server.datagram(data, addr)
        except Exception:                               # a malformed datagram must never stop the relay
            log.exception('datagram from %s', addr)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=39010)
    ap.add_argument('--bind', default='0.0.0.0')
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    server = Server()
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: Udp(server), local_addr=(a.bind, a.port))
    tcp = await asyncio.start_server(server.handle, a.bind, a.port)
    log.info('ldn room server on %s:%d (tcp + udp), protocol %d', a.bind, a.port, VERSION)
    asyncio.ensure_future(server.report())
    async with tcp:
        await tcp.serve_forever()


if __name__ == '__main__':
    asyncio.run(main())
