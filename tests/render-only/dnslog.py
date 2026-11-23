#!/usr/bin/env python3
# dnslog.py ADDRESS LOG: a DNS server (UDP and TCP port 53) that logs every
# query ("<qtype> <name>" per line) and answers A queries with ADDRESS,
# everything else with an empty answer.  The container's only resolver.
import socket, struct, sys, threading

ADDR, LOG = sys.argv[1], sys.argv[2]
lock = threading.Lock()

def answer(query):
    if len(query) < 12:
        return None
    qid, flags, qd = struct.unpack('>HHH', query[:6])
    pos, labels = 12, []
    while pos < len(query) and query[pos] != 0:
        n = query[pos]
        labels.append(query[pos + 1:pos + 1 + n].decode('ascii', 'replace'))
        pos += 1 + n
    pos += 1
    if pos + 4 > len(query):
        return None
    qtype = struct.unpack('>H', query[pos:pos + 2])[0]
    question = query[12:pos + 4]
    name = '.'.join(labels)
    with lock, open(LOG, 'a') as f:
        f.write('%d %s\n' % (qtype, name))
    an = b''
    count = 0
    if qtype == 1:
        an = b'\xc0\x0c' + struct.pack('>HHIH', 1, 1, 5, 4) + socket.inet_aton(ADDR)
        count = 1
    return struct.pack('>HHHHHH', qid, 0x8180, 1, count, 0, 0) + question + an

def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((ADDR, 53))
    while True:
        data, peer = s.recvfrom(4096)
        r = answer(data)
        if r:
            s.sendto(r, peer)

def tcp():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((ADDR, 53))
    s.listen(16)
    while True:
        c, _ = s.accept()
        try:
            n = struct.unpack('>H', c.recv(2))[0]
            r = answer(c.recv(n))
            if r:
                c.sendall(struct.pack('>H', len(r)) + r)
        except Exception:
            pass
        c.close()

threading.Thread(target=tcp, daemon=True).start()
udp()
