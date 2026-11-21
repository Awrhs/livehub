#!/usr/bin/env python3
"""慢观众：极小接收缓冲 + 从不读取，迫使服务端积压，用于验证下行流控。
用法：slow_viewer.py <host> <port> <path> <seconds>
"""
import socket
import sys
import time

host, port, path, secs = sys.argv[1], int(sys.argv[2]), sys.argv[3], float(sys.argv[4])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)  # 必须在 connect 前设置
s.connect((host, port))
s.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
time.sleep(1)
s.setblocking(False)
try:
    head = s.recv(12, socket.MSG_PEEK)
except BlockingIOError:
    head = b""
if not head.startswith(b"HTTP/1.1 200"):
    print("bad_response:" + head.decode(errors="replace"))
    sys.exit(0)
deadline = time.time() + secs
closed_by_server = False
while time.time() < deadline:
    time.sleep(1)
    # 不读取数据；只探测连接是否已被服务端关闭（MSG_PEEK 不消费）
    try:
        s.setblocking(False)
        d = s.recv(1, socket.MSG_PEEK)
        if d == b"":
            closed_by_server = True
            break
    except BlockingIOError:
        pass
    except OSError:
        closed_by_server = True
        break
print("closed_by_server" if closed_by_server else "still_open")
