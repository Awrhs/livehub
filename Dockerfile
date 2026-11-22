# syntax=docker/dockerfile:1.6
# LiveHub 一体化镜像：Go 平台 + 内嵌前端 + C++ 媒体服务器（由平台作为子进程托管）。
#
#   docker run -d -p 8000:8000 -p 1935:1935 -p 10080:10080/udp -v livehub:/data ghcr.io/awrhs/livehub
#
# 构建：docker build -t livehub --build-arg VERSION=v1.0.0 .

# ---- 媒体服务器 ----
FROM ubuntu:24.04 AS media
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
        libssl-dev libsrt-openssl-dev libsrtp2-dev \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY cpp/ ./
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF -DENABLE_BENCHMARKS=OFF -DENABLE_TOOLS=OFF \
    && cmake --build build --target streaming_server -j"$(nproc)"

# ---- 前端 ----
FROM node:22-bookworm-slim AS web
WORKDIR /web
COPY web/package.json web/package-lock.json* ./
RUN if [ -f package-lock.json ]; then npm ci --no-audit --no-fund; else npm install --no-audit --no-fund; fi
COPY web/ ./
RUN npm run build

# ---- 平台 ----
FROM golang:1.23-bookworm AS platform
ARG VERSION=dev
WORKDIR /src
COPY platform/go.mod platform/go.sum* ./
RUN go mod download
COPY platform/ ./
COPY --from=web /web/dist ./internal/webui/dist
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=${VERSION}" -o /out/livehub ./cmd/livehub

# ---- 运行时 ----
FROM ubuntu:24.04
LABEL org.opencontainers.image.source="https://github.com/Awrhs/streaming_media_server" \
      org.opencontainers.image.title="LiveHub" \
      org.opencontainers.image.description="社区自部署的多主播直播平台" \
      org.opencontainers.image.licenses="AGPL-3.0-only"
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 libsrt1.5-openssl libsrtp2-1 ffmpeg ca-certificates tini curl \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r livehub && useradd -r -g livehub -d /data livehub \
    && mkdir -p /data && chown livehub:livehub /data

COPY --from=media /src/build/bin/streaming_server /opt/livehub/streaming_server
COPY --from=platform /out/livehub /opt/livehub/livehub

ENV LIVEHUB_ADDR=:8000 \
    LIVEHUB_DATA_DIR=/data \
    LIVEHUB_MEDIA_BIN=/opt/livehub/streaming_server \
    LIVEHUB_MEDIA_RTMP_PORT=1935 \
    LIVEHUB_MEDIA_SRT_PORT=10080 \
    LIVEHUB_MEDIA_HTTP_PORT=8081

USER livehub
VOLUME /data
# 8000 网页与播放，1935 RTMP 推流，10080/udp SRT 推流
EXPOSE 8000 1935 10080/udp
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s CMD curl -fsS http://127.0.0.1:8000/healthz || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/livehub/livehub"]
