#!/usr/bin/env python3
"""最小 WebSocket-FLV 客户端：握手、接收二进制帧、校验 FLV 并统计 tag。

用法：ws_flv_client.py <host> <port> <path> <seconds>
输出：status=<101|...> flv_header=<0|1> video_tags=N audio_tags=N bytes=N
"""
import base64
import os
import socket
import struct
import sys
import time

host, port, path, secs = sys.argv[1], int(sys.argv[2]), sys.argv[3], float(sys.argv[4])
key = base64.b64encode(os.urandom(16)).decode()
s = socket.create_connection((host, port), timeout=10)
s.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
           f"Sec-WebSocket-Version: 13\r\n\r\n").encode())

buf = b""
while b"\r\n\r\n" not in buf:
    d = s.recv(4096)
    if not d:
        break
    buf += d
head, _, buf = buf.partition(b"\r\n\r\n")
status = head.split(b" ")[1].decode() if b" " in head else "?"
if status != "101":
    print(f"status={status} flv_header=0 video_tags=0 audio_tags=0 bytes=0")
    sys.exit(0)

flv = bytearray()
deadline = time.time() + secs
s.settimeout(1)


def need(n):
    global buf
    while len(buf) < n:
        try:
            d = s.recv(65536)
        except socket.timeout:
            if time.time() > deadline:
                return False
            continue
        if not d:
            return False
        buf += d
    return True


while time.time() < deadline:
    if not need(2):
        break
    b0, b1 = buf[0], buf[1]
    op, ln, off = b0 & 0x0F, b1 & 0x7F, 2
    if ln == 126:
        if not need(4):
            break
        ln = struct.unpack(">H", buf[2:4])[0]
        off = 4
    elif ln == 127:
        if not need(10):
            break
        ln = struct.unpack(">Q", buf[2:10])[0]
        off = 10
    if b1 & 0x80:  # 服务端帧不应带掩码
        off += 4
    if not need(off + ln):
        break
    payload = buf[off:off + ln]
    buf = buf[off + ln:]
    if op in (0x0, 0x2):
        flv += payload
    elif op == 0x8:
        break

hdr_ok = 1 if flv[:3] == b"FLV" else 0
v = a = 0
p = 13  # FLV header(9) + PreviousTagSize0(4)
while hdr_ok and p + 11 <= len(flv):
    t = flv[p]
    size = (flv[p + 1] << 16) | (flv[p + 2] << 8) | flv[p + 3]
    if p + 11 + size + 4 > len(flv):
        break
    if t == 9:
        v += 1
    elif t == 8:
        a += 1
    p += 11 + size + 4
print(f"status={status} flv_header={hdr_ok} video_tags={v} audio_tags={a} bytes={len(flv)}")
