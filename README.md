# Streaming Media Server（C++17）

从零实现的高并发流媒体服务器。**本项目已完全转为 C++ 实现**（历史
Python 原型已移除，如需查阅可从 git 历史 `4bfdbda` 之前找回）。

RTMP（含 H.265/Enhanced RTMP）/ SRT / WebRTC-WHIP 接入 →
HLS / HTTP-FLV / WebSocket-FLV / RTSP / WebRTC-WHEP / SRT 分发，
DVR/fMP4 录制与时移、ABR 转码、vhost 多租户、HTTP webhook、源站-边缘集群。
数据面全部手写（RTMP chunk/AMF0、FLV、HLS TS/PES/m3u8、fMP4、SRT、
STUN/DTLS/SRTP/RTP、RTSP、HTTP）；FFmpeg/libav 与 OpenSSL 为可选依赖，
缺失时自动降级为 stub，可无依赖编译。

完整文档见 **[cpp/README.md](cpp/README.md)**；生产可用性的差距分析、已修复问题与待办见
**[cpp/ROADMAP.md](cpp/ROADMAP.md)**。所有功能在每次推送时由 GitHub Actions 端到端验证
（ASan / TSan / 6 分钟 soak / Docker 冒烟 / ffmpeg 与 aiortc 第三方客户端）。

## 快速开始

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j
./cpp/build/streaming_server          # 读 cpp/streaming.conf.example 的默认配置

# 推流 & 播放
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/live/test
ffplay http://127.0.0.1:8080/live/test.m3u8
ffplay http://127.0.0.1:8080/live/test.flv
```

## 目录结构

```
cpp/            C++17 实现（源码、测试、benchmark、工具、Dockerfile）
.github/        CI（cpp.yml：stub / 全依赖+ASan / clang Release 三矩阵）
```
