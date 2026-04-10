FROM ubuntu:22.04

LABEL maintainer="IPTV Streaming Cluster"

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    ffmpeg \
    nginx \
    python3 \
    python3-pip \
    git \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

RUN ln -s /usr/bin/python3 /usr/bin/python

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

RUN mkdir -p logs /mnt/hls /mnt/vod /mnt/timeshift

EXPOSE 1935 1936 8080 8000 1985

CMD ["./scripts/start.sh"]
