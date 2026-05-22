FROM python:3.11-slim

LABEL maintainer="IPTV Streaming Cluster"

RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

RUN mkdir -p logs /mnt/hls /mnt/vod /mnt/timeshift

EXPOSE 8000

CMD ["gunicorn", "-w", "4", "-b", "0.0.0.0:8000", "-t", "120", "--worker-class", "gevent", "src.app:init_app()", "--access-logfile", "-", "--error-logfile", "-"]
