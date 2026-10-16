"""Room server for nn::ldn over the internet (runtime/ldn_online.inc is the client).

    python3 ldn_room_server.py [--port 39010] [--bind 0.0.0.0]

Players who give the same room password are put in the same room. Each one gets a virtual address 169.254.1.N; the
server relays two things between the members of a room and never looks inside them:
  - LDN messages (scan, scan reply, connect, disconnect, network sync, destroy) over TCP, by destination address or to
    every other member (broadcast);
  - the game's UDP datagrams addressed to virtual addresses, over UDP.
No game data and no account information passes through here other than what the game itself sends to the other players.

TCP frame: u32 length (of what follows, little endian), u8 type, payload.
  client -> server  HELLO  1: u8 version, u8 len + room password (utf-8), u8 len + player name
  server -> client  WELCOME 2: u32 vip, u32 token, u32 mask, u32 member count      (vip / mask as host-order integers)
  server -> client  REFUSED 4: u8 len + reason
  both              LDN   10: u32 vip (client->server: destination, 0xffffffff = every other member;
                               server->client: source), u8 ldn message type, message bytes
  both              PING  3 / PONG 5: no payload
UDP datagram:
  client -> server  REG  1: u32 token                                           (also the keep-alive, every 2 s)
  client -> server  DATA 2: u32 token, u32 dst vip, u16 dst port, u16 src port, payload
  server -> client  DATA 2: u32 src vip, u16 src port, u32 dst vip, u16 dst port, payload
  server -> client  REGOK 3: no payload
A destination of x.x.x.255 or 255.255.255.255 goes to every other member of the room.
"""
import argparse
import asyncio
import hashlib
import logging
import os
import struct
import time

VERSION = 1
NET = (169 << 24) | (254 << 16) | (1 << 8)          # 169.254.1.0/24
MASK = 0xFFFFFF00
BROADCAST = NET | 0xFF
MAX_MEMBERS = 8
MAX_FRAME = 64 * 1024

log = logging.getLogger('ldn')


def ip_str(v):
    return '%d.%d.%d.%d' % (v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255)


class Member:
    def __init__(self, room, vip, token, name, writer):
        self.room, self.vip, self.token, self.name, self.writer = room, vip, token, name, writer
        self.udp_addr = None
        self.last_udp = 0.0

    def send(self, ftype, payload=b''):
        if self.writer.is_closing():
            return
        self.writer.write(struct.pack('<IB', len(payload) + 1, ftype) + payload)


class Server:
    def __init__(self):
        self.rooms = {}            # room key -> {vip: Member}
        self.tokens = {}           # token -> Member
        self.udp = None
        self.stats = {'udp_in': 0, 'udp_out': 0, 'ldn': 0}

    # ---------------------------------------------------------------- TCP: members and LDN messages
    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername')
        member = None
        try:
            while True:
                head = await reader.readexactly(5)
                length, ftype = struct.unpack('<IB', head)
                if length < 1 or length > MAX_FRAME:
                    break
                payload = await reader.readexactly(length - 1)
                if member is None:
                    if ftype != 1:
                        break
                    member = self.join(payload, writer, peer)
                    if member is None:
                        break
                    continue
                if ftype == 10:
                    self.relay_ldn(member, payload)
                elif ftype == 3:
                    member.send(5)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            if member:
                self.leave(member)
            writer.close()

    def join(self, payload, writer, peer):
        try:
            version = payload[0]
            n = payload[1]
            room_pw = payload[2:2 + n].decode('utf-8', 'replace')
            m = payload[2 + n]
            name = payload[3 + n:3 + n + m].decode('utf-8', 'replace')
        except IndexError:
            return None
        if version != VERSION:
            reason = b'server version %d, client version %d' % (VERSION, version)
            writer.write(struct.pack('<IB', len(reason) + 2, 4) + bytes([len(reason)]) + reason)
            return None
        key = hashlib.sha256(room_pw.encode()).hexdigest()
        room = self.rooms.setdefault(key, {})
        vip = next((NET | i for i in range(1, 255) if (NET | i) not in room), None)
        if vip is None or len(room) >= MAX_MEMBERS:
            reason = b'room full'
            writer.write(struct.pack('<IB', len(reason) + 2, 4) + bytes([len(reason)]) + reason)
            return None
        token = struct.unpack('<I', os.urandom(4))[0] or 1
        member = Member(key, vip, token, name, writer)
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
        log.info('leave %s (%s) room %s.. now %d member(s)', ip_str(member.vip), member.name, member.room[:8], len(room))
        # tell the others, as a host that vanished would: an LDN "disconnect" of type 0xff carries only the address
        for other in room.values():
            other.send(10, struct.pack('<IB', member.vip, 0xFF))

    def relay_ldn(self, member, payload):
        if len(payload) < 5:
            return
        dst = struct.unpack_from('<I', payload)[0]
        out = struct.pack('<I', member.vip) + payload[4:]
        room = self.rooms.get(member.room, {})
        self.stats['ldn'] += 1
        if dst == 0xFFFFFFFF or dst == BROADCAST:
            for other in room.values():
                if other is not member:
                    other.send(10, out)
        elif dst in room:
            room[dst].send(10, out)

    # ---------------------------------------------------------------- UDP: the game's datagrams
    def datagram(self, data, addr):
        if len(data) < 5:
            return
        kind = data[0]
        token = struct.unpack_from('<I', data, 1)[0]
        member = self.tokens.get(token)
        if member is None:
            return
        member.udp_addr = addr
        member.last_udp = time.monotonic()
        if kind == 1:
            self.udp.sendto(b'\x03', addr)
            return
        if kind != 2 or len(data) < 17:
            return
        self.stats['udp_in'] += 1
        dst, dport, sport = struct.unpack_from('<IHH', data, 5)
        out = b'\x02' + struct.pack('<IHIH', member.vip, sport, dst, dport) + data[13:]
        room = self.rooms.get(member.room, {})
        if (dst & 0xFF) == 0xFF:
            targets = [m for m in room.values() if m is not member]
        else:
            targets = [room[dst]] if dst in room else []
        for t in targets:
            if t.udp_addr:
                self.udp.sendto(out, t.udp_addr)
                self.stats['udp_out'] += 1

    async def report(self):
        while True:
            await asyncio.sleep(60)
            members = sum(len(r) for r in self.rooms.values())
            if members:
                log.info('rooms %d, members %d, udp in %d out %d, ldn %d',
                         len(self.rooms), members, self.stats['udp_in'], self.stats['udp_out'], self.stats['ldn'])


class Udp(asyncio.DatagramProtocol):
    def __init__(self, server):
        self.server = server

    def connection_made(self, transport):
        self.server.udp = transport

    def datagram_received(self, data, addr):
        self.server.datagram(data, addr)


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
