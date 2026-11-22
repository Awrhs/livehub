#!/usr/bin/env python3
"""第三方 WebRTC 客户端（aiortc），用于 WHIP 推流 / WHEP 拉流互通测试。

用法：
  webrtc_client.py whip <url> <seconds>   推送 testsrc2 视频（H.264）+ 正弦音频（Opus）
  webrtc_client.py whep <url> <seconds>   拉流并统计收到的视频/音频帧数
输出一行结果，如：
  status=201 state=connected video_frames=120 audio_frames=200
"""
import asyncio
import logging
import os
import sys

# 调试：WEBRTC_DEBUG=1 时输出 aioice 连通性检查细节到 stderr
if os.environ.get("WEBRTC_DEBUG"):
    logging.basicConfig(level=logging.DEBUG, stream=sys.stderr,
                        format="%(asctime)s %(name)s %(message)s")
    for noisy in ("aiortc.rtcrtpreceiver", "asyncio"):
        logging.getLogger(noisy).setLevel(logging.WARNING)

import aiohttp
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.contrib.media import MediaPlayer
from aiortc.rtcrtpsender import RTCRtpSender


def prefer(transceiver, kind, mime):
    caps = RTCRtpSender.getCapabilities(kind)
    codecs = [c for c in caps.codecs if c.mimeType.lower() == mime.lower()]
    if codecs:
        transceiver.setCodecPreferences(codecs)


async def signal(pc, url):
    await pc.setLocalDescription(await pc.createOffer())
    # 等待 ICE 收集完成（非 trickle）
    while pc.iceGatheringState != "complete":
        await asyncio.sleep(0.05)
    async with aiohttp.ClientSession() as s:
        async with s.post(url, data=pc.localDescription.sdp,
                          headers={"Content-Type": "application/sdp"}) as r:
            body = await r.text()
            if r.status not in (200, 201):
                return r.status, body
            await pc.setRemoteDescription(RTCSessionDescription(sdp=body, type="answer"))
            return r.status, ""


async def whip(url, secs):
    pc = RTCPeerConnection()
    v = MediaPlayer("testsrc2=size=640x360:rate=25", format="lavfi")
    a = MediaPlayer("sine=frequency=500:sample_rate=48000", format="lavfi")
    vt = pc.addTransceiver(v.video, direction="sendonly")
    prefer(vt, "video", "video/H264")
    at = pc.addTransceiver(a.audio, direction="sendonly")
    prefer(at, "audio", "audio/opus")
    status, err = await signal(pc, url)
    if err:
        print(f"status={status} state=failed error={err[:120]!r}")
        return
    for _ in range(int(secs * 10)):
        await asyncio.sleep(0.1)
        if pc.connectionState in ("failed", "closed"):
            break
    print(f"status={status} state={pc.connectionState} ice={pc.iceConnectionState}")
    await pc.close()


async def whep(url, secs):
    pc = RTCPeerConnection()
    counts = {"video": 0, "audio": 0}

    @pc.on("track")
    def on_track(track):
        async def reader():
            while True:
                try:
                    await track.recv()
                    counts[track.kind] += 1
                except Exception:
                    return
        asyncio.ensure_future(reader())

    vt = pc.addTransceiver("video", direction="recvonly")
    prefer(vt, "video", "video/H264")
    pc.addTransceiver("audio", direction="recvonly")
    status, err = await signal(pc, url)
    if err:
        print(f"status={status} state=failed video_frames=0 audio_frames=0 error={err[:120]!r}")
        return
    await asyncio.sleep(secs)
    print(f"status={status} state={pc.connectionState} ice={pc.iceConnectionState} "
          f"video_frames={counts['video']} audio_frames={counts['audio']}")
    await pc.close()


if __name__ == "__main__":
    mode, url, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
    try:
        asyncio.run(asyncio.wait_for(whip(url, secs) if mode == "whip" else whep(url, secs),
                                     timeout=secs + 30))
    except Exception as e:  # noqa: BLE001
        print(f"status=0 state=exception error={type(e).__name__}:{str(e)[:150]!r}")
