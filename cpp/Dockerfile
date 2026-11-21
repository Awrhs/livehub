# syntax=docker/dockerfile:1.6
# ---- builder ----
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake \
        libssl-dev \
        libavcodec-dev libavformat-dev libavutil-dev \
        libswscale-dev libswresample-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_TESTS=OFF \
        -DENABLE_BENCHMARKS=OFF \
    && cmake --build build -j"$(nproc)"

# ---- runtime ----
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 ffmpeg \
        libavcodec60 libavformat60 libavutil58 \
        libswscale7 libswresample4 \
        ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r stream && useradd -r -g stream -d /var/lib/stream stream

WORKDIR /opt/streaming
COPY --from=builder /src/build/bin/streaming_server /opt/streaming/streaming_server
COPY streaming.conf.example /opt/streaming/streaming.conf.example

# HLS / DVR 输出目录
RUN mkdir -p /var/lib/stream/hls /var/lib/stream/dvr \
    && chown -R stream:stream /var/lib/stream

USER stream
# 工作目录须对运行用户可写（DVR 等相对路径默认落在此处）
WORKDIR /var/lib/stream

# 8080 HTTP/HLS/FLV/API, 1935 RTMP, 8554 RTSP, 19535 SRT/UDP,
# 50000-50999 WebRTC UDP
EXPOSE 8080 1935 8554 19535/udp 50000-50999/udp

# STREAM_* 环境变量可覆盖配置（见 Config::loadFromEnv）；不传则用编译默认值
# 生产环境务必通过 -e 传入 STREAM_JWT_SECRET 与 STREAM_ADMIN_TOKEN
ENV STREAM_IO_THREADS=4 \
    STREAM_HLS_OUTPUT_DIR=/var/lib/stream/hls

ENTRYPOINT ["/usr/bin/tini", "--"]
# 默认仅用环境变量配置；如需配置文件：挂载后把路径作为参数传入
CMD ["/opt/streaming/streaming_server"]
