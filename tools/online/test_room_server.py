# SPDX-License-Identifier: GPL-3.0-or-later
"""Protocol test and fuzz test of ldn_room_server.py (the fixture: a server on 127.0.0.1, started by this test).

    python test_room_server.py [--fuzz N]          default N = 20000 random frames and datagrams

Checks: join and the WELCOME fields; rooms isolated by password; LDN broadcast and unicast relay; UDP relay with the
source address the server saw; a member with another build fingerprint refused; malformed hellos refused; leave tells
the others. Then N random TCP frames (any type, any length up to 64 KB + 1, inside and before a join) and N random
UDP datagrams (valid and invalid tokens), after which the server must still accept a join and relay as before.
"""
import os
import random
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FP_A, FP_B = b'\x11' * 8, b'\x22' * 8


def frame(t, p=b''):
    return struct.pack('<IB', len(p) + 1, t) + p


def recv_frame(s):
    h = b''
    while len(h) < 5:
        c = s.recv(5 - len(h))
        if not c:
            raise ConnectionError('closed')
        h += c
    n, t = struct.unpack('<IB', h)
    p = b''
    while len(p) < n - 1:
        p += s.recv(n - 1 - len(p))
    return t, p


def hello(room, name, fp, version=2):
    r, n = room.encode(), name.encode()
    return frame(1, bytes([version, len(r)]) + r + bytes([len(n)]) + n + fp)


def join(port, room, name, fp=FP_A):
    s = socket.create_connection(('127.0.0.1', port), timeout=3)
    s.sendall(hello(room, name, fp))
    t, p = recv_frame(s)
    if t != 2:
        return s, None, p
    vip, token, mask, count = struct.unpack('<IIII', p)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.connect(('127.0.0.1', port))
    u.settimeout(2)
    u.send(b'\x01' + struct.pack('<I', token))
    assert u.recv(100) == b'\x03', 'no REGOK'
    return s, (u, vip, token, count), None


def check(cond, text):
    print('  %-62s %s' % (text, 'ok' if cond else 'FAILED'))
    if not cond:
        sys.exit(1)


def main():
    fuzz = int(sys.argv[sys.argv.index('--fuzz') + 1]) if '--fuzz' in sys.argv else 20000
    port = random.randint(40000, 50000)
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'ldn_room_server.py'), '--port', str(port), '--bind', '127.0.0.1'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        sa, a, _ = join(port, 'pw1', 'A')
        sb, b, _ = join(port, 'pw1', 'B')
        sc, c, _ = join(port, 'pw2', 'C', FP_B)
        check(a and b and c and a[1] == 0xa9fe0101 and b[1] == 0xa9fe0102 and c[1] == 0xa9fe0101 and b[3] == 2,
              'join: virtual addresses per room, member count')
        _, d, why = join(port, 'pw1', 'D', FP_B)
        check(d is None and b'build differs' in why, 'another build fingerprint refused')
        for bad, what in ((frame(1, b''), 'empty'), (hello('pw1', 'E', FP_A, version=1), 'old protocol'),
                          (frame(1, bytes([2, 200]) + b'x' * 10), 'room length past the end'),
                          (frame(1, bytes([2, 3]) + b'pw1' + bytes([40]) + b'n' * 40 + FP_A), 'nickname over 32 bytes'),
                          (frame(1, bytes([2, 3]) + b'pw1' + bytes([2]) + b'\x01x' + FP_A), 'control character'),
                          (frame(1, bytes([2, 3]) + b'pw1' + bytes([1]) + b'n'), 'no fingerprint')):
            s = socket.create_connection(('127.0.0.1', port), timeout=3)
            s.sendall(bad)
            t, p = recv_frame(s)
            check(t == 4, 'malformed hello refused: ' + what)
            s.close()
        sa.sendall(frame(10, struct.pack('<IB', 0xFFFFFFFF, 1) + b'scan'))
        t, p = recv_frame(sb)
        check(t == 10 and p == struct.pack('<IB', a[1], 1) + b'scan', 'LDN broadcast reaches the room, with the sender address')
        sc.settimeout(0.5)
        try:
            recv_frame(sc)
            isolated = False
        except socket.timeout:
            isolated = True
        check(isolated, 'other room does not see it')
        sb.sendall(frame(10, struct.pack('<IB', a[1], 0xFF)))      # a forged "member left" is not relayed
        sa.settimeout(0.5)
        try:
            recv_frame(sa)
            forged = True
        except socket.timeout:
            forged = False
        check(not forged, 'forged member-left message dropped')
        b[0].send(b'\x02' + struct.pack('<IIHH', b[2], a[1], 1234, 5555) + b'hello')
        dgram = a[0].recv(2000)
        k, src, sport, dst, dport = struct.unpack_from('<BIHIH', dgram)
        check(k == 2 and src == b[1] and sport == 5555 and dport == 1234 and dgram[13:] == b'hello', 'UDP relay with the source the server saw')

        # ---- fuzz
        rnd = random.Random(1)
        fz = socket.create_connection(('127.0.0.1', port), timeout=3)
        fz.sendall(hello('pw3', 'F', FP_A))
        recv_frame(fz)
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        closed = 0
        for i in range(fuzz):
            n = rnd.choice([0, 1, 4, 5, 6, 13, 100, 0x480, 0x500, 0x501, rnd.randint(0, 70000)])
            body = bytes(rnd.getrandbits(8) for _ in range(min(n, 2048))) + b'\0' * max(0, n - 2048)
            t = rnd.choice([1, 2, 3, 4, 5, 10, 10, 10, rnd.randint(0, 255)])
            length = rnd.choice([len(body) + 1, rnd.randint(0, 0x20000)])
            try:
                fz.sendall(struct.pack('<IB', length, t) + body)
            except OSError:
                closed += 1
                fz = socket.create_connection(('127.0.0.1', port), timeout=3)
                if rnd.random() < 0.5:
                    fz.sendall(hello('pw3', 'F', FP_A))
            tok = rnd.choice([a[2], b[2], rnd.getrandbits(32)])
            u.sendto(bytes([rnd.choice([1, 2, 3, rnd.randint(0, 255)])]) + struct.pack('<I', tok) +
                     bytes(rnd.getrandbits(8) for _ in range(rnd.choice([0, 3, 8, 64, 1400]))), ('127.0.0.1', port))
        print('  fuzz: %d frames (%d connections closed by the server), %d datagrams' % (fuzz, closed, fuzz))
        check(srv.poll() is None, 'server still running after the fuzz')
        a[0].settimeout(0.2)
        try:
            while True:
                a[0].recv(70000)                     # datagrams the fuzz sent with A's or B's token
        except socket.timeout:
            pass
        a[0].send(b'\x01' + struct.pack('<I', a[2]))   # the fuzz sent datagrams with A's token from this IP: A registers again
        a[0].recv(100)
        se, e, _ = join(port, 'pw1', 'E')
        check(e is not None and e[1] == 0xa9fe0103, 'a new member can still join')
        e[0].send(b'\x02' + struct.pack('<IIHH', e[2], a[1], 99, 77) + b'after')
        dgram = a[0].recv(2000)
        check(dgram[13:] == b'after', 'and the relay still works')
        sb.close()
        sa.settimeout(3)
        t, p = recv_frame(sa)
        while t == 10 and p[4] != 0xFF:
            t, p = recv_frame(sa)
        check(t == 10 and p == struct.pack('<IB', b[1], 0xFF), 'leave: the others are told')
        print('ALL OK')
    finally:
        srv.kill()


if __name__ == '__main__':
    main()
