#!/usr/bin/env python3
"""HTTP-FLV 观众模拟（基准用）：对一个流打开 M 个连接，持续读取并统计每个连接收到的字节数。

用法：flv_viewers.py <host> <port> <path> <viewers> <seconds>
输出一行 JSON：{"viewers":M,"ok":k,"bytes":总字节,"kbps":[每个观众的平均码率...],"errors":[...]}
"""
import asyncio
import json
import sys
import time


async def viewer(host, port, path, seconds):
    got = 0
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), 5)
        writer.write(f"GET {path} HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
        await writer.drain()
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        status = head.split(b"\r\n", 1)[0].decode(errors="replace")
        if " 200 " not in status:
            writer.close()
            return 0, status
        start = time.monotonic()
        end = start + seconds
        err = None
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            try:
                chunk = await asyncio.wait_for(reader.read(65536), left)
            except asyncio.TimeoutError:
                break
            if not chunk:
                err = "closed by server"
                break
            got += len(chunk)
        writer.close()
        return got, err
    except Exception as e:  # noqa: BLE001
        return got, repr(e)


async def main():
    host, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    n, seconds = int(sys.argv[4]), float(sys.argv[5])
    results = await asyncio.gather(*(viewer(host, port, path, seconds) for _ in range(n)))
    kbps = [round(b * 8 / seconds / 1000, 1) for b, _ in results]
    errors = sorted({e for _, e in results if e})
    print(json.dumps({
        "viewers": n,
        "ok": sum(1 for b, e in results if b > 0 and not e),
        "bytes": sum(b for b, _ in results),
        "kbps": kbps,
        "errors": errors,
    }))


asyncio.run(main())
