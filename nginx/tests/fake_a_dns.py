#!/usr/bin/env python3
"""A DNS server that answers every A query with 127.0.0.1, for intercept_test.sh.

    fake_a_dns.py <port>
"""
import socket
import sys

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", int(sys.argv[1])))

while True:
    q, addr = sock.recvfrom(512)
    if len(q) < 12:
        continue
    end = 12
    while end < len(q) and q[end] != 0:
        end += q[end] + 1
    question = q[12:end + 5]
    qtype = int.from_bytes(q[end + 1:end + 3], "big")
    flags = b"\x81\x80"
    if qtype == 1:
        answer = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + socket.inet_aton("127.0.0.1")
        counts = b"\x00\x01\x00\x01\x00\x00\x00\x00"
    else:
        answer = b""
        counts = b"\x00\x01\x00\x00\x00\x00\x00\x00"
    sock.sendto(q[:2] + flags + counts + question + answer, addr)
